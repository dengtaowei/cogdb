#include "miseat.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "core/conn.h"
#include "util/buf.h"
#include "util/log.h"

struct cg_miseat {
    cg_loop_t *loop;
    cg_bus_t *bus;
    char link[512];
    char pts[64];
    cg_conn_t *conn;
    int slave_fd; /* held until gdb opens the pty, so reads never see EIO */
    int attached;
    cg_buf_t line;
    unsigned token;
    int busy;
    cg_buf_t out;
    cg_mi_done_fn fn;
    void *ctx;
};

static void open_pty(cg_miseat_t *m);

static void finish(cg_miseat_t *m, int ok, const char *msg)
{
    if (!m->busy)
        return;
    cg_buf_putc(&m->out, '\0');
    m->out.len--;
    /* gdb usually logs the error on the & stream before ^error repeats it. */
    if (msg && *msg && !strstr(m->out.data, msg)) {
        if (m->out.len && m->out.data[m->out.len - 1] != '\n')
            cg_buf_putc(&m->out, '\n');
        cg_buf_puts(&m->out, msg);
    }
    cg_mi_done_fn fn = m->fn;
    void *ctx = m->ctx;
    m->busy = 0;
    m->fn = NULL;
    m->ctx = NULL;
    if (fn)
        fn(ctx, ok, m->out.data ? m->out.data : "", m->out.len);
    cg_buf_reset(&m->out);
}

/* MI c-string after the opening quote; appends the decoded text. */
static const char *unquote(const char *p, cg_buf_t *out)
{
    for (; *p && *p != '"'; p++) {
        if (*p != '\\' || !p[1]) {
            cg_buf_putc(out, *p);
            continue;
        }
        p++;
        switch (*p) {
        case 'n': cg_buf_putc(out, '\n'); break;
        case 't': cg_buf_putc(out, '\t'); break;
        case 'r': break;
        case '0': case '1': case '2': case '3': {
            int v = 0, k = 0;
            while (k < 3 && p[k] >= '0' && p[k] <= '7')
                v = v * 8 + (p[k++] - '0');
            cg_buf_putc(out, (char)v);
            p += k - 1;
            break;
        }
        default: cg_buf_putc(out, *p); break;
        }
    }
    return p;
}

static void on_line(cg_miseat_t *m, const char *s)
{
    if (!m->busy)
        return;
    if ((s[0] == '~' || s[0] == '@' || s[0] == '&') && s[1] == '"') {
        unquote(s + 2, &m->out);
        return;
    }
    char *end;
    unsigned long tok = strtoul(s, &end, 10);
    if (end == s || *end != '^' || tok != m->token)
        return;
    const char *cls = end + 1;
    if (strncmp(cls, "error", 5) == 0) {
        cg_buf_t msg;
        cg_buf_init(&msg);
        const char *q = strstr(cls, "msg=\"");
        if (q)
            unquote(q + 5, &msg);
        finish(m, 0, msg.data ? msg.data : "gdb reported an error");
        cg_buf_free(&msg);
    } else {
        finish(m, 1, NULL);
    }
}

static void on_data(void *ctx, const uint8_t *data, size_t n)
{
    cg_miseat_t *m = ctx;
    if (!m->attached) {
        m->attached = 1;
        if (m->slave_fd >= 0)
            close(m->slave_fd);
        m->slave_fd = -1;
        cg_bus_emitf(m->bus, CG_EV_CLIENT, "gdb", "mi attached on %s", m->link);
    }
    for (size_t i = 0; i < n; i++) {
        char c = (char)data[i];
        if (c == '\r')
            continue;
        if (c != '\n') {
            cg_buf_putc(&m->line, c);
            continue;
        }
        cg_buf_putc(&m->line, '\0');
        on_line(m, m->line.data);
        cg_buf_reset(&m->line);
    }
}

static void on_close(void *ctx)
{
    cg_miseat_t *m = ctx;
    m->conn = NULL;
    if (m->attached)
        cg_bus_emitf(m->bus, CG_EV_CLIENT, "gdb", "mi closed");
    m->attached = 0;
    finish(m, 0, "gdb closed its MI channel");
    open_pty(m);
}

static const cg_conn_ops_t k_ops = {on_data, on_close};

static void open_pty(cg_miseat_t *m)
{
    cg_buf_reset(&m->line);
    if (m->slave_fd >= 0)
        close(m->slave_fd);
    m->slave_fd = -1;
    m->pts[0] = '\0';
    int mfd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (mfd < 0 || grantpt(mfd) != 0 || unlockpt(mfd) != 0 || !ptsname(mfd)) {
        CG_WARN("gdb MI channel: no pty available");
        if (mfd >= 0)
            close(mfd);
        return;
    }
    snprintf(m->pts, sizeof(m->pts), "%s", ptsname(mfd));
    m->slave_fd = open(m->pts, O_RDWR | O_NOCTTY | O_CLOEXEC);
    struct termios tio;
    if (m->slave_fd >= 0 && tcgetattr(m->slave_fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(m->slave_fd, TCSANOW, &tio);
    }
    m->conn = cg_conn_new(m->loop, mfd, &k_ops, m);
    if (!m->conn) {
        close(mfd);
        return;
    }
    unlink(m->link);
    if (symlink(m->pts, m->link) != 0)
        CG_WARN("gdb MI channel: cannot link %s", m->link);
}

cg_miseat_t *cg_miseat_new(cg_loop_t *loop, cg_bus_t *bus, const char *link_path)
{
    cg_miseat_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->loop = loop;
    m->bus = bus;
    m->slave_fd = -1;
    snprintf(m->link, sizeof(m->link), "%s", link_path);
    cg_buf_init(&m->line);
    cg_buf_init(&m->out);
    open_pty(m);
    return m;
}

void cg_miseat_free(cg_miseat_t *m)
{
    if (!m)
        return;
    m->fn = NULL;
    if (m->conn)
        cg_conn_close(m->conn);
    if (m->slave_fd >= 0)
        close(m->slave_fd);
    unlink(m->link);
    cg_buf_free(&m->line);
    cg_buf_free(&m->out);
    free(m);
}

int cg_miseat_attached(const cg_miseat_t *m)
{
    return m && m->attached;
}

int cg_miseat_busy(const cg_miseat_t *m)
{
    return m && m->busy;
}

const char *cg_miseat_path(const cg_miseat_t *m)
{
    return m->link;
}

int cg_miseat_exec(cg_miseat_t *m, const char *cmd, cg_mi_done_fn fn, void *ctx)
{
    if (!m->attached || !m->conn)
        return -1;
    if (m->busy)
        return -2;
    cg_buf_t req;
    cg_buf_init(&req);
    cg_buf_printf(&req, "%u-interpreter-exec console \"", ++m->token);
    for (const char *p = cmd; *p; p++) {
        if (*p == '"' || *p == '\\')
            cg_buf_putc(&req, '\\');
        cg_buf_putc(&req, *p);
    }
    cg_buf_puts(&req, "\"\n");
    int rc = cg_conn_write(m->conn, req.data, req.len);
    cg_buf_free(&req);
    if (rc != 0 || !m->conn)
        return -1;
    m->busy = 1;
    m->fn = fn;
    m->ctx = ctx;
    cg_buf_reset(&m->out);
    return 0;
}

void cg_miseat_forget(cg_miseat_t *m, void *ctx)
{
    if (m && m->ctx == ctx) {
        m->fn = NULL;
        m->ctx = NULL;
    }
}
