#include "client.h"

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/sys.h"

static int connect_instance(const char *name)
{
    char path[CG_PATH_MAX];
    if (cg_instance_path(name, "control.sock", path, sizeof(path)) != 0)
        return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path))
        return -1;
    strcpy(sa.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_line(int fd, const char *line)
{
    size_t n = strlen(line);
    while (n) {
        ssize_t w = write(fd, line, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        line += w;
        n -= (size_t)w;
    }
    return write(fd, "\n", 1) == 1 ? 0 : -1;
}

int cg_ctl_request(const char *name, const char *line, cg_buf_t *reply, int timeout_ms)
{
    int fd = connect_instance(name);
    if (fd < 0)
        return -1;
    if (send_line(fd, line) != 0) {
        close(fd);
        return -1;
    }
    uint64_t deadline = cg_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        uint64_t now = cg_now_ms();
        if (now >= deadline) {
            close(fd);
            return -1;
        }
        struct pollfd p = {fd, POLLIN, 0};
        int rc = poll(&p, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc <= 0) {
            close(fd);
            return -1;
        }
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        cg_buf_append(reply, buf, (size_t)n);
    }
    close(fd);
    return 0;
}

int cg_ctl_stream(const char *name, const char *line,
                  int (*fn)(void *ctx, const char *data, size_t n), void *ctx)
{
    int fd = connect_instance(name);
    if (fd < 0)
        return -1;
    if (send_line(fd, line) != 0) {
        close(fd);
        return -1;
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0 || fn(ctx, buf, (size_t)n) != 0)
            break;
    }
    close(fd);
    return 0;
}

static int alive(const char *name)
{
    int fd = connect_instance(name);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int cg_list_instances(cg_buf_t *out)
{
    char root[CG_PATH_MAX];
    if (cg_runtime_dir(root, sizeof(root)) != 0)
        return -1;
    DIR *d = opendir(root);
    if (!d)
        return 0;
    int count = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!cg_valid_name(de->d_name) || !alive(de->d_name))
            continue;
        char path[CG_PATH_MAX];
        cg_instance_path(de->d_name, "control.sock", path, sizeof(path));
        cg_buf_printf(out, "%s\t%s\n", de->d_name, path);
        count++;
    }
    closedir(d);
    return count;
}

int cg_pick_instance(const char *given, char *out, size_t n)
{
    if (!given || !*given)
        given = getenv("COGDB_INSTANCE");
    if (given && *given) {
        if (!cg_valid_name(given))
            return -1;
        snprintf(out, n, "%s", given);
        return 0;
    }
    cg_buf_t list;
    cg_buf_init(&list);
    int count = cg_list_instances(&list);
    int rc = -1;
    if (count == 1) {
        size_t k = strcspn(list.data, "\t");
        snprintf(out, n, "%.*s", (int)k, list.data);
        rc = 0;
    }
    cg_buf_free(&list);
    return rc;
}
