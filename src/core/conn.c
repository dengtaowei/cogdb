#include "conn.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/buf.h"
#include "util/sys.h"

struct cg_conn {
    cg_loop_t *loop;
    int fd;
    cg_conn_ops_t ops;
    void *ctx;
    cg_buf_t out;
    size_t limit;
    int busy;     /* inside a callback: defer the free */
    int dead;     /* closed; free once no longer busy */
    int draining; /* close_after_flush requested */
};

static void destroy(cg_conn_t *c)
{
    cg_loop_del(c->loop, c->fd);
    close(c->fd);
    cg_buf_free(&c->out);
    free(c);
}

static void mark_dead(cg_conn_t *c)
{
    if (c->dead)
        return;
    c->dead = 1;
    if (!c->busy)
        destroy(c);
}

static void hangup(cg_conn_t *c)
{
    if (c->dead)
        return;
    c->busy++;
    if (c->ops.on_close)
        c->ops.on_close(c->ctx);
    c->busy--;
    c->dead = 0;
    mark_dead(c);
}

static void update_interest(cg_conn_t *c)
{
    unsigned ev = c->draining ? 0 : CG_IO_READ;
    if (c->out.len)
        ev |= CG_IO_WRITE;
    cg_loop_mod(c->loop, c->fd, ev);
}

static int flush(cg_conn_t *c)
{
    while (c->out.len) {
        ssize_t n = write(c->fd, c->out.data, c->out.len);
        if (n > 0) {
            cg_buf_consume(&c->out, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static void on_io(void *ctx, int fd, unsigned ev)
{
    (void)fd;
    cg_conn_t *c = ctx;
    c->busy++;
    if ((ev & CG_IO_WRITE) && flush(c) != 0) {
        c->busy--;
        hangup(c);
        return;
    }
    if (c->draining && c->out.len == 0) {
        c->busy--;
        mark_dead(c);
        return;
    }
    if ((ev & CG_IO_READ) && !c->draining) {
        uint8_t buf[4096];
        ssize_t n = read(c->fd, buf, sizeof(buf));
        if (n > 0) {
            if (c->ops.on_data)
                c->ops.on_data(c->ctx, buf, (size_t)n);
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            c->busy--;
            hangup(c);
            return;
        }
    }
    c->busy--;
    if (c->dead) {
        c->dead = 0;
        mark_dead(c);
        return;
    }
    update_interest(c);
}

cg_conn_t *cg_conn_new(cg_loop_t *l, int fd, const cg_conn_ops_t *ops, void *ctx)
{
    cg_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->loop = l;
    c->fd = fd;
    c->ops = *ops;
    c->ctx = ctx;
    cg_buf_init(&c->out);
    cg_set_nonblock(fd);
    if (cg_loop_add(l, fd, CG_IO_READ, on_io, c) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

int cg_conn_write(cg_conn_t *c, const void *data, size_t n)
{
    if (c->dead || c->draining)
        return -1;
    if (c->limit && c->out.len + n > c->limit) {
        hangup(c);
        return -1;
    }
    if (cg_buf_append(&c->out, data, n) != 0)
        return -1;
    if (flush(c) != 0) {
        hangup(c);
        return -1;
    }
    update_interest(c);
    return 0;
}

size_t cg_conn_pending(const cg_conn_t *c)
{
    return c->out.len;
}

void cg_conn_set_limit(cg_conn_t *c, size_t max_pending)
{
    c->limit = max_pending;
}

void cg_conn_close(cg_conn_t *c)
{
    mark_dead(c);
}

void cg_conn_close_after_flush(cg_conn_t *c)
{
    if (c->dead)
        return;
    if (c->out.len == 0) {
        mark_dead(c);
        return;
    }
    c->draining = 1;
    update_interest(c);
}

int cg_conn_fd(const cg_conn_t *c)
{
    return c->fd;
}
