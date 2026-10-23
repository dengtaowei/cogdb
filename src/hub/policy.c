#include "policy.h"

#include <string.h>

static const char *const k_names[] = {"read", "halt", "run", "write"};

void cg_policy_default(cg_policy_t *p)
{
    p->agent = CG_LVL_WRITE;
    p->agent_with_gdb = CG_LVL_HALT;
}

cg_level_t cg_policy_effective(const cg_policy_t *p, int gdb_attached)
{
    if (!gdb_attached)
        return p->agent;
    return p->agent_with_gdb < p->agent ? p->agent_with_gdb : p->agent;
}

int cg_level_parse(const char *s, cg_level_t *out)
{
    for (int i = 0; i <= CG_LVL_WRITE; i++) {
        if (strcmp(s, k_names[i]) == 0) {
            *out = (cg_level_t)i;
            return 0;
        }
    }
    return -1;
}

const char *cg_level_name(cg_level_t l)
{
    return l <= CG_LVL_WRITE ? k_names[l] : "?";
}
