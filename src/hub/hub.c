/* Composition root: builds the modules, wires them through the bus, runs
 * the loop. No protocol logic lives here. */

#include "hub_priv.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util/log.h"

void cg_hub_cfg_default(cg_hub_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->gdb.host = "127.0.0.1";
    cfg->gdb.port = 4333;
    cfg->gdb.detach = CG_DETACH_RESUME;
    cg_policy_default(&cfg->policy);
    cfg->timeout_ms = 10000;
}

cg_level_t cg_hub_agent_level(const cg_hub_t *h)
{
    int attached = cg_gdbseat_attached(h->seat);
    cg_level_t lvl = cg_policy_effective(&h->cfg.policy, attached);
    cg_level_t granted;
    if (attached && cg_grant_level(h->grant, &granted) && granted > lvl)
        lvl = granted < h->cfg.policy.agent ? granted : h->cfg.policy.agent;
    return lvl;
}

void cg_hub_bp_add(cg_hub_t *h, int type, uint64_t addr, unsigned kind)
{
    for (int i = 0; i < h->nbps; i++)
        if (h->bps[i].type == type && h->bps[i].addr == addr && h->bps[i].kind == kind)
            return;
    if (h->nbps < CG_MAX_AGENT_BPS)
        h->bps[h->nbps++] = (cg_agent_bp_t){type, addr, kind};
}

void cg_hub_bp_del(cg_hub_t *h, int type, uint64_t addr, unsigned kind)
{
    for (int i = 0; i < h->nbps; i++) {
        if (h->bps[i].type == type && h->bps[i].addr == addr && h->bps[i].kind == kind) {
            h->bps[i] = h->bps[--h->nbps];
            return;
        }
    }
}

/* A new stub session starts with no breakpoints of ours. */
static void on_link(void *ctx, const cg_event_t *ev)
{
    cg_hub_t *h = ctx;
    if (strncmp(ev->text, "down", 4) == 0)
        h->nbps = 0;
}

static void log_event(void *ctx, const cg_event_t *ev)
{
    cg_hub_t *h = ctx;
    cg_buf_t line;
    cg_buf_init(&line);
    cg_event_format(h->bus, ev, &line);
    if (line.len)
        line.data[--line.len] = '\0';
    CG_INFO("%s", line.data);
    cg_buf_free(&line);
}

static void seat_monitor(void *ctx, const char *args, cg_buf_t *out)
{
    cg_hub_t *h = ctx;
    if (cg_miseat_busy(h->mi)) {
        cg_buf_puts(out, "cogdb: refused: an agent's command is running in this gdb\n");
        return;
    }
    cg_grant_monitor(h->grant, args, out);
    cg_buf_printf(out, "cogdb: agent level is now %s\n", cg_level_name(cg_hub_agent_level(h)));
}

static void teardown(cg_hub_t *h)
{
    cg_ctl_free(h->ctl);
    cg_gdbseat_free(h->seat);
    cg_miseat_free(h->mi);
    cg_grant_free(h->grant);
    cg_recorder_free(h->rec);
    cg_regmap_free(h->regmap);
    cg_target_free(h->target);
    cg_bus_free(h->bus);
    cg_loop_free(h->loop);
    char dir[CG_PATH_MAX];
    if (cg_instance_path(h->name, NULL, dir, sizeof(dir)) == 0)
        rmdir(dir);
}

int cg_hub_run(const cg_hub_cfg_t *cfg)
{
    cg_hub_t hub;
    cg_hub_t *h = &hub;
    memset(h, 0, sizeof(*h));
    h->cfg = *cfg;
    snprintf(h->name, sizeof(h->name), "%s", cfg->name);

    char dir[CG_PATH_MAX];
    if (cg_instance_path(h->name, NULL, dir, sizeof(dir)) != 0 || cg_mkdir_p(dir) != 0 ||
        cg_instance_path(h->name, "control.sock", h->ctl_path, sizeof(h->ctl_path)) != 0) {
        CG_ERR("cannot create runtime dir for '%s'", h->name);
        return 1;
    }

    cg_target_cfg_t tcfg = {.uri = cfg->stub_uri, .timeout_ms = cfg->timeout_ms};
    h->loop = cg_loop_new();
    h->bus = h->loop ? cg_bus_new(1024) : NULL;
    h->target = h->bus ? cg_target_new(h->loop, h->bus, &tcfg) : NULL;
    h->regmap = h->target ? cg_regmap_new(h->target, h->bus, cfg->big_endian) : NULL;
    if (!h->regmap) {
        CG_ERR("out of memory");
        teardown(h);
        return 1;
    }
    h->ctl = cg_ctl_new(h, h->loop, h->ctl_path);
    if (!h->ctl) {
        teardown(h);
        return 1;
    }
    h->grant = cg_grant_new(h->loop, h->bus);
    cg_gdbseat_cfg_t scfg = cfg->gdb;
    scfg.monitor = seat_monitor;
    scfg.monitor_ctx = h;
    h->seat = h->grant ? cg_gdbseat_new(h->loop, h->bus, h->target, &scfg) : NULL;
    if (!h->seat) {
        teardown(h);
        return 1;
    }
    char mi_path[CG_PATH_MAX];
    if (cg_instance_path(h->name, "gdb-mi", mi_path, sizeof(mi_path)) == 0)
        h->mi = cg_miseat_new(h->loop, h->bus, mi_path);
    if (cfg->record_path) {
        h->rec = cg_recorder_new(h->bus, cfg->record_path);
        if (!h->rec) {
            teardown(h);
            return 1;
        }
    }
    h->link_sub = cg_bus_subscribe(h->bus, CG_EVM(CG_EV_LINK), on_link, h);
    cg_bus_subscribe(h->bus, CG_EVM_DEFAULT & ~CG_EVM(CG_EV_CONSOLE), log_event, h);

    CG_INFO("hub '%s': stub %s, gdb on %s:%d, control %s", h->name, cfg->stub_uri,
            cfg->gdb.host, cg_gdbseat_port(h->seat), h->ctl_path);
    if (h->mi)
        CG_INFO("agents can use your gdb after: new-ui mi %s", cg_miseat_path(h->mi));
    cg_target_start(h->target);
    cg_loop_run(h->loop);
    CG_INFO("hub '%s' stopping", h->name);
    teardown(h);
    return 0;
}
