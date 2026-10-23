#ifndef CG_MISEAT_H
#define CG_MISEAT_H

/* A second UI on the human's own gdb. The hub holds a pty and links it at
 * a fixed path; the human opts in with `new-ui mi PATH`, after which agent
 * commands run inside that gdb session (its symbols, its register cache,
 * its `load`). Works with gdb builds that lack Python. One command runs at
 * a time. */

#include <stddef.h>

#include "core/loop.h"
#include "events.h"

typedef struct cg_miseat cg_miseat_t;

/* ok: gdb answered ^done/^running. text: console output, then gdb's error
 * message when !ok. */
typedef void (*cg_mi_done_fn)(void *ctx, int ok, const char *text, size_t n);

cg_miseat_t *cg_miseat_new(cg_loop_t *loop, cg_bus_t *bus, const char *link_path);
void cg_miseat_free(cg_miseat_t *m);

int cg_miseat_attached(const cg_miseat_t *m);
int cg_miseat_busy(const cg_miseat_t *m);
const char *cg_miseat_path(const cg_miseat_t *m);

/* 0 on success; -1 no gdb attached, -2 a command is already running. */
int cg_miseat_exec(cg_miseat_t *m, const char *cmd, cg_mi_done_fn fn, void *ctx);
/* The caller went away; the running command finishes unreported. */
void cg_miseat_forget(cg_miseat_t *m, void *ctx);

#endif
