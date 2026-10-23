#include "flow.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/conn.h"
#include "util/buf.h"
#include "util/log.h"

#define FLOW_OUT_MAX (256 * 1024)

struct cg_flow {
    cg_loop_t *loop;
    cg_bus_t *bus;
    char *gdb, *script, *dir, *names;
    pid_t pid;
    cg_conn_t *conn;
    uint64_t timer;
    int killed;
    char name[64];
    cg_buf_t out;
    cg_flow_done_fn fn;
    void *ctx;
    void (*end_fn)(void *ctx);
    void *end_ctx;
};

static char *dup_or(const char *s, const char *def)
{
    return strdup(s ? s : def);
}

cg_flow_t *cg_flow_new(cg_loop_t *loop, cg_bus_t *bus, const cg_flow_cfg_t *cfg)
{
    cg_flow_t *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->loop = loop;
    f->bus = bus;
    f->pid = -1;
    cg_buf_init(&f->out);
    if (cfg && cfg->script) {
        f->gdb = dup_or(cfg->gdb, "gdb");
        f->script = realpath(cfg->script, NULL);
        f->names = dup_or(cfg->names, "");
        if (f->script) {
            f->dir = strdup(f->script);
            char *slash = strrchr(f->dir, '/');
            if (slash)
                slash[slash == f->dir ? 1 : 0] = '\0';
        } else {
            CG_WARN("flows off: cannot resolve script %s", cfg->script);
        }
    }
    return f;
}

static void reap(cg_flow_t *f)
{
    int st = 0, rc = -1;
    if (f->pid > 0 && waitpid(f->pid, &st, 0) == f->pid && WIFEXITED(st) && !f->killed)
        rc = WEXITSTATUS(st);
    f->pid = -1;
    if (f->timer)
        cg_loop_cancel(f->loop, f->timer);
    f->timer = 0;
    cg_bus_emitf(f->bus, CG_EV_NOTE, "agent", "flow %s ended rc=%d", f->name, rc);
    if (f->end_fn)
        f->end_fn(f->end_ctx);
    cg_flow_done_fn fn = f->fn;
    void *ctx = f->ctx;
    f->fn = NULL;
    f->ctx = NULL;
    if (fn)
        fn(ctx, rc, f->out.data ? f->out.data : "", f->out.len);
    cg_buf_reset(&f->out);
}

static void on_data(void *ctx, const uint8_t *data, size_t n)
{
    cg_flow_t *f = ctx;
    if (f->out.len + n <= FLOW_OUT_MAX)
        cg_buf_append(&f->out, data, n);
}

static void on_close(void *ctx)
{
    cg_flow_t *f = ctx;
    f->conn = NULL;
    reap(f);
}

static const cg_conn_ops_t k_ops = {on_data, on_close};

static void on_timeout(void *ctx)
{
    cg_flow_t *f = ctx;
    f->timer = 0;
    if (f->pid > 0) {
        f->killed = 1;
        cg_buf_puts(&f->out, "\ncogdb: flow timed out, gdb killed\n");
        kill(-f->pid, SIGKILL);
    }
}

void cg_flow_free(cg_flow_t *f)
{
    if (!f)
        return;
    if (f->pid > 0) {
        kill(-f->pid, SIGKILL);
        waitpid(f->pid, NULL, 0);
    }
    if (f->conn)
        cg_conn_close(f->conn);
    if (f->timer)
        cg_loop_cancel(f->loop, f->timer);
    cg_buf_free(&f->out);
    free(f->gdb);
    free(f->script);
    free(f->dir);
    free(f->names);
    free(f);
}

int cg_flow_enabled(const cg_flow_t *f)
{
    return f && f->script;
}

int cg_flow_busy(const cg_flow_t *f)
{
    return f && f->pid > 0;
}

const char *cg_flow_names(const cg_flow_t *f)
{
    return f && f->names ? f->names : "";
}

int cg_flow_allowed(const cg_flow_t *f, const char *name)
{
    if (!cg_flow_enabled(f) || !*name)
        return 0;
    size_t k = strlen(name);
    for (const char *p = f->names; *p;) {
        size_t n = strcspn(p, ",");
        if (n == k && strncmp(p, name, k) == 0)
            return 1;
        p += n;
        if (*p == ',')
            p++;
    }
    return 0;
}

int cg_flow_start(cg_flow_t *f, const char *name, int port, unsigned timeout_ms,
                  cg_flow_done_fn fn, void *ctx)
{
    if (!cg_flow_allowed(f, name) || strlen(name) >= sizeof(f->name))
        return -1;
    if (cg_flow_busy(f))
        return -2;
    char remote[64];
    snprintf(remote, sizeof(remote), "target remote 127.0.0.1:%d", port);
    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        /* The hub's sockets (stub link, gdb listener) are not all CLOEXEC. */
        for (int fd = 3; fd < 1024; fd++)
            close(fd);
        if (chdir(f->dir) != 0)
            _exit(126);
        execlp(f->gdb, f->gdb, "-q", "-nx", "-batch", "-x", f->script, "-ex", remote, "-ex",
               name, (char *)NULL);
        fprintf(stderr, "cogdb: cannot run %s\n", f->gdb);
        _exit(127);
    }
    close(pfd[1]);
    setpgid(pid, pid);
    f->conn = cg_conn_new(f->loop, pfd[0], &k_ops, f);
    if (!f->conn) {
        close(pfd[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    f->pid = pid;
    f->killed = 0;
    snprintf(f->name, sizeof(f->name), "%s", name);
    cg_buf_reset(&f->out);
    f->fn = fn;
    f->ctx = ctx;
    f->timer = cg_loop_timer(f->loop, timeout_ms, on_timeout, f);
    cg_bus_emitf(f->bus, CG_EV_NOTE, "agent", "flow %s started: %s -x %s", name, f->gdb,
                 f->script);
    return 0;
}

void cg_flow_forget(cg_flow_t *f, void *ctx)
{
    if (f && f->ctx == ctx) {
        f->fn = NULL;
        f->ctx = NULL;
    }
}

void cg_flow_on_end(cg_flow_t *f, void (*fn)(void *ctx), void *ctx)
{
    f->end_fn = fn;
    f->end_ctx = ctx;
}
