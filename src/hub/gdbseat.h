#ifndef CG_GDBSEAT_H
#define CG_GDBSEAT_H

/* The human's seat: a TCP port for `target remote`. Packets are relayed
 * through the shared target session, so gdb sees an ordinary stub while
 * agents use the same link. A few packets are answered here because they
 * describe this hop, not the target (ack mode, non-stop, detach,
 * `monitor cogdb ...`). */

#include "core/loop.h"
#include "events.h"
#include "target.h"
#include "util/buf.h"

typedef enum {
    CG_DETACH_RESUME,  /* answer D/k locally, resume the target */
    CG_DETACH_KEEP,    /* answer D/k locally, leave it halted */
    CG_DETACH_FORWARD, /* pass D/k to the stub (may end its session) */
} cg_detach_t;

/* Answers `monitor cogdb ARGS` on this hop; args is the text after "cogdb". */
typedef void (*cg_seat_monitor_fn)(void *ctx, const char *args, cg_buf_t *out);

typedef struct {
    const char *host;
    int port; /* 0 = pick one */
    cg_detach_t detach;
    int takeover; /* a new gdb replaces the attached one */
    cg_seat_monitor_fn monitor; /* NULL: every monitor command goes to the stub */
    void *monitor_ctx;
} cg_gdbseat_cfg_t;

typedef struct cg_gdbseat cg_gdbseat_t;

cg_gdbseat_t *cg_gdbseat_new(cg_loop_t *loop, cg_bus_t *bus, cg_target_t *t,
                             const cg_gdbseat_cfg_t *cfg);
void cg_gdbseat_free(cg_gdbseat_t *s);

int cg_gdbseat_attached(const cg_gdbseat_t *s);
const char *cg_gdbseat_peer(const cg_gdbseat_t *s);
int cg_gdbseat_port(const cg_gdbseat_t *s);
void cg_gdbseat_kick(cg_gdbseat_t *s, const char *why);
/* How the next client to attach detaches, instead of cfg.detach. */
void cg_gdbseat_detach_once(cg_gdbseat_t *s, cg_detach_t mode);

int cg_detach_parse(const char *s, cg_detach_t *out);
const char *cg_detach_name(cg_detach_t d);

#endif
