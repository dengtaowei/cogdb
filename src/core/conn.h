#ifndef CG_CONN_H
#define CG_CONN_H

#include <stddef.h>
#include <stdint.h>

#include "loop.h"

/* A non-blocking byte stream with an output queue. Owners receive data and
 * a single close notification; writes never block the loop. */

typedef struct cg_conn cg_conn_t;

typedef struct {
    void (*on_data)(void *ctx, const uint8_t *data, size_t n);
    /* Peer hung up or an I/O error happened. The conn is freed right after
     * this returns; the owner must drop its pointer. */
    void (*on_close)(void *ctx);
} cg_conn_ops_t;

/* Takes ownership of fd (it is set non-blocking). */
cg_conn_t *cg_conn_new(cg_loop_t *l, int fd, const cg_conn_ops_t *ops, void *ctx);
int cg_conn_write(cg_conn_t *c, const void *data, size_t n);
size_t cg_conn_pending(const cg_conn_t *c);
/* Output larger than this closes the conn (slow reader). 0 = unlimited. */
void cg_conn_set_limit(cg_conn_t *c, size_t max_pending);
/* Closes now; on_close is not called. */
void cg_conn_close(cg_conn_t *c);
/* Stops reading, flushes pending output, then closes; on_close is not
 * called. */
void cg_conn_close_after_flush(cg_conn_t *c);
int cg_conn_fd(const cg_conn_t *c);

#endif
