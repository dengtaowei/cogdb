/* Control commands. Wire format: docs/PROTOCOL.md. Policy level and target
 * state are checked by ctl.c from the table at the bottom, before any
 * handler here runs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hub_priv.h"
#include "rsp/rsp.h"
#include "util/hex.h"

#ifndef COGDB_VERSION
#define COGDB_VERSION "0.1.0"
#endif

#define PROTO_VERSION 1
#define WAIT_DEFAULT_MS 30000
#define HALT_DEFAULT_MS 3000
#define TIMEOUT_MAX_MS 600000
#define MEM_MAX (64 * 1024)

/* ---- small helpers ----------------------------------------------------- */

/* Copies the next space-separated token; returns 0 if there is none. */
static int tok(const char **p, char *buf, size_t cap)
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    size_t n = strcspn(s, " ");
    if (n == 0)
        return 0;
    if (n >= cap)
        n = cap - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    s += strcspn(s, " ");
    *p = s;
    return 1;
}

static unsigned parse_timeout(const char *s, unsigned def)
{
    uint64_t v;
    if (!s || !*s || cg_parse_u64(s, 0, &v) != 0 || v == 0)
        return def;
    return v > TIMEOUT_MAX_MS ? TIMEOUT_MAX_MS : (unsigned)v;
}

static void escaped(cg_buf_t *out, const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 0x20 && c < 0x7f && c != '\\')
            cg_buf_putc(out, (char)c);
        else
            cg_buf_printf(out, "\\x%02x", c);
    }
}

static int is_error(const char *p, size_t n)
{
    return n >= 1 && p[0] == 'E' && (n == 3 || (n > 1 && p[1] == '.'));
}

static cg_call_rc_t err(cg_call_t *call, const char *msg)
{
    cg_buf_printf(cg_call_out(call), "ERR %s\n", msg);
    return CG_CALL_DONE;
}

static void fail(cg_call_t *call, const char *what, cg_reply_status_t st, const char *p, size_t n)
{
    cg_buf_t *out = cg_call_out(call);
    if (st == CG_REPLY_TIMEOUT)
        cg_buf_printf(out, "ERR %s: stub did not answer\n", what);
    else if (st == CG_REPLY_LINKDOWN)
        cg_buf_printf(out, "ERR %s: stub link lost\n", what);
    else if (n == 0)
        cg_buf_printf(out, "ERR %s: not supported by the stub\n", what);
    else {
        cg_buf_printf(out, "ERR %s: stub said ", what);
        escaped(out, p, n);
        cg_buf_putc(out, '\n');
    }
    cg_call_finish(call);
}

static void free_buf(void *p)
{
    cg_buf_free(p);
    free(p);
}

/* ---- stop report (shared by HALT, WAIT, STEP, CONT n) ------------------ */

static void stop_pc_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    if (st == CG_REPLY_OK && n && !is_error(p, n)) {
        cg_buf_t ex;
        cg_buf_init(&ex);
        cg_rsp_expand_rle(p, n, &ex);
        cg_buf_puts(cg_call_out(call), "pc=");
        cg_regmap_value(h->regmap, ex.data, ex.len, cg_call_out(call));
        cg_buf_putc(cg_call_out(call), '\n');
        cg_buf_free(&ex);
    }
    cg_call_finish(call);
}

static void stop_regmap_ready(void *ctx, int ok)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    const cg_stop_t *s = cg_target_last_stop(h->target);
    const cg_reg_t *pc = ok ? cg_regmap_pc(h->regmap) : NULL;
    if (!pc || !s || s->kind != CG_STOP_SIGNAL ||
        cg_target_state(h->target) != CG_TS_STOPPED) {
        cg_call_finish(call);
        return;
    }
    for (int i = 0; i < s->nregs; i++) {
        if (s->regs[i].regnum == pc->regnum) {
            cg_buf_puts(cg_call_out(call), "pc=");
            cg_regmap_value(h->regmap, s->regs[i].hex, strlen(s->regs[i].hex),
                            cg_call_out(call));
            cg_buf_putc(cg_call_out(call), '\n');
            cg_call_finish(call);
            return;
        }
    }
    char pkt[24];
    int k = snprintf(pkt, sizeof(pkt), "p%x", pc->regnum);
    static const cg_req_ops_t ops = {stop_pc_reply, NULL};
    if (cg_target_submit(h->target, "agent", pkt, (size_t)k, &ops, call) != 0)
        cg_call_finish(call);
}

/* Appends state/stop lines, then pc when it can be had, then finishes. */
static cg_call_rc_t reply_stop(cg_hub_t *h, cg_call_t *call)
{
    cg_buf_t *out = cg_call_out(call);
    cg_buf_printf(out, "state=%s\n", cg_tstate_name(cg_target_state(h->target)));
    const cg_stop_t *s = cg_target_last_stop(h->target);
    cg_buf_puts(out, "stop=");
    if (s)
        cg_stop_describe(s, out);
    else
        cg_buf_putc(out, '-');
    cg_buf_putc(out, '\n');
    cg_regmap_ensure(h->regmap, stop_regmap_ready, call);
    return CG_CALL_PENDING;
}

