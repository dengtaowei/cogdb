#include "sys.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t cg_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t cg_now_ms(void)
{
    return cg_now_ns() / 1000000ull;
}

int cg_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -1;
    if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int cg_valid_name(const char *name)
{
    if (!name || !name[0] || name[0] == '.')
        return 0;
    size_t n = strlen(name);
    if (n >= CG_NAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

int cg_runtime_dir(char *out, size_t n)
{
    const char *x = getenv("XDG_RUNTIME_DIR");
    int w;
    if (x && x[0])
        w = snprintf(out, n, "%s/cogdb", x);
    else
        w = snprintf(out, n, "/tmp/cogdb-%u", (unsigned)getuid());
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int cg_instance_path(const char *name, const char *leaf, char *out, size_t n)
{
    char root[CG_PATH_MAX];
    if (!cg_valid_name(name) || cg_runtime_dir(root, sizeof(root)) != 0)
        return -1;
    int w = leaf ? snprintf(out, n, "%s/%s/%s", root, name, leaf)
                 : snprintf(out, n, "%s/%s", root, name);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

int cg_mkdir_p(const char *path)
{
    char tmp[CG_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
        return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
