#ifndef CG_LOOP_H
#define CG_LOOP_H

#include <stdint.h>

/* Single-threaded poll(2) reactor with one-shot timers. Every module that
 * owns an fd registers it here; nobody else calls poll. */

enum {
    CG_IO_READ = 1u << 0,
    CG_IO_WRITE = 1u << 1,
    CG_IO_ERROR = 1u << 2, /* reported only, never requested */
};

typedef struct cg_loop cg_loop_t;
typedef void (*cg_io_fn)(void *ctx, int fd, unsigned events);
typedef void (*cg_timer_fn)(void *ctx);

cg_loop_t *cg_loop_new(void);
void cg_loop_free(cg_loop_t *l);

int cg_loop_add(cg_loop_t *l, int fd, unsigned events, cg_io_fn fn, void *ctx);
int cg_loop_mod(cg_loop_t *l, int fd, unsigned events);
/* Safe to call from inside any callback, including for the fd being
 * dispatched. */
void cg_loop_del(cg_loop_t *l, int fd);

/* Returns a non-zero timer id. */
uint64_t cg_loop_timer(cg_loop_t *l, uint64_t delay_ms, cg_timer_fn fn, void *ctx);
void cg_loop_cancel(cg_loop_t *l, uint64_t id);

/* Runs until cg_loop_stop or SIGINT/SIGTERM. */
int cg_loop_run(cg_loop_t *l);
void cg_loop_stop(cg_loop_t *l);

#endif
