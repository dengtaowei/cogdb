#ifndef CG_TARGET_H
#define CG_TARGET_H

/* The one RSP session to the stub. Everybody who wants something from the
 * target (the gdb seat, control commands, the register map) submits a
 * request here; this module serialises them, does the ack dance, tracks
 * whether the target runs, and publishes what happened on the bus. */

#include <stddef.h>
#include <stdint.h>

#include "core/loop.h"
#include "events.h"
#include "rsp/stop.h"

typedef enum {
    CG_TS_DOWN,    /* no link to the stub */
    CG_TS_UNKNOWN, /* linked, not asked yet */
    CG_TS_STOPPED,
    CG_TS_RUNNING,
    CG_TS_EXITED,
} cg_tstate_t;

typedef enum {
    CG_REPLY_OK,
    CG_REPLY_TIMEOUT,
    CG_REPLY_LINKDOWN,
} cg_reply_status_t;

typedef struct {
    /* Final reply (raw payload, RLE intact). Called exactly once unless the
     * owner calls cg_target_forget first. */
    void (*on_reply)(void *ctx, cg_reply_status_t st, const char *data, size_t n);
    /* Intermediate packets while the request is open: O console output
     * (resume, qRcmd) and F file-I/O requests (resume). Optional. */
    void (*on_output)(void *ctx, const char *data, size_t n);
} cg_req_ops_t;

typedef struct {
    const char *uri;
    unsigned timeout_ms;      /* ordinary request */
    unsigned slow_timeout_ms; /* qRcmd, vFlash*, vRun, vAttach */
    unsigned reconnect_max_ms;
} cg_target_cfg_t;

typedef struct {
    unsigned packet_size; /* 0 = stub did not say */
    int xfer_features;    /* qXfer:features:read+ */
} cg_target_caps_t;

typedef struct cg_target cg_target_t;

cg_target_t *cg_target_new(cg_loop_t *loop, cg_bus_t *bus, const cg_target_cfg_t *cfg);
void cg_target_free(cg_target_t *t);
/* Connects now and keeps reconnecting with backoff while the hub runs. */
void cg_target_start(cg_target_t *t);
void cg_target_reconnect(cg_target_t *t);

/* origin must be a string with static lifetime. Returns 0 if queued. */
int cg_target_submit(cg_target_t *t, const char *origin, const char *payload, size_t n,
                     const cg_req_ops_t *ops, void *ctx);
/* Drops every callback registered with ctx; unsent requests are discarded,
 * the one on the wire still completes silently. */
void cg_target_forget(cg_target_t *t, void *ctx);
/* Sends 0x03 if the target runs. Returns 0 if sent, 1 if not running. */
int cg_target_interrupt(cg_target_t *t, const char *origin);
/* Writes a packet with no reply tracking (gdb's F replies while running). */
int cg_target_send_raw(cg_target_t *t, const char *origin, const char *payload, size_t n);

cg_tstate_t cg_target_state(const cg_target_t *t);
const char *cg_tstate_name(cg_tstate_t s);
/* NULL until the first stop. */
const cg_stop_t *cg_target_last_stop(const cg_target_t *t);
const char *cg_target_run_origin(const cg_target_t *t);
const cg_target_caps_t *cg_target_caps(const cg_target_t *t);
size_t cg_target_queue_len(const cg_target_t *t);
const char *cg_target_uri(const cg_target_t *t);

#endif
