#include "transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int unix_open(const char *addr, char *err, size_t errn)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(addr) >= sizeof(sa.sun_path)) {
        snprintf(err, errn, "unix: path too long");
        return -1;
    }
    strcpy(sa.sun_path, addr);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        snprintf(err, errn, "unix: %s: %s", addr, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

const cg_transport_t cg_transport_unix = {
    "unix", "unix:/path/to/socket", unix_open,
};
