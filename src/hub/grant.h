#ifndef CG_GRANT_H
#define CG_GRANT_H

/* Temporary rights the human in gdb hands to agents, e.g.
 * `monitor cogdb allow write 120`. A grant lifts the with-gdb cap for a
 * while; it ends when it expires, when the human revokes it, or when that
 * gdb session ends. Only the gdb seat can create one, so an agent cannot
 * raise its own level. */

#include <stdint.h>

#include "core/loop.h"
#include "events.h"
#include "policy.h"
#include "util/buf.h"

#define CG_GRANT_DEFAULT_S 300
#define CG_GRANT_MAX_S 86400

typedef struct cg_grant cg_grant_t;

cg_grant_t *cg_grant_new(cg_loop_t *loop, cg_bus_t *bus);
void cg_grant_free(cg_grant_t *g);

/* Returns 1 and the granted level while a grant is in force. */
int cg_grant_level(const cg_grant_t *g, cg_level_t *out);
/* Seconds left, 0 when there is no grant. */
unsigned cg_grant_remaining_s(const cg_grant_t *g);

/* Handles the text after "cogdb" of a gdb `monitor cogdb ...` command and
 * appends the human-readable answer to out. */
void cg_grant_monitor(cg_grant_t *g, const char *args, cg_buf_t *out);

#endif