static void wait_on_stop(void *ctx, const cg_event_t *ev)
{
    (void)ev;
    cg_call_t *call = ctx;
    cg_call_disarm(call);
    cg_buf_puts(cg_call_out(call), "OK stopped\n");
    reply_stop(cg_call_hub(call), call);
}

static void wait_timeout(void *ctx)
{
    cg_call_t *call = ctx;
    cg_call_own_timer(call, 0);
    cg_buf_puts(cg_call_out(call), "ERR timeout: target still running\n");
    cg_call_finish(call);
}

static cg_call_rc_t arm_wait(cg_hub_t *h, cg_call_t *call, unsigned timeout_ms)
{
    cg_call_own_sub(call, cg_bus_subscribe(h->bus, CG_EVM(CG_EV_STOP), wait_on_stop, call));
    cg_call_own_timer(call, cg_loop_timer(h->loop, timeout_ms, wait_timeout, call));
    return CG_CALL_PENDING;
}

/* ---- informational ----------------------------------------------------- */

static cg_call_rc_t cmd_hello(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)h;
    (void)args;
    cg_buf_t *out = cg_call_out(call);
    cg_buf_printf(out, "OK cogdb=%s proto=%d\ncmds=", COGDB_VERSION, PROTO_VERSION);
    for (const cg_cmd_t *c = cg_commands; c->name; c++)
        cg_buf_printf(out, "%s%s", c == cg_commands ? "" : " ", c->name);
    cg_buf_putc(out, '\n');
    return CG_CALL_DONE;
}

static cg_call_rc_t cmd_ping(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)h;
    (void)args;
    cg_buf_puts(cg_call_out(call), "OK pong\n");
    return CG_CALL_DONE;
}

static cg_call_rc_t cmd_status(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    cg_buf_t *out = cg_call_out(call);
    cg_tstate_t st = cg_target_state(h->target);
    const cg_stop_t *s = cg_target_last_stop(h->target);
    const cg_tdesc_t *td = cg_regmap_tdesc(h->regmap);
    const char *owner = cg_target_run_origin(h->target);
    int attached = cg_gdbseat_attached(h->seat);

    cg_buf_printf(out, "OK\nname=%s\nstub=%s\nlink=%s\nstate=%s\nrun_owner=%s\n", h->name,
                  cg_target_uri(h->target), st == CG_TS_DOWN ? "down" : "up",
                  cg_tstate_name(st), owner ? owner : "-");
    cg_buf_puts(out, "last_stop=");
    if (s)
        cg_stop_describe(s, out);
    else
        cg_buf_putc(out, '-');
    cg_buf_printf(out, "\ngdb=%s\ngdb_port=%d\ndetach=%s\n",
                  attached ? cg_gdbseat_peer(h->seat) : "-", cg_gdbseat_port(h->seat),
                  cg_detach_name(h->cfg.gdb.detach));
    cg_buf_printf(out, "agent_level=%s\npolicy=%s/%s\n",
                  cg_level_name(cg_hub_agent_level(h)), cg_level_name(h->cfg.policy.agent),
                  cg_level_name(h->cfg.policy.agent_with_gdb));
    cg_level_t granted;
    if (cg_grant_level(h->grant, &granted))
        cg_buf_printf(out, "grant=%s %us\n", cg_level_name(granted),
                      cg_grant_remaining_s(h->grant));
    else
        cg_buf_puts(out, "grant=-\n");
    cg_buf_printf(out, "queue=%zu\narch=%s\nregisters=%zu\npacket_size=%u\nagent_breakpoints=%d\n",
                  cg_target_queue_len(h->target), td && td->arch[0] ? td->arch : "-",
                  td ? td->n : 0, cg_target_caps(h->target)->packet_size, h->nbps);
    cg_buf_printf(out, "events=%llu\nrecord=%s\n", (unsigned long long)cg_bus_seq(h->bus),
                  h->cfg.record_path ? h->cfg.record_path : "-");
    return CG_CALL_DONE;
}

static cg_call_rc_t cmd_events(cg_hub_t *h, cg_call_t *call, const char *args)
{
    size_t n = 50;
    unsigned mask = CG_EVM_DEFAULT;
    char t[128];
    while (tok(&args, t, sizeof(t))) {
        uint64_t v;
        if (cg_parse_u64(t, 0, &v) == 0)
            n = v ? (size_t)v : 50;
        else if (cg_ev_parse_mask(t, &mask) != 0)
            return err(call, "unknown event type (link client run stop console note packet all)");
    }
    cg_buf_puts(cg_call_out(call), "OK\n");
    cg_bus_history(h->bus, n, mask, cg_call_out(call));
    return CG_CALL_DONE;
}

static void watch_event(void *ctx, const cg_event_t *ev)
{
    cg_call_t *call = ctx;
    cg_buf_t line;
    cg_buf_init(&line);
    cg_event_format(cg_call_hub(call)->bus, ev, &line);
    cg_call_stream(call, line.data, line.len);
    cg_buf_free(&line);
}

