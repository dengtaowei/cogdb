#include "regmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rsp/rsp.h"
#include "util/hex.h"
#include "util/log.h"

#define MAX_DOCS 32
#define XFER_CHUNK 0x800

typedef struct {
    char href[96];
    cg_buf_t data;
    int done;
} doc_t;

typedef struct {
    cg_regmap_ready_fn fn;
    void *ctx;
} waiter_t;

typedef enum { RM_EMPTY, RM_FETCHING, RM_READY, RM_FAILED } rm_state_t;

struct cg_regmap {
    cg_target_t *target;
    cg_bus_t *bus;
    uint64_t sub;
    int big_endian;
    rm_state_t state;
    cg_tdesc_t tdesc;
    doc_t docs[MAX_DOCS];
    int ndocs;
    int cur;
    waiter_t *waiters;
    size_t nwait, capwait;
};

static void fetch_next(cg_regmap_t *rm);

static void reset_docs(cg_regmap_t *rm)
{
    for (int i = 0; i < rm->ndocs; i++)
        cg_buf_free(&rm->docs[i].data);
    rm->ndocs = 0;
    rm->cur = 0;
}

static void finish(cg_regmap_t *rm, int ok)
{
    rm->state = ok ? RM_READY : RM_FAILED;
    reset_docs(rm);
    waiter_t *w = rm->waiters;
    size_t n = rm->nwait;
    rm->waiters = NULL;
    rm->nwait = rm->capwait = 0;
    for (size_t i = 0; i < n; i++)
        if (w[i].fn)
            w[i].fn(w[i].ctx, ok);
    free(w);
}

static void add_doc(void *ctx, const char *href)
{
    cg_regmap_t *rm = ctx;
    for (int i = 0; i < rm->ndocs; i++)
        if (strcmp(rm->docs[i].href, href) == 0)
            return;
    if (rm->ndocs == MAX_DOCS || strlen(href) >= sizeof(rm->docs[0].href))
        return;
    doc_t *d = &rm->docs[rm->ndocs++];
    snprintf(d->href, sizeof(d->href), "%s", href);
    cg_buf_init(&d->data);
    d->done = 0;
}

static const char *lookup(void *ctx, const char *href, size_t *len)
{
    cg_regmap_t *rm = ctx;
    for (int i = 0; i < rm->ndocs; i++) {
        if (strcmp(rm->docs[i].href, href) == 0 && rm->docs[i].done) {
            *len = rm->docs[i].data.len;
            return rm->docs[i].data.data ? rm->docs[i].data.data : "";
        }
    }
    return NULL;
}

static void xfer_reply(void *ctx, cg_reply_status_t st, const char *p, size_t n)
{
    cg_regmap_t *rm = ctx;
    if (rm->state != RM_FETCHING)
        return;
    doc_t *d = &rm->docs[rm->cur];
    if (st != CG_REPLY_OK || n == 0 || (p[0] != 'm' && p[0] != 'l')) {
        CG_INFO("target description unavailable (%s)", st == CG_REPLY_OK ? "stub refused" : "no reply");
        finish(rm, 0);
        return;
    }
    cg_buf_t ex;
    cg_buf_init(&ex);
    cg_rsp_expand_rle(p + 1, n - 1, &ex);
    cg_rsp_unescape(ex.data ? ex.data : "", ex.len, &d->data);
    cg_buf_free(&ex);
    if (p[0] == 'l') {
        d->done = 1;
        cg_tdesc_includes(d->data.data ? d->data.data : "", d->data.len, add_doc, rm);
        rm->cur++;
    }
    fetch_next(rm);
}

static void fetch_next(cg_regmap_t *rm)
{
    if (rm->cur >= rm->ndocs) {
        cg_tdesc_free(&rm->tdesc);
        int ok = cg_tdesc_build(&rm->tdesc, "target.xml", lookup, rm) == 0;
        if (ok)
            cg_bus_emitf(rm->bus, CG_EV_NOTE, "hub", "target description: %s, %zu registers",
                         rm->tdesc.arch[0] ? rm->tdesc.arch : "unknown arch", rm->tdesc.n);
        finish(rm, ok);
        return;
    }
    doc_t *d = &rm->docs[rm->cur];
    unsigned chunk = XFER_CHUNK;
    const cg_target_caps_t *caps = cg_target_caps(rm->target);
    if (caps->packet_size > 64 && caps->packet_size - 32 < chunk)
        chunk = caps->packet_size - 32;
    char pkt[192];
    int k = snprintf(pkt, sizeof(pkt), "qXfer:features:read:%s:%zx,%x", d->href, d->data.len,
                     chunk);
    static const cg_req_ops_t ops = {xfer_reply, NULL};
    if (cg_target_submit(rm->target, "hub", pkt, (size_t)k, &ops, rm) != 0)
        finish(rm, 0);
}

static void on_link(void *ctx, const cg_event_t *ev)
{
    cg_regmap_t *rm = ctx;
    (void)ev;
    if (rm->state == RM_FETCHING)
        finish(rm, 0);
    cg_tdesc_free(&rm->tdesc);
    rm->state = RM_EMPTY;
}

