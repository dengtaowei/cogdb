#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_MS 2000

static int connect_timeout(int fd, const struct sockaddr *sa, socklen_t len)
{
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (connect(fd, sa, len) == 0)
        return 0;
    if (errno != EINPROGRESS)
        return -1;
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, CONNECT_TIMEOUT_MS) != 1) {
        errno = ETIMEDOUT;
        return -1;
    }
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr) {
        errno = soerr;
        return -1;
    }
    return 0;
}

static int tcp_open(const char *addr, char *err, size_t errn)
{
    char host[256];
    const char *colon = strrchr(addr, ':');
    if (!colon || !colon[1]) {
        snprintf(err, errn, "tcp: expected host:port, got '%s'", addr);
        return -1;
    }
    size_t hn = (size_t)(colon - addr);
    if (hn >= sizeof(host))
        hn = sizeof(host) - 1;
    memcpy(host, addr, hn);
    host[hn] = '\0';
    if (!host[0])
        snprintf(host, sizeof(host), "127.0.0.1");

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, colon + 1, &hints, &res);
    if (rc != 0) {
        snprintf(err, errn, "tcp: %s: %s", addr, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect_timeout(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        snprintf(err, errn, "tcp: %s: %s", addr, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

const cg_transport_t cg_transport_tcp = {
    "tcp", "tcp:HOST:PORT (or HOST:PORT, :PORT)", tcp_open,
};
