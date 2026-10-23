#include "target.h"

#include <stdlib.h>
#include <string.h>

#include "core/conn.h"
#include "rsp/rsp.h"
#include "transport/transport.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/sys.h"

#define TICK_MS 100
#define NOREPLY_GRACE_MS 1500
#define MAX_RETRANSMIT 5
#define BACKOFF_MIN_MS 250

/* Sent once per link-up so the hub knows the packet size and whether it
 * can fetch target.xml before any gdb shows up. */
static const char k_hub_supported[] = "qSupported:swbreak+;hwbreak+;xmlRegisters=i386";

typedef struct req {
    struct req *next;
    char *payload;
    size_t len;
    const char *origin;
    cg_req_ops_t ops;
    void *ctx;
    cg_pkt_class_t cls;
    int is_qrcmd;
    int is_status; /* "?" */
    uint64_t deadline_ms;
} req_t;

struct cg_target {
    cg_loop_t *loop;
    cg_bus_t *bus;
    cg_target_cfg_t cfg;
    char *uri;

    cg_conn_t *conn;
    cg_rsp_dec_t dec;
    cg_tstate_t state;
    cg_target_caps_t caps;

    req_t *head, *tail;
    size_t qlen;
    req_t *inflight;
    cg_buf_t last_frame;
    int retransmits;
    unsigned stale; /* late replies still owed by timed-out requests */

    cg_stop_t last_stop;
    int have_stop;
    const char *run_origin;

    uint64_t tick_timer;
    uint64_t reconnect_timer;
    unsigned backoff_ms;
    int started;
};

static void pump(cg_target_t *t);
static void schedule_reconnect(cg_target_t *t);

static const cg_conn_ops_t k_conn_ops;

/* ---- requests ---------------------------------------------------------- */

static void req_free(req_t *r)
{
    if (!r)
        return;
    free(r->payload);
    free(r);
}

static void complete(cg_target_t *t, req_t *r, cg_reply_status_t st, const char *data, size_t n)
{
    if (t->inflight == r)
        t->inflight = NULL;
    if (r->ops.on_reply)
        r->ops.on_reply(r->ctx, st, data ? data : "", n);
    req_free(r);
}

static void fail_all(cg_target_t *t, cg_reply_status_t st)
{
    req_t *r = t->inflight;
    t->inflight = NULL;
    if (r)
        complete(t, r, st, "", 0);
    while (t->head) {
        r = t->head;
        t->head = r->next;
        t->qlen--;
        complete(t, r, st, "", 0);
    }
    t->tail = NULL;
}

static unsigned timeout_for(const cg_target_t *t, const req_t *r)
{
    if (r->is_qrcmd || cg_rsp_starts(r->payload, r->len, "vFlash") ||
        cg_rsp_starts(r->payload, r->len, "vRun") ||
        cg_rsp_starts(r->payload, r->len, "vAttach"))
        return t->cfg.slow_timeout_ms;
    return t->cfg.timeout_ms;
}

static void wire_send(cg_target_t *t, const char *origin, const char *payload, size_t n, int keep)
{
    cg_buf_t frame;
    cg_buf_init(&frame);
    cg_rsp_frame(&frame, payload, n);
    cg_bus_packet(t->bus, origin, '>', payload, n);
    if (keep) {
        cg_buf_reset(&t->last_frame);
        cg_buf_append(&t->last_frame, frame.data, frame.len);
        t->retransmits = 0;
    }
    if (t->conn)
        cg_conn_write(t->conn, frame.data, frame.len);
    cg_buf_free(&frame);
}

static void set_state(cg_target_t *t, cg_tstate_t s)
{
    t->state = s;
}