static cg_call_rc_t cmd_watch(cg_hub_t *h, cg_call_t *call, const char *args)
{
    unsigned mask = CG_EVM_DEFAULT;
    char t[128];
    if (tok(&args, t, sizeof(t)) && cg_ev_parse_mask(t, &mask) != 0)
        return err(call, "unknown event type (link client run stop console note packet all)");
    static const char hello[] = "OK watching\n";
    if (cg_call_stream(call, hello, sizeof(hello) - 1) != 0)
        return CG_CALL_PENDING; /* peer already gone; the call is released */
    cg_call_own_sub(call, cg_bus_subscribe(h->bus, mask, watch_event, call));
    return CG_CALL_STREAM;
}

static cg_call_rc_t cmd_breaks(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    static const char *const types[] = {"sw", "hw", "write", "read", "access"};
    cg_buf_t *out = cg_call_out(call);
    cg_buf_printf(out, "OK count=%d\n", h->nbps);
    for (int i = 0; i < h->nbps; i++)
        cg_buf_printf(out, "%s 0x%llx kind=%u\n", types[h->bps[i].type],
                      (unsigned long long)h->bps[i].addr, h->bps[i].kind);
    return CG_CALL_DONE;
}

/* ---- run control ------------------------------------------------------- */

static cg_call_rc_t cmd_wait(cg_hub_t *h, cg_call_t *call, const char *args)
{
    unsigned timeout = parse_timeout(args, WAIT_DEFAULT_MS);
    cg_tstate_t st = cg_target_state(h->target);
    if (st != CG_TS_RUNNING) {
        cg_buf_printf(cg_call_out(call), "OK %s\n", cg_tstate_name(st));
        return reply_stop(h, call);
    }
    return arm_wait(h, call, timeout);
}

static cg_call_rc_t cmd_halt(cg_hub_t *h, cg_call_t *call, const char *args)
{
    unsigned timeout = parse_timeout(args, HALT_DEFAULT_MS);
    cg_tstate_t st = cg_target_state(h->target);
    if (st != CG_TS_RUNNING) {
        cg_buf_printf(cg_call_out(call), "OK already %s\n", cg_tstate_name(st));
        return reply_stop(h, call);
    }
    arm_wait(h, call, timeout);
    cg_target_interrupt(h->target, "agent");
    return CG_CALL_PENDING;
}

static cg_call_rc_t cmd_cont(cg_hub_t *h, cg_call_t *call, const char *args)
{
    if (cg_target_submit(h->target, "agent", "c", 1, NULL, NULL) != 0)
        return err(call, "stub link is down");
    if (!*args) {
        cg_buf_puts(cg_call_out(call), "OK running\n");
        return CG_CALL_DONE;
    }
    return arm_wait(h, call, parse_timeout(args, WAIT_DEFAULT_MS));
}

static void step_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    if (st != CG_REPLY_OK || !cg_rsp_is_stop(p, n)) {
        fail(call, "step", st, p, n);
        return;
    }
    cg_buf_puts(cg_call_out(call), "OK stepped\n");
    reply_stop(cg_call_hub(call), call);
}

static cg_call_rc_t cmd_step(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    static const cg_req_ops_t ops = {step_reply, NULL};
    if (cg_target_submit(h->target, "agent", "s", 1, &ops, call) != 0)
        return err(call, "stub link is down");
    return CG_CALL_PENDING;
}

/* ---- registers --------------------------------------------------------- */

typedef struct {
    cg_buf_t which;
    unsigned missing[CG_REGMAP_MAX_MISSING];
    size_t nmiss, next;
} regs_t;

static void regs_free(void *p)
{
    regs_t *r = p;
    cg_buf_free(&r->which);
    free(r);
}

static void regs_p_next(cg_call_t *call);

static void regs_p_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    regs_t *rs = cg_call_user(call);
    const cg_tdesc_t *td = cg_regmap_tdesc(h->regmap);
    const cg_reg_t *r = td ? cg_tdesc_by_num(td, rs->missing[rs->next]) : NULL;
    cg_buf_t *out = cg_call_out(call);
    rs->next++;
    if (r) {
        cg_buf_printf(out, "%s=", r->name);
        if (st == CG_REPLY_OK && n && !is_error(p, n)) {
            cg_buf_t ex;
            cg_buf_init(&ex);
            cg_rsp_expand_rle(p, n, &ex);
            cg_regmap_value(h->regmap, ex.data, ex.len, out);
            cg_buf_free(&ex);
        } else {
            cg_buf_puts(out, "unavailable");
        }
        cg_buf_putc(out, '\n');
    }
    if (st == CG_REPLY_LINKDOWN)
        rs->next = rs->nmiss;
    regs_p_next(call);
}

static void regs_p_next(cg_call_t *call)
{
    regs_t *rs = cg_call_user(call);
    if (rs->next >= rs->nmiss) {
        cg_call_finish(call);
        return;
    }
    char pkt[24];
    int k = snprintf(pkt, sizeof(pkt), "p%x", rs->missing[rs->next]);
    static const cg_req_ops_t ops = {regs_p_reply, NULL};
    if (cg_target_submit(cg_call_hub(call)->target, "agent", pkt, (size_t)k, &ops, call) != 0)
        cg_call_finish(call);
}

