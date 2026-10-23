#ifndef CG_POLICY_H
#define CG_POLICY_H

/* What control-socket clients (agents, scripts) may do to the target.
 * Levels are cumulative. A human in gdb keeps a private picture of the
 * target (register cache, breakpoint list, "it is stopped"), so the hub
 * caps agents at a lower level while a gdb client is attached. */

typedef enum {
    CG_LVL_READ = 0, /* status, registers, memory, events, wait */
    CG_LVL_HALT,     /* + interrupt a running target */
    CG_LVL_RUN,      /* + continue, step, breakpoints */
    CG_LVL_WRITE,    /* + memory/register writes, monitor, raw packets, kick */
} cg_level_t;

typedef struct {
    cg_level_t agent;          /* no gdb client attached */
    cg_level_t agent_with_gdb; /* while one is */
} cg_policy_t;

void cg_policy_default(cg_policy_t *p);
cg_level_t cg_policy_effective(const cg_policy_t *p, int gdb_attached);
int cg_level_parse(const char *s, cg_level_t *out);
const char *cg_level_name(cg_level_t l);

#endif
