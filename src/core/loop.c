#include "loop.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "util/sys.h"

typedef struct {
    int fd;
    unsigned events;
    cg_io_fn fn;
    void *ctx;
    uint64_t gen; /* distinguishes a re-added fd within one dispatch round */
    int dead;
} watcher_t;

typedef struct {
    uint64_t id;
    uint64_t due_ms;
    cg_timer_fn fn;
    void *ctx;
} timer_t_;

struct cg_loop {
    watcher_t *w;
    size_t nw, capw;
    timer_t_ *t;
    size_t nt, capt;
    uint64_t next_gen;
    uint64_t next_timer;
    int stop;
};

static volatile sig_atomic_t g_signalled;

static void on_signal(int sig)
{
    (void)sig;
    g_signalled = 1;
}

cg_loop_t *cg_loop_new(void)
{
    cg_loop_t *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    return l;
}

void cg_loop_free(cg_loop_t *l)
{
    if (!l)
        return;
    free(l->w);
    free(l->t);
    free(l);
}

static watcher_t *find(cg_loop_t *l, int fd)
{
    for (size_t i = 0; i < l->nw; i++)
        if (!l->w[i].dead && l->w[i].fd == fd)
            return &l->w[i];
    return NULL;
}

int cg_loop_add(cg_loop_t *l, int fd, unsigned events, cg_io_fn fn, void *ctx)
{
    if (find(l, fd))
        return -1;
    if (l->nw == l->capw) {
        size_t cap = l->capw ? l->capw * 2 : 16;
        watcher_t *p = realloc(l->w, cap * sizeof(*p));
        if (!p)
            return -1;
        l->w = p;
        l->capw = cap;
    }
    l->w[l->nw++] = (watcher_t){fd, events, fn, ctx, ++l->next_gen, 0};
    return 0;
}

int cg_loop_mod(cg_loop_t *l, int fd, unsigned events)
{
    watcher_t *w = find(l, fd);
    if (!w)
        return -1;
    w->events = events;
    return 0;
}

void cg_loop_del(cg_loop_t *l, int fd)
{
    watcher_t *w = find(l, fd);
    if (w)
        w->dead = 1;
}

static void compact(cg_loop_t *l)
{
    size_t o = 0;
    for (size_t i = 0; i < l->nw; i++)
        if (!l->w[i].dead)
            l->w[o++] = l->w[i];
    l->nw = o;
}

uint64_t cg_loop_timer(cg_loop_t *l, uint64_t delay_ms, cg_timer_fn fn, void *ctx)
{
    if (l->nt == l->capt) {
        size_t cap = l->capt ? l->capt * 2 : 16;
        timer_t_ *p = realloc(l->t, cap * sizeof(*p));
        if (!p)
            return 0;
        l->t = p;
        l->capt = cap;
    }
    uint64_t id = ++l->next_timer;
    l->t[l->nt++] = (timer_t_){id, cg_now_ms() + delay_ms, fn, ctx};
    return id;
}

void cg_loop_cancel(cg_loop_t *l, uint64_t id)
{
    for (size_t i = 0; i < l->nt; i++) {
        if (l->t[i].id == id) {
            l->t[i] = l->t[--l->nt];
            return;
        }
    }
}

static int next_timeout(cg_loop_t *l)
{
    if (l->nt == 0)
        return 1000;
    uint64_t now = cg_now_ms();
    uint64_t due = l->t[0].due_ms;
    for (size_t i = 1; i < l->nt; i++)
        if (l->t[i].due_ms < due)
            due = l->t[i].due_ms;
    if (due <= now)
        return 0;
    uint64_t d = due - now;
    return d > 1000 ? 1000 : (int)d;
}

static void fire_timers(cg_loop_t *l)
{
    uint64_t now = cg_now_ms();
    for (;;) {
        size_t hit = l->nt;
        for (size_t i = 0; i < l->nt; i++) {
            if (l->t[i].due_ms <= now) {
                hit = i;
                break;
            }
        }
        if (hit == l->nt)
            return;
        timer_t_ t = l->t[hit];
        l->t[hit] = l->t[--l->nt];
        t.fn(t.ctx);
    }
}

void cg_loop_stop(cg_loop_t *l)
{
    l->stop = 1;
}

int cg_loop_run(cg_loop_t *l)
{
    struct pollfd *pfd = NULL;
    uint64_t *gens = NULL;
    size_t cap = 0;

    while (!l->stop && !g_signalled) {
        compact(l);
        if (l->nw > cap) {
            cap = l->nw * 2;
            struct pollfd *p = realloc(pfd, cap * sizeof(*p));
            uint64_t *g = realloc(gens, cap * sizeof(*g));
            if (p)
                pfd = p;
            if (g)
                gens = g;
            if (!p || !g)
                break;
        }
        size_t n = l->nw;
        for (size_t i = 0; i < n; i++) {
            pfd[i].fd = l->w[i].fd;
            pfd[i].events = (short)((l->w[i].events & CG_IO_READ ? POLLIN : 0) |
                                    (l->w[i].events & CG_IO_WRITE ? POLLOUT : 0));
            pfd[i].revents = 0;
            gens[i] = l->w[i].gen;
        }
        int rc = poll(pfd, n, next_timeout(l));
        if (rc < 0 && errno != EINTR) {
            CG_ERR("poll: %s", strerror(errno));
            break;
        }
        for (size_t i = 0; rc > 0 && i < n; i++) {
            if (!pfd[i].revents)
                continue;
            watcher_t *w = find(l, pfd[i].fd);
            if (!w || w->gen != gens[i])
                continue;
            unsigned ev = 0;
            if (pfd[i].revents & (POLLIN | POLLHUP))
                ev |= CG_IO_READ;
            if (pfd[i].revents & POLLOUT)
                ev |= CG_IO_WRITE;
            if (pfd[i].revents & (POLLERR | POLLNVAL))
                ev |= CG_IO_ERROR | CG_IO_READ;
            w->fn(w->ctx, w->fd, ev);
        }
        fire_timers(l);
    }
    free(pfd);
    free(gens);
    return 0;
}