static void regs_g_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    if (st != CG_REPLY_OK || n == 0 || is_error(p, n)) {
        fail(call, "read registers", st, p, n);
        return;
    }
    cg_buf_t ex;
    cg_buf_init(&ex);
    cg_rsp_expand_rle(p, n, &ex);
    regs_t *rs = cg_call_user(call);
    cg_buf_puts(cg_call_out(call), "OK\n");
    rs->nmiss = cg_regmap_format_g(h->regmap, ex.data, ex.len, rs->which.data,
                                   cg_call_out(call), rs->missing, CG_REGMAP_MAX_MISSING);
    rs->next = 0;
    cg_buf_free(&ex);
    regs_p_next(call);
}

static const cg_req_ops_t k_g_ops = {regs_g_reply, NULL};

static void regs_ready(void *ctx, int ok)
{
    (void)ok; /* without names REGS still returns raw g */
    cg_call_t *call = ctx;
    if (cg_target_submit(cg_call_hub(call)->target, "agent", "g", 1, &k_g_ops, call) != 0)
        fail(call, "read registers", CG_REPLY_LINKDOWN, "", 0);
}

/* REG fallback for stubs without 'p': slice the one register out of g. */
static void reg_g_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    if (st != CG_REPLY_OK || n == 0 || is_error(p, n)) {
        fail(call, "read register", st, p, n);
        return;
    }
    cg_buf_t ex;
    cg_buf_init(&ex);
    cg_rsp_expand_rle(p, n, &ex);
    cg_buf_t *name = cg_call_user(call);
    cg_buf_t lines;
    cg_buf_init(&lines);
    cg_regmap_format_g(h->regmap, ex.data, ex.len, name->data, &lines, NULL, 0);
    if (lines.len) {
        cg_buf_puts(cg_call_out(call), "OK\n");
        cg_buf_append(cg_call_out(call), lines.data, lines.len);
    } else {
        cg_buf_printf(cg_call_out(call), "ERR read register: stub has no value for %s\n",
                      name->data);
    }
    cg_buf_free(&lines);
    cg_buf_free(&ex);
    cg_call_finish(call);
}

static cg_buf_t *user_text(cg_call_t *call, const char *s)
{
    cg_buf_t *b = malloc(sizeof(*b));
    if (!b)
        return NULL;
    cg_buf_init(b);
    cg_buf_puts(b, s);
    cg_call_set_user(call, b, free_buf);
    return b;
}

static cg_call_rc_t cmd_regs(cg_hub_t *h, cg_call_t *call, const char *args)
{
    regs_t *rs = calloc(1, sizeof(*rs));
    if (!rs)
        return err(call, "out of memory");
    cg_buf_init(&rs->which);
    if (*args)
        cg_buf_puts(&rs->which, args);
    cg_call_set_user(call, rs, regs_free);
    cg_regmap_ensure(h->regmap, regs_ready, call);
    return CG_CALL_PENDING;
}

static void reg_p_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    cg_buf_t *name = cg_call_user(call);
    if (st == CG_REPLY_OK && (n == 0 || is_error(p, n))) {
        static const cg_req_ops_t g_ops = {reg_g_reply, NULL};
        if (cg_target_submit(h->target, "agent", "g", 1, &g_ops, call) != 0)
            fail(call, "read register", CG_REPLY_LINKDOWN, "", 0);
        return;
    }
    if (st != CG_REPLY_OK) {
        fail(call, "read register", st, p, n);
        return;
    }
    cg_buf_t ex;
    cg_buf_init(&ex);
    cg_rsp_expand_rle(p, n, &ex);
    cg_buf_printf(cg_call_out(call), "OK\n%s=", name->data);
    cg_regmap_value(h->regmap, ex.data, ex.len, cg_call_out(call));
    cg_buf_putc(cg_call_out(call), '\n');
    cg_buf_free(&ex);
    cg_call_finish(call);
}

static const cg_reg_t *lookup_reg(cg_call_t *call, int ok, const char *name)
{
    cg_hub_t *h = cg_call_hub(call);
    const cg_tdesc_t *td = ok ? cg_regmap_tdesc(h->regmap) : NULL;
    if (!td) {
        cg_buf_puts(cg_call_out(call), "ERR no target description; use REGS for raw g\n");
        cg_call_finish(call);
        return NULL;
    }
    const cg_reg_t *r = cg_tdesc_find(td, name);
    if (!r) {
        cg_buf_printf(cg_call_out(call), "ERR no register named %s\n", name);
        cg_call_finish(call);
    }
    return r;
}

static void reg_ready(void *ctx, int ok)
{
    cg_call_t *call = ctx;
    cg_buf_t *name = cg_call_user(call);
    const cg_reg_t *r = lookup_reg(call, ok, name->data);
    if (!r)
        return;
    cg_buf_reset(name);
    cg_buf_puts(name, r->name);
    char pkt[24];
    int k = snprintf(pkt, sizeof(pkt), "p%x", r->regnum);
    static const cg_req_ops_t ops = {reg_p_reply, NULL};
    if (cg_target_submit(cg_call_hub(call)->target, "agent", pkt, (size_t)k, &ops, call) != 0)
        fail(call, "read register", CG_REPLY_LINKDOWN, "", 0);
}

