#ifndef CG_HUB_PRIV_H
#define CG_HUB_PRIV_H

/* Shared by the hub's own modules (hub.c, ctl.c, ctl_cmd.c). */

#include "core/loop.h"
#include "ctl.h"
#include "events.h"
#include "gdbseat.h"
#include "grant.h"
#include "hub.h"
#include "policy.h"
#include "record.h"
#include "regmap.h"
#include "target.h"
#include "util/sys.h"

#define CG_MAX_AGENT_BPS 64

typedef struct {
    int type; /* Z type 0..4 */
    uint64_t addr;
    unsigned kind;
} cg_agent_bp_t;

struct cg_hub {
    cg_hub_cfg_t cfg;
    char name[CG_NAME_MAX];
    char ctl_path[CG_PATH_MAX];
    cg_loop_t *loop;
    cg_bus_t *bus;
    cg_target_t *target;
    cg_regmap_t *regmap;
    cg_gdbseat_t *seat;
    cg_grant_t *grant;
    cg_ctl_t *ctl;
    cg_recorder_t *rec;
    uint64_t link_sub;
    cg_agent_bp_t bps[CG_MAX_AGENT_BPS];
    int nbps;
};

cg_level_t cg_hub_agent_level(const cg_hub_t *h);
void cg_hub_bp_add(cg_hub_t *h, int type, uint64_t addr, unsigned kind);
void cg_hub_bp_del(cg_hub_t *h, int type, uint64_t addr, unsigned kind);

#endif
