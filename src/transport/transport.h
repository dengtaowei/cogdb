#ifndef CG_TRANSPORT_H
#define CG_TRANSPORT_H

#include <stddef.h>

/* How cogdb reaches the stub. A transport only produces a connected fd;
 * framing and policy live above it. URIs look like scheme:address:
 *
 *   tcp:host:port       OpenOCD, gdbserver, J-Link, pyOCD, QEMU -gdb
 *   unix:/path          stubs listening on a Unix socket
 *   serial:/dev/X[,baud] a UART stub (also a couart seat PTY)
 *
 * A bare host:port or :port means tcp. */

typedef struct {
    const char *scheme;
    const char *usage;
    /* addr is the part after "scheme:". Returns a connected fd or -1 with
     * err filled in. */
    int (*open)(const char *addr, char *err, size_t errn);
} cg_transport_t;

extern const cg_transport_t cg_transport_tcp;
extern const cg_transport_t cg_transport_unix;
extern const cg_transport_t cg_transport_serial;

/* Resolves the scheme; *addr receives the remainder. NULL if unknown. */
const cg_transport_t *cg_transport_lookup(const char *uri, const char **addr);
int cg_transport_open(const char *uri, char *err, size_t errn);
/* NULL-terminated list, for --help. */
const cg_transport_t *const *cg_transport_all(void);

#endif
