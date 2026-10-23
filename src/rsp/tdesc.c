#include "tdesc.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_DEPTH 8

typedef struct {
    const char *p, *end;
} scan_t;

typedef struct {
    char name[24];
    char attr[6][2][64]; /* up to 6 key/value pairs */
    int nattr;
    int closing;
    const char *body; /* text after '>' */
} tag_t;

static void copy(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *find_str(const char *p, const char *end, const char *needle)
{
    size_t k = strlen(needle);
    for (; p + k <= end; p++)
        if (memcmp(p, needle, k) == 0)
            return p;
    return NULL;
}

/* Advances to the next element tag. Skips comments, PIs and DOCTYPE. */
static int next_tag(scan_t *s, tag_t *t)
{
    for (;;) {
        const char *lt = memchr(s->p, '<', (size_t)(s->end - s->p));
        if (!lt)
            return 0;
        s->p = lt + 1;
        if (find_str(s->p, s->end, "!--") == s->p) {
            const char *e = find_str(s->p, s->end, "-->");
            s->p = e ? e + 3 : s->end;
            continue;
        }
        if (s->p < s->end && (*s->p == '?' || *s->p == '!')) {
            const char *gt = memchr(s->p, '>', (size_t)(s->end - s->p));
            s->p = gt ? gt + 1 : s->end;
            continue;
        }
        break;
    }
    memset(t, 0, sizeof(*t));
    const char *q = s->p;
    if (q < s->end && *q == '/') {
        t->closing = 1;
        q++;
    }
    const char *nb = q;
    while (q < s->end && !is_space(*q) && *q != '>' && *q != '/')
        q++;
    copy(t->name, sizeof(t->name), nb, (size_t)(q - nb));
    while (q < s->end && *q != '>') {
        while (q < s->end && (is_space(*q) || *q == '/'))
            q++;
        if (q >= s->end || *q == '>')
            break;
        const char *kb = q;
        while (q < s->end && *q != '=' && !is_space(*q) && *q != '>')
            q++;
        const char *ke = q;
        while (q < s->end && is_space(*q))
            q++;
        if (q >= s->end || *q != '=')
            continue;
        q++;
        while (q < s->end && is_space(*q))
            q++;
        if (q >= s->end || (*q != '"' && *q != '\''))
            continue;
        char quote = *q++;
        const char *vb = q;
        while (q < s->end && *q != quote)
            q++;
        if (t->nattr < 6) {
            copy(t->attr[t->nattr][0], 64, kb, (size_t)(ke - kb));
            copy(t->attr[t->nattr][1], 64, vb, (size_t)(q - vb));
            t->nattr++;
        }
        if (q < s->end)
            q++;
    }
    s->p = q < s->end ? q + 1 : s->end;
    t->body = s->p;
    return 1;
}

static const char *attr(const tag_t *t, const char *key)
{
    for (int i = 0; i < t->nattr; i++)
        if (strcmp(t->attr[i][0], key) == 0)
            return t->attr[i][1];
    return NULL;
}

void cg_tdesc_init(cg_tdesc_t *t)
{
    memset(t, 0, sizeof(*t));
}

void cg_tdesc_free(cg_tdesc_t *t)
{
    free(t->regs);
    cg_tdesc_init(t);
}

void cg_tdesc_includes(const char *xml, size_t n, cg_tdesc_href_fn fn, void *ctx)
{
    scan_t s = {xml, xml + n};
    tag_t t;
    while (next_tag(&s, &t)) {
        if (!t.closing && strcmp(t.name, "xi:include") == 0) {
            const char *href = attr(&t, "href");
            if (href && href[0])
                fn(ctx, href);
        }
    }
}

typedef struct {
    cg_tdesc_t *t;
    cg_tdesc_lookup_fn lookup;
    void *ctx;
    unsigned next_regnum;
} build_t;

static int add_reg(build_t *b, const tag_t *tag)
{
    cg_tdesc_t *t = b->t;
    const char *name = attr(tag, "name");
    const char *bits = attr(tag, "bitsize");
    if (!name || !bits)
        return 0;
    if (t->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 64;
        cg_reg_t *p = realloc(t->regs, cap * sizeof(*p));
        if (!p)
            return -1;
        t->regs = p;
        t->cap = cap;
    }
    cg_reg_t *r = &t->regs[t->n++];
    memset(r, 0, sizeof(*r));
    copy(r->name, sizeof(r->name), name, strlen(name));
    const char *grp = attr(tag, "group");
    if (grp)
        copy(r->group, sizeof(r->group), grp, strlen(grp));
    r->bitsize = (unsigned)strtoul(bits, NULL, 10);
    const char *num = attr(tag, "regnum");
    r->regnum = num ? (unsigned)strtoul(num, NULL, 10) : b->next_regnum;
    b->next_regnum = r->regnum + 1;
    return 0;
}

static int build_doc(build_t *b, const char *href, int depth)
{
    if (depth > MAX_DEPTH)
        return -1;
    size_t n = 0;
    const char *xml = b->lookup(b->ctx, href, &n);
    if (!xml)
        return -1;
    scan_t s = {xml, xml + n};
    tag_t t;
    while (next_tag(&s, &t)) {
        if (t.closing)
            continue;
        if (strcmp(t.name, "reg") == 0) {
            if (add_reg(b, &t) != 0)
                return -1;
        } else if (strcmp(t.name, "xi:include") == 0) {
            const char *inc = attr(&t, "href");
            if (inc && build_doc(b, inc, depth + 1) != 0)
                return -1;
        } else if (strcmp(t.name, "architecture") == 0) {
            const char *e = memchr(t.body, '<', (size_t)(s.end - t.body));
            if (e)
                copy(b->t->arch, sizeof(b->t->arch), t.body, (size_t)(e - t.body));
        }
    }
    return 0;
}

static int by_regnum(const void *a, const void *b)
{
    unsigned x = ((const cg_reg_t *)a)->regnum, y = ((const cg_reg_t *)b)->regnum;
    return x < y ? -1 : x > y;
}

int cg_tdesc_build(cg_tdesc_t *t, const char *root_href, cg_tdesc_lookup_fn lookup, void *ctx)
{
    build_t b = {t, lookup, ctx, 0};
    if (build_doc(&b, root_href, 0) != 0)
        return -1;
    if (t->n)
        qsort(t->regs, t->n, sizeof(*t->regs), by_regnum);
    return t->n ? 0 : -1;
}

const cg_reg_t *cg_tdesc_find(const cg_tdesc_t *t, const char *name)
{
    for (size_t i = 0; i < t->n; i++)
        if (strcasecmp(t->regs[i].name, name) == 0)
            return &t->regs[i];
    return NULL;
}

const cg_reg_t *cg_tdesc_by_num(const cg_tdesc_t *t, unsigned regnum)
{
    for (size_t i = 0; i < t->n; i++)
        if (t->regs[i].regnum == regnum)
            return &t->regs[i];
    return NULL;
}
