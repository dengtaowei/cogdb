#ifndef CG_FLOW_H
#define CG_FLOW_H

/* Named procedures (load, reset sequences, ...) that live in the user's
 * own gdb script, not in the hub. A flow runs a batch gdb that sources the
 * script, connects to the hub's gdb seat and invokes one user-defined
 * command. Only names on the configured list can be run. */

#include <stddef.h>

#include "core/loop.h"
#include "events.h"

typedef struct {
    const char *gdb;    /* gdb executable; NULL = "gdb" */
    const char *script; /* sourced with -x; NULL = flows off */
    const char *names;  /* comma list of allowed commands */
} cg_flow_cfg_t;

typedef struct cg_flow cg_flow_t;

/* rc: gdb's exit status, or -1 if it was killed (timeout, signal). */
typedef void (*cg_flow_done_fn)(void *ctx, int rc, const char *out, size_t n);

cg_flow_t *cg_flow_new(cg_loop_t *loop, cg_bus_t *bus, const cg_flow_cfg_t *cfg);
void cg_flow_free(cg_flow_t *f);

int cg_flow_enabled(const cg_flow_t *f);
int cg_flow_busy(const cg_flow_t *f);
const char *cg_flow_names(const cg_flow_t *f);
int cg_flow_allowed(const cg_flow_t *f, const char *name);

/* 0 started; -1 cannot start (see log), -2 a flow is already running. */
int cg_flow_start(cg_flow_t *f, const char *name, int port, unsigned timeout_ms,
                  cg_flow_done_fn fn, void *ctx);
void cg_flow_forget(cg_flow_t *f, void *ctx);
/* Called after every flow ends, whether or not its caller is still there. */
void cg_flow_on_end(cg_flow_t *f, void (*fn)(void *ctx), void *ctx);

#endif
