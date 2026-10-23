#include "gdbseat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "core/conn.h"
#include "rsp/rsp.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/sys.h"

typedef struct client client_t;

struct cg_gdbseat {
    cg_loop_t *loop;
    cg_bus_t *bus;
    cg_target_t *target;
    cg_gdbseat_cfg_t cfg;
    int listen_fd;
    int port;
    uint64_t sub;
    client_t *client;
};

struct client {
    cg_gdbseat_t *seat;
    cg_conn_t *conn;
    cg_rsp_dec_t dec;
    int noack;
    int rewrite_supported;
    cg_buf_t last; /* last frame sent, for '-' */
    char peer[64];
    int busy; /* inside client_data: defer the free */
    int dead;
};

typedef int (*local_fn)(client_t *c, const char *p, size_t n);

static void client_drop(client_t *c, const char *why);

/* ---- sending ----------------------------------------------------------- */

static void send_packet(client_t *c, const char *p, size_t n)
{
    if (!c->conn)
        return;
    cg_buf_reset(&c->last);
    cg_rsp_frame(&c->last, p, n);
    cg_bus_packet(c->seat->bus, "gdb", '<', p, n);
    cg_conn_write(c->conn, c->last.data, c->last.len);
}

static void send_str(client_t *c, const char *s)
{
    send_packet(c, s, strlen(s));
}

/* ---- target callbacks -------------------------------------------------- */

static void on_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    client_t *c = ctx;
    if (st == CG_REPLY_LINKDOWN) {
        client_drop(c, "stub link lost");
        return;
    }
    if (st == CG_REPLY_TIMEOUT) {
        send_str(c, "E01");
        return;
    }
    if (c->rewrite_supported) {
        c->rewrite_supported = 0;
        /* The hop to gdb is always reliable, so offer no-ack mode even
         * when the stub (e.g. on a UART) does not. */
        if (!memmem(p, n, "QStartNoAckMode+", 16)) {
            cg_buf_t b;
            cg_buf_init(&b);
            cg_buf_append(&b, p, n);
            cg_buf_puts(&b, n ? ";QStartNoAckMode+" : "QStartNoAckMode+");
            send_packet(c, b.data, b.len);
            cg_buf_free(&b);
            return;
        }
    }
    send_packet(c, p, n);
}

static void on_output(void *ctx, const char *p, size_t n)
{
    send_packet(ctx, p, n);
}

static const cg_req_ops_t k_req_ops = {on_reply, on_output};

/* ---- packets answered on this hop -------------------------------------- */

static int local_noack(client_t *c, const char *p, size_t n)
{
    (void)p;
    (void)n;
    send_str(c, "OK");
    c->noack = 1;
    return 1;
}

static int local_nonstop(client_t *c, const char *p, size_t n)
{
    if (n >= 10 && p[9] == '1') {
        send_str(c, "E01");
        return 1;
    }
    return 0;
}

static int local_detach(client_t *c, const char *p, size_t n)
{
    cg_gdbseat_t *s = c->seat;
    if (s->cfg.detach == CG_DETACH_FORWARD)
        return 0;
    int is_kill = p[0] == 'k' || cg_rsp_starts(p, n, "vKill;");
    if (p[0] != 'k') /* bare k expects no answer; D and vKill do */
        send_str(c, "OK");
    cg_bus_emitf(s->bus, CG_EV_NOTE, "gdb", "%s answered by hub (%s)",
                 is_kill ? "kill" : "detach", cg_detach_name(s->cfg.detach));
    if (s->cfg.detach == CG_DETACH_RESUME && cg_target_state(s->target) == CG_TS_STOPPED)
        cg_target_submit(s->target, "hub", "c", 1, NULL, NULL);
    return 1;
}

static int local_monitor(client_t *c, const char *p, size_t n)
{
    cg_gdbseat_t *s = c->seat;
    if (!s->cfg.monitor || (n - 6) % 2)
        return 0;
    size_t len = (n - 6) / 2;
    char *text = malloc(len + 1);
    if (!text || cg_hex_decode(p + 6, n - 6, (uint8_t *)text, len) != (long)len) {
        free(text);
        return 0;
    }
    text[len] = '\0';
    int ours = strncmp(text, "cogdb", 5) == 0 && (text[5] == '\0' || text[5] == ' ');
    if (ours) {
        cg_buf_t out, pkt;
        cg_buf_init(&out);
        cg_buf_init(&pkt);
        s->cfg.monitor(s->cfg.monitor_ctx, text + 5, &out);
        if (out.len) {
            cg_buf_putc(&pkt, 'O');
            cg_hex_encode(&pkt, (const uint8_t *)out.data, out.len);
            send_packet(c, pkt.data, pkt.len);
        }
        send_str(c, "OK");
        cg_buf_free(&out);
        cg_buf_free(&pkt);
    }
    free(text);
    return ours;
}

