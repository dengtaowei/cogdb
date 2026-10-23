#include "ctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/conn.h"
#include "hub_priv.h"
#include "util/log.h"

#define LINE_MAX_BYTES (256 * 1024)
#define MAX_CALLS 32
#define STREAM_LIMIT (4u << 20)

struct cg_call {
    cg_ctl_t *ctl;
    cg_conn_t *conn;
    cg_buf_t in;
    cg_buf_t out;
    int started;
    int ended;
    uint64_t sub;
    uint64_t timer;
    void *user;
    void (*user_free)(void *);
};

struct cg_ctl {
    cg_hub_t *hub;
    cg_loop_t *loop;
    int fd;
    char path[CG_PATH_MAX];
    int ncalls;
};

/* ---- call lifetime ----------------------------------------------------- */

static void release(cg_call_t *call)
{
    cg_hub_t *h = call->ctl->hub;
    cg_target_forget(h->target, call);
    cg_regmap_forget(h->regmap, call);
    if (call->sub)
        cg_bus_unsubscribe(h->bus, call->sub);
    if (call->timer)
        cg_loop_cancel(call->ctl->loop, call->timer);
    if (call->user_free)
        call->user_free(call->user);
    call->sub = call->timer = 0;
    call->user = NULL;
    call->user_free = NULL;
}

static void call_free(cg_call_t *call)
{
    release(call);
    call->ctl->ncalls--;
    cg_buf_free(&call->in);
    cg_buf_free(&call->out);
    free(call);
}

void cg_call_finish(cg_call_t *call)
{
    if (call->ended)
        return;
    call->ended = 1;
    cg_conn_t *conn = call->conn;
    call->conn = NULL;
    if (conn) {
        cg_conn_write(conn, call->out.data ? call->out.data : "", call->out.len);
        cg_conn_close_after_flush(conn);
    }
    call_free(call);
}

int cg_call_stream(cg_call_t *call, const char *data, size_t n)
{
    if (!call->conn)
        return -1;
    return cg_conn_write(call->conn, data, n);
}

cg_buf_t *cg_call_out(cg_call_t *call)
{
    return &call->out;
}

cg_hub_t *cg_call_hub(cg_call_t *call)
{
    return call->ctl->hub;
}

void cg_call_own_sub(cg_call_t *call, uint64_t sub_id)
{
    call->sub = sub_id;
}

void cg_call_own_timer(cg_call_t *call, uint64_t timer_id)
{
    call->timer = timer_id;
}

void cg_call_disarm(cg_call_t *call)
{
    cg_hub_t *h = call->ctl->hub;
    if (call->sub)
        cg_bus_unsubscribe(h->bus, call->sub);
    if (call->timer)
        cg_loop_cancel(call->ctl->loop, call->timer);
    call->sub = call->timer = 0;
}

void cg_call_set_user(cg_call_t *call, void *user, void (*free_fn)(void *))
{
    call->user = user;
    call->user_free = free_fn;
}

void *cg_call_user(cg_call_t *call)
{
    return call->user;
}

/* ---- dispatch ---------------------------------------------------------- */

static const cg_cmd_t *find_cmd(const char *line, const char **args)
{
    size_t k = strcspn(line, " ");
    for (const cg_cmd_t *c = cg_commands; c->name; c++) {
        if (strlen(c->name) == k && strncasecmp(line, c->name, k) == 0) {
            const char *a = line + k;
            while (*a == ' ')
                a++;
            *args = a;
            return c;
        }
    }
    return NULL;
}

/* Returns NULL if the command may run, or the reason it may not. */
static const char *gate(cg_hub_t *h, const cg_cmd_t *cmd, cg_buf_t *why)
{
    cg_level_t have = cg_hub_agent_level(h);
    if (cmd->level > have) {
        if (cg_gdbseat_attached(h->seat) && cmd->level <= h->cfg.policy.agent)
            cg_buf_printf(why,
                          "ERR policy: %s needs level %s, agents have %s while gdb is attached"
                          " (the gdb user can run: monitor cogdb allow %s)\n",
                          cmd->name, cg_level_name(cmd->level), cg_level_name(have),
                          cg_level_name(cmd->level));
        else
            cg_buf_printf(why, "ERR policy: %s needs level %s, agents have %s\n", cmd->name,
                          cg_level_name(cmd->level), cg_level_name(have));
        return why->data;
    }
    cg_tstate_t st = cg_target_state(h->target);
    if ((cmd->need & (CG_NEED_LINK | CG_NEED_STOPPED | CG_NEED_IDLE)) && st == CG_TS_DOWN) {
        cg_buf_puts(why, "ERR stub link is down\n");
        return why->data;
    }
    if ((cmd->need & (CG_NEED_STOPPED | CG_NEED_IDLE)) && st == CG_TS_RUNNING) {
        cg_buf_puts(why, "ERR target running (HALT first)\n");
        return why->data;
    }
    if ((cmd->need & CG_NEED_STOPPED) && st != CG_TS_STOPPED) {
        cg_buf_printf(why, "ERR target is %s\n", cg_tstate_name(st));
        return why->data;
    }
    return NULL;
}