cg_regmap_t *cg_regmap_new(cg_target_t *t, cg_bus_t *bus, int big_endian)
{
    cg_regmap_t *rm = calloc(1, sizeof(*rm));
    if (!rm)
        return NULL;
    rm->target = t;
    rm->bus = bus;
    rm->big_endian = big_endian;
    cg_tdesc_init(&rm->tdesc);
    rm->sub = cg_bus_subscribe(bus, CG_EVM(CG_EV_LINK), on_link, rm);
    return rm;
}

void cg_regmap_free(cg_regmap_t *rm)
{
    if (!rm)
        return;
    cg_bus_unsubscribe(rm->bus, rm->sub);
    cg_target_forget(rm->target, rm);
    reset_docs(rm);
    cg_tdesc_free(&rm->tdesc);
    free(rm->waiters);
    free(rm);
}

void cg_regmap_ensure(cg_regmap_t *rm, cg_regmap_ready_fn fn, void *ctx)
{
    if (rm->state == RM_READY || rm->state == RM_FAILED) {
        fn(ctx, rm->state == RM_READY);
        return;
    }
    if (rm->nwait == rm->capwait) {
        size_t cap = rm->capwait ? rm->capwait * 2 : 4;
        waiter_t *p = realloc(rm->waiters, cap * sizeof(*p));
        if (!p) {
            fn(ctx, 0);
            return;
        }
        rm->waiters = p;
        rm->capwait = cap;
    }
    rm->waiters[rm->nwait++] = (waiter_t){fn, ctx};
    if (rm->state == RM_FETCHING)
        return;
    rm->state = RM_FETCHING;
    reset_docs(rm);
    add_doc(rm, "target.xml");
    fetch_next(rm);
}

void cg_regmap_forget(cg_regmap_t *rm, void *ctx)
{
    for (size_t i = 0; i < rm->nwait; i++)
        if (rm->waiters[i].ctx == ctx)
            rm->waiters[i].fn = NULL;
}

const cg_tdesc_t *cg_regmap_tdesc(const cg_regmap_t *rm)
{
    return rm->state == RM_READY ? &rm->tdesc : NULL;
}

int cg_regmap_value(const cg_regmap_t *rm, const char *hex, size_t n, cg_buf_t *out)
{
    if (n && (hex[0] == 'x' || hex[0] == 'X'))
        return cg_buf_puts(out, "unavailable");
    if (n == 0 || n % 2 || n > 16)
        return cg_buf_append(out, hex, n);
    uint8_t b[8];
    if (cg_hex_decode(hex, n, b, sizeof(b)) < 0)
        return cg_buf_append(out, hex, n);
    size_t nb = n / 2;
    uint64_t v = 0;
    for (size_t i = 0; i < nb; i++) {
        size_t idx = rm->big_endian ? i : nb - 1 - i;
        v = v << 8 | b[idx];
    }
    return cg_buf_printf(out, "0x%0*llx", (int)n, (unsigned long long)v);
}

int cg_regmap_encode(const cg_regmap_t *rm, uint64_t value, unsigned bitsize, cg_buf_t *out)
{
    size_t nb = bitsize / 8;
    if (nb == 0 || nb > 8)
        return -1;
    uint8_t b[8];
    for (size_t i = 0; i < nb; i++) {
        uint8_t byte = (uint8_t)(value >> (8 * i));
        b[rm->big_endian ? nb - 1 - i : i] = byte;
    }
    return cg_hex_encode(out, b, nb);
}

static int wanted(const cg_reg_t *r, const char *which)
{
    if (!which || !*which)
        return r->bitsize <= 64;
    if (strcmp(which, "all") == 0)
        return 1;
    size_t k = strlen(r->name);
    for (const char *p = which; *p;) {
        size_t m = strcspn(p, ",");
        if (m == k && strncasecmp(p, r->name, k) == 0)
            return 1;
        p += m;
        if (*p == ',')
            p++;
    }
    return 0;
}

int cg_regmap_format_g(const cg_regmap_t *rm, const char *g, size_t n, const char *which,
                       cg_buf_t *out)
{
    const cg_tdesc_t *td = cg_regmap_tdesc(rm);
    if (!td) {
        cg_buf_puts(out, "g=");
        cg_buf_append(out, g, n);
        return cg_buf_putc(out, '\n');
    }
    size_t off = 0;
    for (size_t i = 0; i < td->n; i++) {
        const cg_reg_t *r = &td->regs[i];
        size_t w = r->bitsize / 4;
        if (w == 0 || off + w > n)
            break;
        if (wanted(r, which)) {
            cg_buf_printf(out, "%s=", r->name);
            cg_regmap_value(rm, g + off, w, out);
            cg_buf_putc(out, '\n');
        }
        off += w;
    }
    return 0;
}

unsigned cg_regmap_default_bp_kind(const cg_regmap_t *rm)
{
    const cg_tdesc_t *td = cg_regmap_tdesc(rm);
    if (td && (strncmp(td->arch, "i386", 4) == 0 || strstr(td->arch, "x86")))
        return 1;
    return 4;
}

const cg_reg_t *cg_regmap_pc(const cg_regmap_t *rm)
{
    static const char *const names[] = {"pc", "rip", "eip", NULL};
    const cg_tdesc_t *td = cg_regmap_tdesc(rm);
    if (!td)
        return NULL;
    for (int i = 0; names[i]; i++) {
        const cg_reg_t *r = cg_tdesc_find(td, names[i]);
        if (r)
            return r;
    }
    return NULL;
}