static void pump(cg_target_t *t)
{
    while (!t->inflight && t->head && t->conn && t->state != CG_TS_RUNNING) {
        req_t *r = t->head;
        t->head = r->next;
        if (!t->head)
            t->tail = NULL;
        t->qlen--;
        r->next = NULL;
        t->inflight = r;

        uint64_t now = cg_now_ms();
        r->deadline_ms = 0;
        if (r->cls == CG_PKT_NOREPLY)
            r->deadline_ms = now + NOREPLY_GRACE_MS;
        else if (r->cls != CG_PKT_RESUME)
            r->deadline_ms = now + timeout_for(t, r);

        wire_send(t, r->origin, r->payload, r->len, 1);
        if (!t->conn)
            return; /* write failed and the link went down under us */
        if (r->cls == CG_PKT_RESUME) {
            set_state(t, CG_TS_RUNNING);
            t->run_origin = r->origin;
            cg_bus_emitf(t->bus, CG_EV_RUN, r->origin, "%.*s", (int)(r->len > 64 ? 64 : r->len),
                         r->payload);
        }
    }
}

int cg_target_submit(cg_target_t *t, const char *origin, const char *payload, size_t n,
                     const cg_req_ops_t *ops, void *ctx)
{
    if (!t->conn)
        return -1;
    req_t *r = calloc(1, sizeof(*r));
    if (!r)
        return -1;
    r->payload = malloc(n + 1);
    if (!r->payload) {
        free(r);
        return -1;
    }
    memcpy(r->payload, payload, n);
    r->payload[n] = '\0';
    r->len = n;
    r->origin = origin;
    if (ops)
        r->ops = *ops;
    r->ctx = ctx;
    r->cls = cg_rsp_classify(payload, n);
    r->is_qrcmd = cg_rsp_starts(payload, n, "qRcmd,");
    r->is_status = (n == 1 && payload[0] == '?');
    if (t->tail)
        t->tail->next = r;
    else
        t->head = r;
    t->tail = r;
    t->qlen++;
    pump(t);
    return 0;
}

void cg_target_forget(cg_target_t *t, void *ctx)
{
    if (t->inflight && t->inflight->ctx == ctx) {
        memset(&t->inflight->ops, 0, sizeof(t->inflight->ops));
        t->inflight->ctx = NULL;
    }
    req_t **pp = &t->head;
    t->tail = NULL;
    while (*pp) {
        req_t *r = *pp;
        if (r->ctx == ctx) {
            *pp = r->next;
            t->qlen--;
            req_free(r);
            continue;
        }
        t->tail = r;
        pp = &r->next;
    }
}

int cg_target_interrupt(cg_target_t *t, const char *origin)
{
    if (t->state != CG_TS_RUNNING || !t->conn)
        return 1;
    static const char ctrl_c = 0x03;
    cg_bus_emitf(t->bus, CG_EV_NOTE, origin, "interrupt");
    cg_bus_packet(t->bus, origin, '>', "\\x03", 4);
    cg_conn_write(t->conn, &ctrl_c, 1);
    return 0;
}

int cg_target_send_raw(cg_target_t *t, const char *origin, const char *payload, size_t n)
{
    if (!t->conn)
        return -1;
    wire_send(t, origin, payload, n, 1);
    return 0;
}

/* ---- replies ----------------------------------------------------------- */

static void record_stop(cg_target_t *t, const char *raw, size_t n, int announce)
{
    cg_buf_t ex;
    cg_buf_init(&ex);
    if (cg_rsp_expand_rle(raw, n, &ex) != 0 ||
        cg_stop_parse(ex.data ? ex.data : "", ex.len, &t->last_stop) != 0) {
        cg_buf_free(&ex);
        return;
    }
    cg_buf_free(&ex);
    t->have_stop = 1;
    cg_tstate_t ns = (t->last_stop.kind == CG_STOP_EXITED ||
                      t->last_stop.kind == CG_STOP_TERMINATED)
                         ? CG_TS_EXITED
                         : CG_TS_STOPPED;
    int changed = t->state != ns;
    set_state(t, ns);
    if (!announce && !changed)
        return;
    cg_buf_t text;
    cg_buf_init(&text);
    cg_stop_describe(&t->last_stop, &text);
    cg_event_t ev = {
        .type = CG_EV_STOP,
        .origin = "target",
        .text = text.data,
        .len = text.len,
        .stop = &t->last_stop,
    };
    cg_bus_publish(t->bus, &ev);
    cg_buf_free(&text);
}