static cg_call_rc_t cmd_reg(cg_hub_t *h, cg_call_t *call, const char *args)
{
    char name[64];
    if (!tok(&args, name, sizeof(name)) || !user_text(call, name))
        return err(call, "usage: REG NAME");
    cg_regmap_ensure(h->regmap, reg_ready, call);
    return CG_CALL_PENDING;
}

static void simple_ok_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    if (st != CG_REPLY_OK || !(n == 2 && memcmp(p, "OK", 2) == 0)) {
        fail(call, "write", st, p, n);
        return;
    }
    cg_buf_puts(cg_call_out(call), "OK\n");
    cg_call_finish(call);
}

static void wreg_ready(void *ctx, int ok)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    cg_buf_t *spec = cg_call_user(call);
    const char *args = spec->data;
    char name[64], val[40];
    tok(&args, name, sizeof(name));
    tok(&args, val, sizeof(val));
    const cg_reg_t *r = lookup_reg(call, ok, name);
    if (!r)
        return;
    uint64_t v;
    cg_buf_t pkt;
    cg_buf_init(&pkt);
    cg_buf_printf(&pkt, "P%x=", r->regnum);
    if (cg_parse_u64(val, 0, &v) != 0 || cg_regmap_encode(h->regmap, v, r->bitsize, &pkt) != 0) {
        cg_buf_free(&pkt);
        cg_buf_printf(cg_call_out(call), "ERR bad value for %u-bit register %s\n", r->bitsize,
                      r->name);
        cg_call_finish(call);
        return;
    }
    static const cg_req_ops_t ops = {simple_ok_reply, NULL};
    if (cg_target_submit(h->target, "agent", pkt.data, pkt.len, &ops, call) != 0)
        fail(call, "write register", CG_REPLY_LINKDOWN, "", 0);
    cg_buf_free(&pkt);
}

static cg_call_rc_t cmd_wreg(cg_hub_t *h, cg_call_t *call, const char *args)
{
    const char *a = args;
    char name[64], val[40];
    if (!tok(&a, name, sizeof(name)) || !tok(&a, val, sizeof(val)) || !user_text(call, args))
        return err(call, "usage: WREG NAME VALUE");
    cg_regmap_ensure(h->regmap, wreg_ready, call);
    return CG_CALL_PENDING;
}

/* ---- memory ------------------------------------------------------------ */

typedef struct {
    uint64_t addr;
    size_t len;
    size_t done;
    size_t chunk;
    cg_buf_t data; /* raw bytes (read) or hex (write) */
} mem_t;

static void mem_free(void *p)
{
    mem_t *m = p;
    cg_buf_free(&m->data);
    free(m);
}

static mem_t *mem_new(cg_call_t *call, uint64_t addr, size_t len, size_t chunk)
{
    mem_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->addr = addr;
    m->len = len;
    m->chunk = chunk;
    cg_buf_init(&m->data);
    cg_call_set_user(call, m, mem_free);
    return m;
}

static size_t chunk_for(const cg_hub_t *h, size_t overhead, size_t cap)
{
    unsigned ps = cg_target_caps(h->target)->packet_size;
    size_t c = ps > overhead + 16 ? (ps - overhead) / 2 : 0x200;
    return c > cap ? cap : c;
}

static void mem_read_next(cg_call_t *call);

static void mem_read_finish(cg_call_t *call, int partial)
{
    mem_t *m = cg_call_user(call);
    cg_buf_t *out = cg_call_out(call);
    cg_buf_printf(out, "OK addr=0x%llx bytes=%zu%s\n", (unsigned long long)m->addr, m->data.len,
                  partial ? " partial=1" : "");
    cg_hex_encode(out, (const uint8_t *)m->data.data, m->data.len);
    cg_buf_putc(out, '\n');
    cg_call_finish(call);
}

static void mem_read_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    mem_t *m = cg_call_user(call);
    if (st != CG_REPLY_OK || n == 0 || is_error(p, n)) {
        if (m->done)
            mem_read_finish(call, 1);
        else
            fail(call, "read memory", st, p, n);
        return;
    }
    cg_buf_t ex;
    cg_buf_init(&ex);
    cg_rsp_expand_rle(p, n, &ex);
    size_t room = m->len - m->done;
    size_t nb = ex.len / 2 < room ? ex.len / 2 : room;
    cg_buf_reserve(&m->data, nb);
    long got = cg_hex_decode(ex.data, nb * 2, (uint8_t *)m->data.data + m->data.len, nb);
    cg_buf_free(&ex);
    if (got <= 0) {
        if (m->done)
            mem_read_finish(call, 1);
        else
            fail(call, "read memory", CG_REPLY_OK, p, n);
        return;
    }
    m->data.len += (size_t)got;
    m->data.data[m->data.len] = '\0';
    m->done += (size_t)got;
    mem_read_next(call);
}

