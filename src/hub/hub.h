#ifndef CG_HUB_H
#define CG_HUB_H

#include "flow.h"
#include "gdbseat.h"
#include "policy.h"

typedef struct {
    const char *name;
    const char *stub_uri;
    cg_gdbseat_cfg_t gdb;
    cg_policy_t policy;
    const char *record_path; /* NULL = off */
    cg_flow_cfg_t flow;
    unsigned timeout_ms;
    int big_endian;
} cg_hub_cfg_t;

void cg_hub_cfg_default(cg_hub_cfg_t *cfg);
/* Runs until SIGINT/SIGTERM or SHUTDOWN. Returns a process exit code. */
int cg_hub_run(const cg_hub_cfg_t *cfg);

#endif