static void publish_console(cg_target_t *t, const char *p, size_t n)
{
    uint8_t *bytes = malloc(n / 2 + 1);
    if (!bytes)
        return;
    long got = cg_hex_decode(p + 1, n - 1, bytes, n / 2 + 1);
    if (got > 0) {
        cg_event_t ev = {
            .type = CG_EV_CONSOLE,
            .origin = "target",
            .text = (const char *)bytes,
            .len = (size_t)got,
        };
        cg_bus_publish(t->bus, &ev);
    }
    free(bytes);
}

static void parse_supported(cg_target_t *t, const char *p, size_t n)
{
    const char *end = p + n;
    while (p < end) {
        const char *semi = memchr(p, ';', (size_t)(end - p));
        size_t k = semi ? (size_t)(semi - p) : (size_t)(end - p);
        if (k > 11 && memcmp(p, "PacketSize=", 11) == 0) {
            char tmp[16];
            size_t vn = k - 11 < sizeof(tmp) - 1 ? k - 11 : sizeof(tmp) - 1;
            memcpy(tmp, p + 11, vn);
            tmp[vn] = '\0';
            t->caps.packet_size = (unsigned)strtoul(tmp, NULL, 16);
        } else if (k == 20 && memcmp(p, "qXfer:features:read+", 20) == 0) {
            t->caps.xfer_features = 1;
        }
        p += k + 1;
    }
}

static void on_packet(cg_target_t *t, const char *p, size_t n)
{
    static const char ack = '+';
    cg_conn_write(t->conn, &ack, 1);
    if (!t->conn)
        return;
    cg_bus_packet(t->bus, "target", '<', p, n);

    req_t *r = t->inflight;
    int console = cg_rsp_is_console(p, n);
    int stop = cg_rsp_is_stop(p, n);

    if (!r) {
        if (t->stale) {
            t->stale--;
            cg_bus_emitf(t->bus, CG_EV_NOTE, "hub", "dropped late reply (%zu bytes)", n);
        } else if (stop) {
            record_stop(t, p, n, 1);
        } else if (console) {
            publish_console(t, p, n);
        } else {
            cg_bus_emitf(t->bus, CG_EV_NOTE, "hub", "unsolicited packet (%zu bytes)", n);
        }
        return;
    }

    if (r->cls == CG_PKT_RESUME) {
        if (console) {
            publish_console(t, p, n);
            if (r->ops.on_output)
                r->ops.on_output(r->ctx, p, n);
            return;
        }
        if (n && p[0] == 'F') {
            if (r->ops.on_output)
                r->ops.on_output(r->ctx, p, n);
            return;
        }
        if (stop)
            record_stop(t, p, n, 1);
        else
            set_state(t, CG_TS_STOPPED); /* resume refused, e.g. E01 */
        t->run_origin = NULL;
        complete(t, r, CG_REPLY_OK, p, n);
        pump(t);
        return;
    }

    if (r->is_qrcmd && console) {
        publish_console(t, p, n);
        if (r->ops.on_output)
            r->ops.on_output(r->ctx, p, n);
        return;
    }
    if (r->is_status && stop)
        record_stop(t, p, n, 0);
    complete(t, r, CG_REPLY_OK, p, n);
    pump(t);
}