static void mem_read_next(cg_call_t *call)
{
    mem_t *m = cg_call_user(call);
    if (m->done >= m->len) {
        mem_read_finish(call, 0);
        return;
    }
    size_t want = m->len - m->done < m->chunk ? m->len - m->done : m->chunk;
    char pkt[48];
    int k = snprintf(pkt, sizeof(pkt), "m%llx,%zx", (unsigned long long)(m->addr + m->done), want);
    static const cg_req_ops_t ops = {mem_read_reply, NULL};
    if (cg_target_submit(cg_call_hub(call)->target, "agent", pkt, (size_t)k, &ops, call) != 0)
        fail(call, "read memory", CG_REPLY_LINKDOWN, "", 0);
}

static cg_call_rc_t cmd_mem(cg_hub_t *h, cg_call_t *call, const char *args)
{
    char a[32], l[32];
    uint64_t addr, len;
    if (!tok(&args, a, sizeof(a)) || !tok(&args, l, sizeof(l)) || cg_parse_u64(a, 1, &addr) ||
        cg_parse_u64(l, 0, &len))
        return err(call, "usage: MEM ADDR LEN (ADDR is hex, LEN decimal or 0x..)");
    if (len == 0 || len > MEM_MAX)
        return err(call, "LEN must be 1..65536");
    if (!mem_new(call, addr, (size_t)len, chunk_for(h, 8, 0x1000)))
        return err(call, "out of memory");
    mem_read_next(call);
    return CG_CALL_PENDING;
}

static void mem_write_next(cg_call_t *call);

static void mem_write_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    mem_t *m = cg_call_user(call);
    if (st != CG_REPLY_OK || !(n == 2 && memcmp(p, "OK", 2) == 0)) {
        fail(call, "write memory", st, p, n);
        return;
    }
    m->done += m->len - m->done < m->chunk ? m->len - m->done : m->chunk;
    mem_write_next(call);
}

static void mem_write_next(cg_call_t *call)
{
    mem_t *m = cg_call_user(call);
    if (m->done >= m->len) {
        cg_buf_printf(cg_call_out(call), "OK wrote=%zu\n", m->len);
        cg_call_finish(call);
        return;
    }
    size_t nb = m->len - m->done < m->chunk ? m->len - m->done : m->chunk;
    cg_buf_t pkt;
    cg_buf_init(&pkt);
    cg_buf_printf(&pkt, "M%llx,%zx:", (unsigned long long)(m->addr + m->done), nb);
    cg_buf_append(&pkt, m->data.data + m->done * 2, nb * 2);
    static const cg_req_ops_t ops = {mem_write_reply, NULL};
    if (cg_target_submit(cg_call_hub(call)->target, "agent", pkt.data, pkt.len, &ops, call) != 0)
        fail(call, "write memory", CG_REPLY_LINKDOWN, "", 0);
    cg_buf_free(&pkt);
}

static cg_call_rc_t cmd_wmem(cg_hub_t *h, cg_call_t *call, const char *args)
{
    char a[32];
    uint64_t addr;
    if (!tok(&args, a, sizeof(a)) || cg_parse_u64(a, 1, &addr))
        return err(call, "usage: WMEM ADDR HEX");
    while (*args == ' ')
        args++;
    size_t hl = strlen(args);
    if (hl == 0 || hl % 2 || hl / 2 > MEM_MAX)
        return err(call, "HEX must be an even number of digits, at most 64 KiB");
    for (size_t i = 0; i < hl; i++)
        if (cg_hex_val((unsigned char)args[i]) < 0)
            return err(call, "HEX has a non-hex digit");
    mem_t *m = mem_new(call, addr, hl / 2, chunk_for(h, 32, 0x800));
    if (!m)
        return err(call, "out of memory");
    cg_buf_append(&m->data, args, hl);
    mem_write_next(call);
    return CG_CALL_PENDING;
}

/* ---- breakpoints ------------------------------------------------------- */

typedef struct {
    int insert;
    int type;
    uint64_t addr;
    unsigned kind; /* 0 = pick a default */
} bp_t;

static int bp_type(const char *s)
{
    static const char *const names[] = {"sw", "hw", "write", "read", "access"};
    for (int i = 0; i < 5; i++)
        if (strcasecmp(s, names[i]) == 0)
            return i;
    return -1;
}

static void bp_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    bp_t *b = cg_call_user(call);
    if (st != CG_REPLY_OK || !(n == 2 && memcmp(p, "OK", 2) == 0)) {
        fail(call, b->insert ? "insert breakpoint" : "remove breakpoint", st, p, n);
        return;
    }
    if (b->insert)
        cg_hub_bp_add(h, b->type, b->addr, b->kind);
    else
        cg_hub_bp_del(h, b->type, b->addr, b->kind);
    cg_buf_printf(cg_call_out(call), "OK addr=0x%llx kind=%u\n", (unsigned long long)b->addr,
                  b->kind);
    cg_call_finish(call);
}

