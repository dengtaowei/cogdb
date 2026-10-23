#include "events.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/sys.h"

#define RING_LINE_MAX 240

typedef struct {
    uint64_t id;
    unsigned mask;
    cg_ev_fn fn;
    void *ctx;
} sub_t;

typedef struct {
    cg_ev_type_t type;
    char *line;
} ring_ent_t;

struct cg_bus {
    sub_t *subs;
    size_t nsubs, capsubs;
    uint64_t next_id;
    int publishing;
    int dirty;
    uint64_t seq;
    uint64_t t0;
    ring_ent_t *ring;
    size_t ring_cap, ring_head, ring_n;
};

static const char *const k_names[CG_EV__COUNT] = {
    "link", "client", "run", "stop", "console", "note", "packet",
};

const char *cg_ev_name(cg_ev_type_t t)
{
    return t < CG_EV__COUNT ? k_names[t] : "?";
}

int cg_ev_parse_mask(const char *s, unsigned *mask)
{
    if (!s || !*s || strcmp(s, "default") == 0) {
        *mask = CG_EVM_DEFAULT;
        return 0;
    }
    if (strcmp(s, "all") == 0) {
        *mask = CG_EVM_ALL;
        return 0;
    }
    unsigned m = 0;
    while (*s) {
        size_t k = strcspn(s, ",");
        int found = 0;
        for (int t = 0; t < CG_EV__COUNT; t++) {
            if (strlen(k_names[t]) == k && strncmp(s, k_names[t], k) == 0) {
                m |= CG_EVM(t);
                found = 1;
            }
        }
        if (!found)
            return -1;
        s += k;
        if (*s == ',')
            s++;
    }
    *mask = m;
    return 0;
}

cg_bus_t *cg_bus_new(size_t ring_cap)
{
    cg_bus_t *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->ring_cap = ring_cap ? ring_cap : 1;
    b->ring = calloc(b->ring_cap, sizeof(*b->ring));
    b->t0 = cg_now_ns();
    if (!b->ring) {
        free(b);
        return NULL;
    }
    return b;
}

void cg_bus_free(cg_bus_t *b)
{
    if (!b)
        return;
    for (size_t i = 0; i < b->ring_cap; i++)
        free(b->ring[i].line);
    free(b->ring);
    free(b->subs);
    free(b);
}

uint64_t cg_bus_subscribe(cg_bus_t *b, unsigned mask, cg_ev_fn fn, void *ctx)
{
    if (b->nsubs == b->capsubs) {
        size_t cap = b->capsubs ? b->capsubs * 2 : 8;
        sub_t *p = realloc(b->subs, cap * sizeof(*p));
        if (!p)
            return 0;
        b->subs = p;
        b->capsubs = cap;
    }
    uint64_t id = ++b->next_id;
    b->subs[b->nsubs++] = (sub_t){id, mask, fn, ctx};
    return id;
}

void cg_bus_unsubscribe(cg_bus_t *b, uint64_t id)
{
    for (size_t i = 0; i < b->nsubs; i++) {
        if (b->subs[i].id == id) {
            b->subs[i].fn = NULL;
            b->dirty = 1;
        }
    }
}

static void compact(cg_bus_t *b)
{
    size_t o = 0;
    for (size_t i = 0; i < b->nsubs; i++)
        if (b->subs[i].fn)
            b->subs[o++] = b->subs[i];
    b->nsubs = o;
    b->dirty = 0;
}

static void escape_into(cg_buf_t *out, const char *s, size_t n, size_t max)
{
    for (size_t i = 0; i < n; i++) {
        if (max && out->len >= max) {
            cg_buf_puts(out, "...");
            return;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '\\')
            cg_buf_puts(out, "\\\\");
        else if (c >= 0x20 && c < 0x7f)
            cg_buf_putc(out, (char)c);
        else if (c == '\n')
            cg_buf_puts(out, "\\n");
        else
            cg_buf_printf(out, "\\x%02x", c);
    }
}