static void on_msg(void *ctx, const cg_rsp_msg_t *m)
{
    cg_target_t *t = ctx;
    if (!t->conn)
        return;
    switch (m->kind) {
    case CG_RSP_PACKET:
        on_packet(t, m->data, m->len);
        break;
    case CG_RSP_NAK:
        if (t->last_frame.len && t->retransmits++ < MAX_RETRANSMIT)
            cg_conn_write(t->conn, t->last_frame.data, t->last_frame.len);
        break;
    case CG_RSP_BADSUM: {
        static const char nak = '-';
        cg_bus_emitf(t->bus, CG_EV_NOTE, "hub", "bad checksum from stub");
        cg_conn_write(t->conn, &nak, 1);
        break;
    }
    case CG_RSP_NOTIFY:
        cg_bus_emitf(t->bus, CG_EV_NOTE, "target", "notification %.*s", (int)m->len, m->data);
        break;
    case CG_RSP_ACK:
    case CG_RSP_INTERRUPT:
        break;
    }
}

/* ---- link -------------------------------------------------------------- */

static void conn_data(void *ctx, const uint8_t *data, size_t n)
{
    cg_target_t *t = ctx;
    cg_rsp_feed(&t->dec, data, n, on_msg, t);
}

static void link_down(cg_target_t *t, const char *why)
{
    t->conn = NULL;
    set_state(t, CG_TS_DOWN);
    t->run_origin = NULL;
    t->stale = 0;
    memset(&t->caps, 0, sizeof(t->caps));
    cg_rsp_dec_reset(&t->dec);
    cg_bus_emitf(t->bus, CG_EV_LINK, "hub", "down %s", why);
    fail_all(t, CG_REPLY_LINKDOWN);
    schedule_reconnect(t);
}

static void conn_closed(void *ctx)
{
    link_down(ctx, "stub closed the connection");
}

static const cg_conn_ops_t k_conn_ops = {conn_data, conn_closed};

static void supported_reply(void *ctx, cg_reply_status_t st, const char *d, size_t n)
{
    cg_target_t *t = ctx;
    if (st == CG_REPLY_OK)
        parse_supported(t, d, n);
}

static void status_reply(void *ctx, cg_reply_status_t st, const char *d, size_t n)
{
    cg_target_t *t = ctx;
    if (st == CG_REPLY_OK && !cg_rsp_is_stop(d, n) && t->state == CG_TS_UNKNOWN)
        CG_WARN("stub answered '?' with %.*s", (int)n, d);
}

static int try_connect(cg_target_t *t)
{
    char err[256] = "";
    int fd = cg_transport_open(t->uri, err, sizeof(err));
    if (fd < 0) {
        CG_DEBUG("%s", err);
        return -1;
    }
    t->conn = cg_conn_new(t->loop, fd, &k_conn_ops, t);
    if (!t->conn)
        return -1;
    t->backoff_ms = 0;
    set_state(t, CG_TS_UNKNOWN);
    cg_rsp_dec_reset(&t->dec);
    cg_bus_emitf(t->bus, CG_EV_LINK, "hub", "up %s", t->uri);
    static const cg_req_ops_t sup = {supported_reply, NULL};
    static const cg_req_ops_t stat = {status_reply, NULL};
    cg_target_submit(t, "hub", k_hub_supported, sizeof(k_hub_supported) - 1, &sup, t);
    cg_target_submit(t, "hub", "?", 1, &stat, t);
    return 0;
}

static void reconnect_fire(void *ctx)
{
    cg_target_t *t = ctx;
    t->reconnect_timer = 0;
    if (t->conn)
        return;
    if (try_connect(t) != 0)
        schedule_reconnect(t);
}

static void schedule_reconnect(cg_target_t *t)
{
    if (!t->started || t->reconnect_timer)
        return;
    unsigned max = t->cfg.reconnect_max_ms ? t->cfg.reconnect_max_ms : 8000;
    t->backoff_ms = t->backoff_ms ? t->backoff_ms * 2 : BACKOFF_MIN_MS;
    if (t->backoff_ms > max)
        t->backoff_ms = max;
    t->reconnect_timer = cg_loop_timer(t->loop, t->backoff_ms, reconnect_fire, t);
}

