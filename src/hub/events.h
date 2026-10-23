#ifndef CG_EVENTS_H
#define CG_EVENTS_H

/* In-process publish/subscribe. Producers (target session, gdb seat, link)
 * do not know who listens; consumers (watch streams, recorder, waiters,
 * the event ring) do not know who produced. */

#include <stddef.h>
#include <stdint.h>

#include "rsp/stop.h"
#include "util/buf.h"

typedef enum {
    CG_EV_LINK,    /* stub connection up/down */
    CG_EV_CLIENT,  /* gdb client attached/detached/refused */
    CG_EV_RUN,     /* target resumed */
    CG_EV_STOP,    /* target stopped or exited */
    CG_EV_CONSOLE, /* O-packet output, decoded */
    CG_EV_NOTE,    /* hub notices: policy, timeouts, protocol oddities */
    CG_EV_PACKET,  /* raw RSP traffic */
    CG_EV__COUNT
} cg_ev_type_t;

#define CG_EVM(t) (1u << (t))
#define CG_EVM_ALL ((1u << CG_EV__COUNT) - 1)
#define CG_EVM_DEFAULT (CG_EVM_ALL & ~CG_EVM(CG_EV_PACKET))

typedef struct {
    cg_ev_type_t type;
    uint64_t seq;
    uint64_t ns;        /* CLOCK_MONOTONIC */
    const char *origin; /* gdb, agent, hub, target */
    const char *text;   /* human line; packets: raw payload */
    size_t len;
    char dir;              /* packets: '>' toward the stub, '<' from it */
    const cg_stop_t *stop; /* CG_EV_STOP */
} cg_event_t;

typedef void (*cg_ev_fn)(void *ctx, const cg_event_t *ev);
typedef struct cg_bus cg_bus_t;

cg_bus_t *cg_bus_new(size_t ring_cap);
void cg_bus_free(cg_bus_t *b);

uint64_t cg_bus_subscribe(cg_bus_t *b, unsigned mask, cg_ev_fn fn, void *ctx);
/* Safe from inside a callback. */
void cg_bus_unsubscribe(cg_bus_t *b, uint64_t id);

void cg_bus_publish(cg_bus_t *b, cg_event_t *ev);
void cg_bus_emitf(cg_bus_t *b, cg_ev_type_t type, const char *origin, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void cg_bus_packet(cg_bus_t *b, const char *origin, char dir, const char *data, size_t n);

/* Newest n ring lines matching mask, oldest first. */
int cg_bus_history(cg_bus_t *b, size_t n, unsigned mask, cg_buf_t *out);
uint64_t cg_bus_seq(const cg_bus_t *b);

const char *cg_ev_name(cg_ev_type_t t);
/* Parses "stop,run" style lists; "all" and "default" are accepted. */
int cg_ev_parse_mask(const char *s, unsigned *mask);
/* "<seq> <sec.ms> <type> <origin> <text>\n" with unprintables escaped. */
int cg_event_format(const cg_bus_t *b, const cg_event_t *ev, cg_buf_t *out);

#endif