static const struct {
    const char *prefix;
    int exact;
    local_fn fn;
} k_local[] = {
    {"QStartNoAckMode", 1, local_noack},
    {"qRcmd,", 0, local_monitor},
    {"QNonStop:", 0, local_nonstop},
    {"D", 1, local_detach},
    {"D;", 0, local_detach},
    {"k", 1, local_detach},
    {"vKill;", 0, local_detach},
};

static int try_local(client_t *c, const char *p, size_t n)
{
    for (size_t i = 0; i < sizeof(k_local) / sizeof(k_local[0]); i++) {
        size_t k = strlen(k_local[i].prefix);
        if (k_local[i].exact ? (n == k && memcmp(p, k_local[i].prefix, k) == 0)
                             : cg_rsp_starts(p, n, k_local[i].prefix))
            if (k_local[i].fn(c, p, n))
                return 1;
    }
    return 0;
}

/* ---- client input ------------------------------------------------------ */

static void handle_packet(client_t *c, const char *p, size_t n)
{
    cg_gdbseat_t *s = c->seat;
    cg_bus_packet(s->bus, "gdb", '>', p, n);
    if (try_local(c, p, n))
        return;
    if (n && p[0] == 'F' && cg_target_state(s->target) == CG_TS_RUNNING) {
        cg_target_send_raw(s->target, "gdb", p, n);
        return;
    }
    c->rewrite_supported = cg_rsp_starts(p, n, "qSupported");
    if (cg_target_submit(s->target, "gdb", p, n, &k_req_ops, c) != 0)
        send_str(c, "E01");
}

static void on_msg(void *ctx, const cg_rsp_msg_t *m)
{
    client_t *c = ctx;
    if (!c->conn)
        return;
    switch (m->kind) {
    case CG_RSP_PACKET:
        if (!c->noack)
            cg_conn_write(c->conn, "+", 1);
        handle_packet(c, m->data, m->len);
        break;
    case CG_RSP_BADSUM:
        if (!c->noack)
            cg_conn_write(c->conn, "-", 1);
        break;
    case CG_RSP_NAK:
        if (c->last.len)
            cg_conn_write(c->conn, c->last.data, c->last.len);
        break;
    case CG_RSP_INTERRUPT:
        cg_target_interrupt(c->seat->target, "gdb");
        break;
    case CG_RSP_ACK:
    case CG_RSP_NOTIFY:
        break;
    }
}

static void client_data(void *ctx, const uint8_t *data, size_t n)
{
    client_t *c = ctx;
    c->busy++;
    cg_rsp_feed(&c->dec, data, n, on_msg, c);
    c->busy--;
    if (c->dead) {
        cg_rsp_dec_free(&c->dec);
        cg_buf_free(&c->last);
        free(c);
    }
}

static void client_free(client_t *c)
{
    cg_gdbseat_t *s = c->seat;
    cg_target_forget(s->target, c);
    if (s->client == c)
        s->client = NULL;
    if (c->busy) {
        c->dead = 1;
        return;
    }
    cg_rsp_dec_free(&c->dec);
    cg_buf_free(&c->last);
    free(c);
}

static void client_closed(void *ctx)
{
    client_t *c = ctx;
    cg_bus_emitf(c->seat->bus, CG_EV_CLIENT, "gdb", "detached %s", c->peer);
    c->conn = NULL;
    client_free(c);
}

static void client_drop(client_t *c, const char *why)
{
    cg_conn_t *conn = c->conn;
    cg_bus_emitf(c->seat->bus, CG_EV_CLIENT, "gdb", "dropped %s: %s", c->peer, why);
    c->conn = NULL;
    client_free(c);
    if (conn)
        cg_conn_close_after_flush(conn);
}

static const cg_conn_ops_t k_client_ops = {client_data, client_closed};

/* ---- listener ---------------------------------------------------------- */

