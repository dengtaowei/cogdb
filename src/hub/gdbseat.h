#ifndef CG_GDBSEAT_H
#define CG_GDBSEAT_H

/* The human's seat: a TCP port for `target remote`. Packets are relayed
 * through the shared target session, so gdb sees an ordinary stub while
 * agents use the same link. A few packets are answered here because they
 * describe this hop, not the target (ack mode, non-stop, detach). */

#include "core/loop.h"
#include "events.h"
#include "target.h"

typedef enum {
    CG_DETACH_RESUME,  /* answer D/k locally, resume the target */
    CG_DETACH_KEEP,    /* answer D/k locally, leave it halted */
    CG_DETACH_FORWARD, /* pass D/k to the stub (may end its session) */
} cg_detach_t;

typedef struct {
    const char *host;
    int port; /* 0 = pick one */
    cg_detach_t detach;
    int takeover; /* a new gdb replaces the attached one */
} cg_gdbseat_cfg_t;

typedef struct cg_gdbseat cg_gdbseat_t;

cg_gdbseat_t *cg_gdbseat_new(cg_loop_t *loop, cg_bus_t *bus, cg_target_t *t,
                             const cg_gdbseat_cfg_t *cfg);
void cg_gdbseat_free(cg_gdbseat_t *s);

int cg_gdbseat_attached(const cg_gdbseat_t *s);
const char *cg_gdbseat_peer(const cg_gdbseat_t *s);
int cg_gdbseat_port(const cg_gdbseat_t *s);
void cg_gdbseat_kick(cg_gdbseat_t *s, const char *why);

int cg_detach_parse(const char *s, cg_detach_t *out);
const char *cg_detach_name(cg_detach_t d);

#endif
