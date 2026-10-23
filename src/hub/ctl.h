#ifndef CG_CTL_H
#define CG_CTL_H

/* Control socket: one line in, a reply out, then close. A command may
 * answer later (it waits on the target) or stream (WATCH). The generic
 * gates, policy level and target state, are applied here before any
 * handler runs, so handlers only do their own work. */

#include <stdint.h>

#include "core/loop.h"
#include "policy.h"
#include "util/buf.h"

typedef struct cg_hub cg_hub_t;
typedef struct cg_ctl cg_ctl_t;
typedef struct cg_call cg_call_t;

typedef enum {
    CG_CALL_DONE,    /* reply is in cg_call_out(); close now */
    CG_CALL_PENDING, /* handler will call cg_call_finish() */
    CG_CALL_STREAM,  /* connection stays open; cg_call_stream() writes */
} cg_call_rc_t;

enum {
    CG_NEED_LINK = 1u << 0,
    CG_NEED_STOPPED = 1u << 1,
    CG_NEED_IDLE = 1u << 2, /* linked and not running */
};

typedef enum {
    CG_ARGS_NONE,
    CG_ARGS_OPT,
    CG_ARGS_REQ,
} cg_args_t;

typedef cg_call_rc_t (*cg_cmd_fn)(cg_hub_t *h, cg_call_t *call, const char *args);

typedef struct {
    const char *name;
    cg_level_t level;
    unsigned need;
    cg_args_t args;
    cg_cmd_fn fn;
    const char *usage;
} cg_cmd_t;

/* Defined in ctl_cmd.c, terminated by a NULL name. */
extern const cg_cmd_t cg_commands[];

cg_ctl_t *cg_ctl_new(cg_hub_t *h, cg_loop_t *loop, const char *path);
void cg_ctl_free(cg_ctl_t *c);

cg_buf_t *cg_call_out(cg_call_t *call);
cg_hub_t *cg_call_hub(cg_call_t *call);
void cg_call_finish(cg_call_t *call);
int cg_call_stream(cg_call_t *call, const char *data, size_t n);

/* Resources released automatically when the call ends either way. The
 * target and register map forget the call as a callback context too. */
void cg_call_own_sub(cg_call_t *call, uint64_t sub_id);
void cg_call_own_timer(cg_call_t *call, uint64_t timer_id);
/* Drops the owned subscription and timer now (the event they waited for
 * has happened). */
void cg_call_disarm(cg_call_t *call);
void cg_call_set_user(cg_call_t *call, void *user, void (*free_fn)(void *));
void *cg_call_user(cg_call_t *call);

#endif