static void on_accept(void *ctx, int fd, unsigned ev)
{
    (void)ev;
    cg_gdbseat_t *s = ctx;
    struct sockaddr_in sa;
    socklen_t sl = sizeof(sa);
    int cfd = accept(fd, (struct sockaddr *)&sa, &sl);
    if (cfd < 0)
        return;
    char peer[64];
    snprintf(peer, sizeof(peer), "%s:%d", inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));

    if (s->client) {
        if (!s->cfg.takeover) {
            cg_bus_emitf(s->bus, CG_EV_CLIENT, "gdb", "refused %s: %s already attached", peer,
                         s->client->peer);
            close(cfd);
            return;
        }
        client_drop(s->client, "taken over");
    }
    if (cg_target_state(s->target) == CG_TS_DOWN) {
        cg_bus_emitf(s->bus, CG_EV_CLIENT, "gdb", "refused %s: stub link is down", peer);
        close(cfd);
        return;
    }
    int one = 1;
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        close(cfd);
        return;
    }
    c->seat = s;
    cg_rsp_dec_init(&c->dec);
    cg_buf_init(&c->last);
    snprintf(c->peer, sizeof(c->peer), "%s", peer);
    c->conn = cg_conn_new(s->loop, cfd, &k_client_ops, c);
    if (!c->conn) {
        close(cfd);
        cg_rsp_dec_free(&c->dec);
        free(c);
        return;
    }
    s->client = c;
    cg_bus_emitf(s->bus, CG_EV_CLIENT, "gdb", "attached %s", peer);
    /* All-stop gdb expects a halted target on connect (gdbserver and OpenOCD
     * halt on attach); while it runs the hub holds gdb's qSupported and gdb
     * gives up after its 2 s remotetimeout. */
    if (cg_target_state(s->target) == CG_TS_RUNNING) {
        cg_bus_emitf(s->bus, CG_EV_NOTE, "gdb", "halting running target for gdb attach");
        cg_target_interrupt(s->target, "gdb");
    }
}

static void on_link(void *ctx, const cg_event_t *ev)
{
    cg_gdbseat_t *s = ctx;
    if (s->client && cg_rsp_starts(ev->text, ev->len, "down"))
        client_drop(s->client, "stub link lost");
}

static int listen_tcp(const char *host, int port, int *bound)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
        bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 4) != 0) {
        CG_ERR("gdb seat: cannot listen on %s:%d: %s", host, port, strerror(errno));
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof(sa);
    getsockname(fd, (struct sockaddr *)&sa, &sl);
    *bound = ntohs(sa.sin_port);
    cg_set_nonblock(fd);
    return fd;
}

cg_gdbseat_t *cg_gdbseat_new(cg_loop_t *loop, cg_bus_t *bus, cg_target_t *t,
                             const cg_gdbseat_cfg_t *cfg)
{
    cg_gdbseat_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->loop = loop;
    s->bus = bus;
    s->target = t;
    s->cfg = *cfg;
    if (!s->cfg.host)
        s->cfg.host = "127.0.0.1";
    s->listen_fd = listen_tcp(s->cfg.host, cfg->port, &s->port);
    if (s->listen_fd < 0 || cg_loop_add(loop, s->listen_fd, CG_IO_READ, on_accept, s) != 0) {
        if (s->listen_fd >= 0)
            close(s->listen_fd);
        free(s);
        return NULL;
    }
    s->sub = cg_bus_subscribe(bus, CG_EVM(CG_EV_LINK), on_link, s);
    return s;
}

void cg_gdbseat_free(cg_gdbseat_t *s)
{
    if (!s)
        return;
    if (s->client)
        client_drop(s->client, "hub shutting down");
    cg_bus_unsubscribe(s->bus, s->sub);
    cg_loop_del(s->loop, s->listen_fd);
    close(s->listen_fd);
    free(s);
}

int cg_gdbseat_attached(const cg_gdbseat_t *s)
{
    return s->client != NULL;
}

const char *cg_gdbseat_peer(const cg_gdbseat_t *s)
{
    return s->client ? s->client->peer : "-";
}

int cg_gdbseat_port(const cg_gdbseat_t *s)
{
    return s->port;
}

void cg_gdbseat_kick(cg_gdbseat_t *s, const char *why)
{
    if (s->client)
        client_drop(s->client, why);
}

int cg_detach_parse(const char *s, cg_detach_t *out)
{
    static const char *const names[] = {"resume", "keep", "forward"};
    for (int i = 0; i < 3; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = (cg_detach_t)i;
            return 0;
        }
    }
    return -1;
}

const char *cg_detach_name(cg_detach_t d)
{
    switch (d) {
    case CG_DETACH_RESUME: return "resume";
    case CG_DETACH_KEEP: return "keep";
    case CG_DETACH_FORWARD: return "forward";
    }
    return "?";
}