static void bp_ready(void *ctx, int ok)
{
    (void)ok;
    cg_call_t *call = ctx;
    cg_hub_t *h = cg_call_hub(call);
    bp_t *b = cg_call_user(call);
    if (!b->kind)
        b->kind = b->type <= 1 ? cg_regmap_default_bp_kind(h->regmap) : 4;
    char pkt[64];
    int k = snprintf(pkt, sizeof(pkt), "%c%d,%llx,%x", b->insert ? 'Z' : 'z', b->type,
                     (unsigned long long)b->addr, b->kind);
    static const cg_req_ops_t ops = {bp_reply, NULL};
    if (cg_target_submit(h->target, "agent", pkt, (size_t)k, &ops, call) != 0)
        fail(call, "breakpoint", CG_REPLY_LINKDOWN, "", 0);
}

static cg_call_rc_t bp_cmd(cg_hub_t *h, cg_call_t *call, const char *args, int insert)
{
    char a[32], t[16] = "sw", k[16] = "";
    uint64_t addr, kind = 0;
    if (!tok(&args, a, sizeof(a)) || cg_parse_u64(a, 1, &addr))
        return err(call, "usage: BREAK|UNBREAK ADDR [sw|hw|write|read|access] [KIND]");
    tok(&args, t, sizeof(t));
    tok(&args, k, sizeof(k));
    int type = bp_type(t);
    if (type < 0 || (k[0] && cg_parse_u64(k, 0, &kind)))
        return err(call, "usage: BREAK|UNBREAK ADDR [sw|hw|write|read|access] [KIND]");
    bp_t *b = calloc(1, sizeof(*b));
    if (!b)
        return err(call, "out of memory");
    *b = (bp_t){insert, type, addr, (unsigned)kind};
    cg_call_set_user(call, b, free);
    cg_regmap_ensure(h->regmap, bp_ready, call);
    return CG_CALL_PENDING;
}

static cg_call_rc_t cmd_break(cg_hub_t *h, cg_call_t *call, const char *args)
{
    return bp_cmd(h, call, args, 1);
}

static cg_call_rc_t cmd_unbreak(cg_hub_t *h, cg_call_t *call, const char *args)
{
    return bp_cmd(h, call, args, 0);
}

/* ---- pass-through ------------------------------------------------------ */

static void monitor_output(void *ctx, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_buf_t *text = cg_call_user(call);
    size_t nb = (n - 1) / 2;
    if (cg_buf_reserve(text, nb) != 0)
        return;
    long got = cg_hex_decode(p + 1, nb * 2, (uint8_t *)text->data + text->len, nb);
    if (got > 0) {
        text->len += (size_t)got;
        text->data[text->len] = '\0';
    }
}

static void monitor_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    cg_buf_t *text = cg_call_user(call);
    cg_buf_t *out = cg_call_out(call);
    if (st == CG_REPLY_OK && !is_error(p, n) && n) {
        cg_buf_puts(out, "OK\n");
    } else {
        cg_buf_t why;
        cg_buf_init(&why);
        if (st != CG_REPLY_OK)
            cg_buf_puts(&why, st == CG_REPLY_TIMEOUT ? "stub did not answer" : "stub link lost");
        else if (n == 0)
            cg_buf_puts(&why, "monitor commands not supported by the stub");
        else
            escaped(&why, p, n);
        cg_buf_printf(out, "ERR monitor: %s\n", why.data ? why.data : "");
        cg_buf_free(&why);
    }
    if (n && p[0] != 'O' && !is_error(p, n) && !(n == 2 && memcmp(p, "OK", 2) == 0)) {
        /* Some stubs answer with hex output instead of O packets. */
        size_t nb = n / 2;
        if (cg_buf_reserve(text, nb) == 0) {
            long got = cg_hex_decode(p, nb * 2, (uint8_t *)text->data + text->len, nb);
            if (got > 0) {
                text->len += (size_t)got;
                text->data[text->len] = '\0';
            }
        }
    }
    if (text->len)
        cg_buf_append(out, text->data, text->len);
    if (text->len && text->data[text->len - 1] != '\n')
        cg_buf_putc(out, '\n');
    cg_call_finish(call);
}

static cg_call_rc_t cmd_monitor(cg_hub_t *h, cg_call_t *call, const char *args)
{
    cg_buf_t pkt;
    cg_buf_init(&pkt);
    cg_buf_puts(&pkt, "qRcmd,");
    cg_hex_encode(&pkt, (const uint8_t *)args, strlen(args));
    if (!user_text(call, "")) {
        cg_buf_free(&pkt);
        return err(call, "out of memory");
    }
    cg_buf_reset(cg_call_user(call));
    static const cg_req_ops_t ops = {monitor_reply, monitor_output};
    int rc = cg_target_submit(h->target, "agent", pkt.data, pkt.len, &ops, call);
    cg_buf_free(&pkt);
    if (rc != 0)
        return err(call, "stub link is down");
    return CG_CALL_PENDING;
}

