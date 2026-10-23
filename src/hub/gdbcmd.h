#ifndef CG_GDBCMD_H
#define CG_GDBCMD_H

/* What an agent needs to run a gdb CLI command in the human's gdb.
 * Commands that only report (bt, info, x, print without side effects) need
 * read; anything else needs write. Commands that act on the host or on
 * gdb's own command set are refused outright, whatever the grant. Unknown
 * commands count as writes. */

#include "policy.h"

typedef enum {
    CG_GDBCMD_OK,
    CG_GDBCMD_BLOCKED, /* never from an agent */
    CG_GDBCMD_BAD,     /* empty, multi-line or control characters */
} cg_gdbcmd_verdict_t;

cg_gdbcmd_verdict_t cg_gdbcmd_classify(const char *cmd, cg_level_t *level);

#endif
