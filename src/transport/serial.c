#include "transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static speed_t baud_const(long baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    case 1500000: return B1500000;
    case 2000000: return B2000000;
    case 3000000: return B3000000;
    default: return 0;
    }
}

static int serial_open(const char *addr, char *err, size_t errn)
{
    char path[256];
    long baud = 115200;
    const char *comma = strchr(addr, ',');
    size_t pn = comma ? (size_t)(comma - addr) : strlen(addr);
    if (pn == 0 || pn >= sizeof(path)) {
        snprintf(err, errn, "serial: expected /dev/X[,baud]");
        return -1;
    }
    memcpy(path, addr, pn);
    path[pn] = '\0';
    if (comma)
        baud = strtol(comma + 1, NULL, 10);
    speed_t sp = baud_const(baud);
    if (!sp) {
        snprintf(err, errn, "serial: unsupported baud %ld", baud);
        return -1;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, errn, "serial: %s: %s", path, strerror(errno));
        return -1;
    }
    if (ioctl(fd, TIOCEXCL) != 0 && errno != ENOTTY) {
        snprintf(err, errn, "serial: %s: exclusive open failed: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~(CRTSCTS | CSTOPB);
        tio.c_cc[VMIN] = 1;
        tio.c_cc[VTIME] = 0;
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
        tcsetattr(fd, TCSANOW, &tio);
        tcflush(fd, TCIOFLUSH);
    }
    return fd;
}

const cg_transport_t cg_transport_serial = {
    "serial", "serial:/dev/ttyUSB0[,115200]", serial_open,
};