static void raw_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_call_t *call = ctx;
    if (st != CG_REPLY_OK) {
        fail(call, "raw", st, p, n);
        return;
    }
    cg_buf_t *out = cg_call_out(call);
    cg_buf_printf(out, "OK bytes=%zu\n", n);
    escaped(out, p, n);
    cg_buf_putc(out, '\n');
    cg_call_finish(call);
}

static cg_call_rc_t cmd_raw(cg_hub_t *h, cg_call_t *call, const char *args)
{
    size_t n = strlen(args);
    switch (cg_rsp_classify(args, n)) {
    case CG_PKT_RESUME:
        return err(call, "use CONT/STEP to resume; RAW is for packets with one reply");
    case CG_PKT_NOREPLY:
        return err(call, "RAW refuses packets that may end the stub session (k, R, r)");
    case CG_PKT_NORMAL:
        break;
    }
    static const cg_req_ops_t ops = {raw_reply, NULL};
    if (cg_target_submit(h->target, "agent", args, n, &ops, call) != 0)
        return err(call, "stub link is down");
    return CG_CALL_PENDING;
}

/* ---- hub management ---------------------------------------------------- */

static cg_call_rc_t cmd_kick(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    if (!cg_gdbseat_attached(h->seat))
        return err(call, "no gdb client attached");
    cg_gdbseat_kick(h->seat, "kicked by control client");
    cg_buf_puts(cg_call_out(call), "OK kicked\n");
    return CG_CALL_DONE;
}

static cg_call_rc_t cmd_reconnect(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    cg_target_reconnect(h->target);
    cg_buf_printf(cg_call_out(call), "OK link=%s\n",
                  cg_target_state(h->target) == CG_TS_DOWN ? "down" : "up");
    return CG_CALL_DONE;
}

static cg_call_rc_t cmd_shutdown(cg_hub_t *h, cg_call_t *call, const char *args)
{
    (void)args;
    cg_loop_stop(h->loop);
    cg_buf_puts(cg_call_out(call), "OK shutting down\n");
    return CG_CALL_DONE;
}

/* ---- table ------------------------------------------------------------- */

#define L(x) CG_LVL_##x
#define N(x) CG_NEED_##x

const cg_cmd_t cg_commands[] = {
    {"HELLO", L(READ), 0, CG_ARGS_NONE, cmd_hello, "HELLO"},
    {"PING", L(READ), 0, CG_ARGS_NONE, cmd_ping, "PING"},
    {"STATUS", L(READ), 0, CG_ARGS_NONE, cmd_status, "STATUS"},
    {"EVENTS", L(READ), 0, CG_ARGS_OPT, cmd_events, "EVENTS [N] [TYPES]"},
    {"WATCH", L(READ), 0, CG_ARGS_OPT, cmd_watch, "WATCH [TYPES]"},
    {"BREAKS", L(READ), 0, CG_ARGS_NONE, cmd_breaks, "BREAKS"},
    {"WAIT", L(READ), N(LINK), CG_ARGS_OPT, cmd_wait, "WAIT [TIMEOUT_MS]"},
    {"REGS", L(READ), N(STOPPED), CG_ARGS_OPT, cmd_regs, "REGS [all|NAME,NAME...]"},
    {"REG", L(READ), N(STOPPED), CG_ARGS_REQ, cmd_reg, "REG NAME"},
    {"MEM", L(READ), N(STOPPED), CG_ARGS_REQ, cmd_mem, "MEM ADDR LEN"},
    {"HALT", L(HALT), N(LINK), CG_ARGS_OPT, cmd_halt, "HALT [TIMEOUT_MS]"},
    {"CONT", L(RUN), N(STOPPED), CG_ARGS_OPT, cmd_cont, "CONT [WAIT_MS]"},
    {"STEP", L(RUN), N(STOPPED), CG_ARGS_NONE, cmd_step, "STEP"},
    {"BREAK", L(RUN), N(STOPPED), CG_ARGS_REQ, cmd_break,
     "BREAK ADDR [sw|hw|write|read|access] [KIND]"},
    {"UNBREAK", L(RUN), N(STOPPED), CG_ARGS_REQ, cmd_unbreak,
     "UNBREAK ADDR [sw|hw|write|read|access] [KIND]"},
    {"WMEM", L(WRITE), N(STOPPED), CG_ARGS_REQ, cmd_wmem, "WMEM ADDR HEX"},
    {"WREG", L(WRITE), N(STOPPED), CG_ARGS_REQ, cmd_wreg, "WREG NAME VALUE"},
    {"MONITOR", L(WRITE), N(IDLE), CG_ARGS_REQ, cmd_monitor, "MONITOR TEXT"},
    {"RAW", L(WRITE), N(IDLE), CG_ARGS_REQ, cmd_raw, "RAW PACKET"},
    {"KICK", L(WRITE), 0, CG_ARGS_NONE, cmd_kick, "KICK"},
    {"RECONNECT", L(WRITE), 0, CG_ARGS_NONE, cmd_reconnect, "RECONNECT"},
    {"SHUTDOWN", L(WRITE), 0, CG_ARGS_NONE, cmd_shutdown, "SHUTDOWN"},
    {NULL, 0, 0, 0, NULL, NULL},
};