static void run_line(cg_call_t *call, char *line)
{
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    cg_hub_t *h = call->ctl->hub;
    const char *args = "";
    const cg_cmd_t *cmd = find_cmd(line, &args);
    if (!cmd || (cmd->args == CG_ARGS_NONE && *args) || (cmd->args == CG_ARGS_REQ && !*args)) {
        if (cmd && cmd->usage)
            cg_buf_printf(&call->out, "ERR usage: %s\n", cmd->usage);
        else
            cg_buf_puts(&call->out, "ERR unknown command\n");
        cg_call_finish(call);
        return;
    }
    if (gate(h, cmd, &call->out)) {
        cg_call_finish(call);
        return;
    }
    switch (cmd->fn(h, call, args)) {
    case CG_CALL_DONE:
        cg_call_finish(call);
        break;
    case CG_CALL_STREAM:
        if (call->conn)
            cg_conn_set_limit(call->conn, STREAM_LIMIT);
        break;
    case CG_CALL_PENDING:
        break;
    }
}

static void call_data(void *ctx, const uint8_t *data, size_t n)
{
    cg_call_t *call = ctx;
    if (call->started)
        return; /* one command per connection */
    if (call->in.len + n > LINE_MAX_BYTES) {
        cg_buf_reset(&call->out);
        cg_buf_puts(&call->out, "ERR line too long\n");
        call->started = 1;
        cg_call_finish(call);
        return;
    }
    cg_buf_append(&call->in, data, n);
    char *nl = memchr(call->in.data, '\n', call->in.len);
    if (!nl)
        return;
    *nl = '\0';
    call->started = 1;
    run_line(call, call->in.data);
}

static void call_closed(void *ctx)
{
    cg_call_t *call = ctx;
    call->conn = NULL;
    if (!call->ended) {
        call->ended = 1;
        call_free(call);
    }
}

static const cg_conn_ops_t k_call_ops = {call_data, call_closed};

static void on_accept(void *ctx, int fd, unsigned ev)
{
    (void)ev;
    cg_ctl_t *c = ctx;
    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0)
        return;
    if (c->ncalls >= MAX_CALLS) {
        static const char busy[] = "ERR too many clients\n";
        ssize_t w = write(cfd, busy, sizeof(busy) - 1);
        (void)w;
        close(cfd);
        return;
    }
    cg_call_t *call = calloc(1, sizeof(*call));
    if (!call) {
        close(cfd);
        return;
    }
    call->ctl = c;
    cg_buf_init(&call->in);
    cg_buf_init(&call->out);
    call->conn = cg_conn_new(c->loop, cfd, &k_call_ops, call);
    if (!call->conn) {
        close(cfd);
        free(call);
        return;
    }
    c->ncalls++;
}

/* ---- socket ------------------------------------------------------------ */

static int socket_in_use(const char *path)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    int used = connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    close(fd);
    return used;
}

cg_ctl_t *cg_ctl_new(cg_hub_t *h, cg_loop_t *loop, const char *path)
{
    struct sockaddr_un sa;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        CG_ERR("control socket path too long: %s", path);
        return NULL;
    }
    if (socket_in_use(path)) {
        CG_ERR("another hub already serves %s", path);
        return NULL;
    }
    unlink(path);
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t old = umask(077);
    int ok = fd >= 0 && bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 16) == 0;
    umask(old);
    if (!ok) {
        CG_ERR("control socket %s: %s", path, strerror(errno));
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    cg_ctl_t *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return NULL;
    }
    c->hub = h;
    c->loop = loop;
    c->fd = fd;
    snprintf(c->path, sizeof(c->path), "%s", path);
    cg_set_nonblock(fd);
    cg_loop_add(loop, fd, CG_IO_READ, on_accept, c);
    return c;
}

void cg_ctl_free(cg_ctl_t *c)
{
    if (!c)
        return;
    cg_loop_del(c->loop, c->fd);
    close(c->fd);
    unlink(c->path);
    free(c);
}