static int format_limited(const cg_bus_t *b, const cg_event_t *ev, cg_buf_t *out, size_t max)
{
    uint64_t rel = ev->ns > b->t0 ? ev->ns - b->t0 : 0;
    cg_buf_printf(out, "%llu %llu.%03llu %s %s", (unsigned long long)ev->seq,
                  (unsigned long long)(rel / 1000000000ull),
                  (unsigned long long)(rel / 1000000ull % 1000ull),
                  cg_ev_name(ev->type), ev->origin ? ev->origin : "-");
    if (ev->type == CG_EV_PACKET)
        cg_buf_printf(out, "%c ", ev->dir ? ev->dir : '>');
    else
        cg_buf_putc(out, ' ');
    escape_into(out, ev->text ? ev->text : "", ev->len, max ? out->len + max : 0);
    return cg_buf_putc(out, '\n');
}

int cg_event_format(const cg_bus_t *b, const cg_event_t *ev, cg_buf_t *out)
{
    return format_limited(b, ev, out, 0);
}

static void ring_add(cg_bus_t *b, const cg_event_t *ev)
{
    cg_buf_t line;
    cg_buf_init(&line);
    format_limited(b, ev, &line, RING_LINE_MAX);
    size_t slot = (b->ring_head + b->ring_n) % b->ring_cap;
    if (b->ring_n == b->ring_cap) {
        slot = b->ring_head;
        b->ring_head = (b->ring_head + 1) % b->ring_cap;
    } else {
        b->ring_n++;
    }
    free(b->ring[slot].line);
    b->ring[slot].type = ev->type;
    b->ring[slot].line = line.data;
}

void cg_bus_publish(cg_bus_t *b, cg_event_t *ev)
{
    ev->seq = ++b->seq;
    ev->ns = cg_now_ns();
    if (!ev->text)
        ev->text = "";
    ring_add(b, ev);
    b->publishing++;
    size_t n = b->nsubs; /* subscribers added during dispatch wait a round */
    for (size_t i = 0; i < n; i++) {
        sub_t s = b->subs[i];
        if (s.fn && (s.mask & CG_EVM(ev->type)))
            s.fn(s.ctx, ev);
    }
    if (--b->publishing == 0 && b->dirty)
        compact(b);
}

void cg_bus_emitf(cg_bus_t *b, cg_ev_type_t type, const char *origin, const char *fmt, ...)
{
    cg_buf_t text;
    cg_buf_init(&text);
    va_list ap;
    va_start(ap, fmt);
    cg_buf_vprintf(&text, fmt, ap);
    va_end(ap);
    cg_event_t ev = {.type = type, .origin = origin, .text = text.data, .len = text.len};
    cg_bus_publish(b, &ev);
    cg_buf_free(&text);
}

void cg_bus_packet(cg_bus_t *b, const char *origin, char dir, const char *data, size_t n)
{
    cg_event_t ev = {.type = CG_EV_PACKET, .origin = origin, .text = data, .len = n, .dir = dir};
    cg_bus_publish(b, &ev);
}

int cg_bus_history(cg_bus_t *b, size_t n, unsigned mask, cg_buf_t *out)
{
    size_t matched = 0;
    for (size_t i = b->ring_n; i-- > 0 && matched < n;) {
        ring_ent_t *e = &b->ring[(b->ring_head + i) % b->ring_cap];
        if (mask & CG_EVM(e->type))
            matched++;
    }
    size_t skip = 0;
    for (size_t i = b->ring_n; i-- > 0;) {
        ring_ent_t *e = &b->ring[(b->ring_head + i) % b->ring_cap];
        if ((mask & CG_EVM(e->type)) && ++skip == matched) {
            for (size_t j = i; j < b->ring_n; j++) {
                ring_ent_t *f = &b->ring[(b->ring_head + j) % b->ring_cap];
                if (mask & CG_EVM(f->type))
                    cg_buf_puts(out, f->line);
            }
            break;
        }
    }
    return 0;
}

uint64_t cg_bus_seq(const cg_bus_t *b)
{
    return b->seq;
}