static void tick(void *ctx)
{
    cg_target_t *t = ctx;
    t->tick_timer = cg_loop_timer(t->loop, TICK_MS, tick, t);
    req_t *r = t->inflight;
    if (!r || !r->deadline_ms || cg_now_ms() < r->deadline_ms)
        return;
    if (r->cls == CG_PKT_NOREPLY) {
        complete(t, r, CG_REPLY_OK, "", 0);
    } else {
        t->stale++;
        cg_bus_emitf(t->bus, CG_EV_NOTE, "hub", "timeout waiting for reply to %.*s",
                     (int)(r->len > 32 ? 32 : r->len), r->payload);
        complete(t, r, CG_REPLY_TIMEOUT, "", 0);
    }
    pump(t);
}

void cg_target_start(cg_target_t *t)
{
    t->started = 1;
    if (!t->tick_timer)
        t->tick_timer = cg_loop_timer(t->loop, TICK_MS, tick, t);
    if (!t->conn && try_connect(t) != 0) {
        CG_WARN("stub %s not reachable yet; retrying", t->uri);
        schedule_reconnect(t);
    }
}

void cg_target_reconnect(cg_target_t *t)
{
    if (t->conn) {
        cg_conn_t *c = t->conn;
        link_down(t, "reconnect requested");
        cg_conn_close(c);
    }
    if (t->reconnect_timer) {
        cg_loop_cancel(t->loop, t->reconnect_timer);
        t->reconnect_timer = 0;
    }
    t->backoff_ms = 0;
    reconnect_fire(t);
}

/* ---- lifecycle / queries ----------------------------------------------- */

cg_target_t *cg_target_new(cg_loop_t *loop, cg_bus_t *bus, const cg_target_cfg_t *cfg)
{
    cg_target_t *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->loop = loop;
    t->bus = bus;
    t->cfg = *cfg;
    t->uri = strdup(cfg->uri);
    t->cfg.uri = t->uri;
    if (!t->cfg.timeout_ms)
        t->cfg.timeout_ms = 10000;
    if (!t->cfg.slow_timeout_ms)
        t->cfg.slow_timeout_ms = 120000;
    cg_rsp_dec_init(&t->dec);
    cg_buf_init(&t->last_frame);
    t->state = CG_TS_DOWN;
    return t;
}

void cg_target_free(cg_target_t *t)
{
    if (!t)
        return;
    t->started = 0;
    if (t->conn) {
        cg_conn_t *c = t->conn;
        t->conn = NULL;
        cg_conn_close(c);
    }
    fail_all(t, CG_REPLY_LINKDOWN);
    if (t->tick_timer)
        cg_loop_cancel(t->loop, t->tick_timer);
    if (t->reconnect_timer)
        cg_loop_cancel(t->loop, t->reconnect_timer);
    cg_rsp_dec_free(&t->dec);
    cg_buf_free(&t->last_frame);
    free(t->uri);
    free(t);
}

cg_tstate_t cg_target_state(const cg_target_t *t)
{
    return t->state;
}

const char *cg_tstate_name(cg_tstate_t s)
{
    switch (s) {
    case CG_TS_DOWN: return "down";
    case CG_TS_UNKNOWN: return "unknown";
    case CG_TS_STOPPED: return "stopped";
    case CG_TS_RUNNING: return "running";
    case CG_TS_EXITED: return "exited";
    }
    return "?";
}

const cg_stop_t *cg_target_last_stop(const cg_target_t *t)
{
    return t->have_stop ? &t->last_stop : NULL;
}

const char *cg_target_run_origin(const cg_target_t *t)
{
    return t->run_origin;
}

const cg_target_caps_t *cg_target_caps(const cg_target_t *t)
{
    return &t->caps;
}

size_t cg_target_queue_len(const cg_target_t *t)
{
    return t->qlen + (t->inflight ? 1 : 0);
}

const char *cg_target_uri(const cg_target_t *t)
{
    return t->uri;
}
