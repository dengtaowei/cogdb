#include "rsp.h"

#include <string.h>

#include "util/hex.h"

enum { ST_IDLE, ST_BODY, ST_ESC, ST_SUM };

void cg_rsp_dec_init(cg_rsp_dec_t *d)
{
    memset(d, 0, sizeof(*d));
    cg_buf_init(&d->body);
}

void cg_rsp_dec_free(cg_rsp_dec_t *d)
{
    cg_buf_free(&d->body);
}

void cg_rsp_dec_reset(cg_rsp_dec_t *d)
{
    d->state = ST_IDLE;
    d->csum_digits = 0;
    d->want = 0;
    cg_buf_reset(&d->body);
}

static void emit(cg_rsp_sink_fn sink, void *ctx, cg_rsp_kind_t k,
                 const char *data, size_t n)
{
    cg_rsp_msg_t m = {k, data, n};
    sink(ctx, &m);
}

void cg_rsp_feed(cg_rsp_dec_t *d, const uint8_t *data, size_t n,
                 cg_rsp_sink_fn sink, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        switch (d->state) {
        case ST_IDLE:
            if (c == '$' || c == '%') {
                d->notify = (c == '%');
                cg_buf_reset(&d->body);
                d->state = ST_BODY;
            } else if (c == '+') {
                emit(sink, ctx, CG_RSP_ACK, NULL, 0);
            } else if (c == '-') {
                emit(sink, ctx, CG_RSP_NAK, NULL, 0);
            } else if (c == 0x03) {
                emit(sink, ctx, CG_RSP_INTERRUPT, NULL, 0);
            }
            break;
        case ST_BODY:
        case ST_ESC:
            if (c == '#' && d->state == ST_BODY) {
                d->state = ST_SUM;
                d->csum_digits = 0;
                d->want = 0;
                break;
            }
            if (d->state == ST_BODY && c == '$') {
                /* A fresh frame start inside a body: the old one was lost. */
                cg_buf_reset(&d->body);
                d->notify = 0;
                break;
            }
            d->state = (d->state == ST_BODY && c == '}') ? ST_ESC : ST_BODY;
            if (d->body.len >= CG_RSP_MAX_PAYLOAD) {
                cg_rsp_dec_reset(d);
                break;
            }
            cg_buf_putc(&d->body, (char)c);
            break;
        case ST_SUM: {
            int v = cg_hex_val(c);
            if (v < 0) {
                emit(sink, ctx, CG_RSP_BADSUM, d->body.data, d->body.len);
                cg_rsp_dec_reset(d);
                break;
            }
            d->want = (uint8_t)(d->want << 4 | v);
            if (++d->csum_digits < 2)
                break;
            const char *body = d->body.data ? d->body.data : "";
            uint8_t got = cg_rsp_checksum(body, d->body.len);
            cg_rsp_kind_t k = got != d->want ? CG_RSP_BADSUM
                              : d->notify    ? CG_RSP_NOTIFY
                                             : CG_RSP_PACKET;
            emit(sink, ctx, k, body, d->body.len);
            cg_rsp_dec_reset(d);
            break;
        }
        }
    }
}

uint8_t cg_rsp_checksum(const char *data, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++)
        s = (uint8_t)(s + (uint8_t)data[i]);
    return s;
}

int cg_rsp_frame(cg_buf_t *out, const char *payload, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t s = cg_rsp_checksum(payload, n);
    if (cg_buf_reserve(out, n + 4) != 0)
        return -1;
    cg_buf_putc(out, '$');
    cg_buf_append(out, payload, n);
    cg_buf_putc(out, '#');
    cg_buf_putc(out, digits[s >> 4]);
    cg_buf_putc(out, digits[s & 0xf]);
    return 0;
}

int cg_rsp_escape(cg_buf_t *out, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = data[i];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            if (cg_buf_putc(out, '}') || cg_buf_putc(out, (char)(c ^ 0x20)))
                return -1;
        } else if (cg_buf_putc(out, (char)c)) {
            return -1;
        }
    }
    return 0;
}

int cg_rsp_expand_rle(const char *in, size_t n, cg_buf_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '}' && i + 1 < n) {
            /* Escaped byte: copy both so a later unescape sees them. */
            cg_buf_putc(out, in[i]);
            cg_buf_putc(out, in[++i]);
            continue;
        }
        if (in[i] != '*') {
            cg_buf_putc(out, in[i]);
            continue;
        }
        if (i == 0 || i + 1 >= n)
            return -1;
        int reps = (unsigned char)in[++i] - 29;
        if (reps < 3)
            return -1;
        char prev = in[i - 2];
        for (int r = 0; r < reps; r++)
            cg_buf_putc(out, prev);
    }
    return 0;
}

int cg_rsp_unescape(const char *in, size_t n, cg_buf_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '}') {
            if (i + 1 >= n)
                return -1;
            cg_buf_putc(out, (char)(in[++i] ^ 0x20));
        } else {
            cg_buf_putc(out, in[i]);
        }
    }
    return 0;
}

int cg_rsp_starts(const char *payload, size_t n, const char *prefix)
{
    size_t k = strlen(prefix);
    return n >= k && memcmp(payload, prefix, k) == 0;
}

cg_pkt_class_t cg_rsp_classify(const char *p, size_t n)
{
    if (n == 0)
        return CG_PKT_NORMAL;
    switch (p[0]) {
    case 'c':
    case 'C':
    case 's':
    case 'S':
        return CG_PKT_RESUME;
    case 'k':
    case 'R':
    case 'r':
        return CG_PKT_NOREPLY;
    case 'v':
        if (cg_rsp_starts(p, n, "vCont;"))
            return CG_PKT_RESUME;
        return CG_PKT_NORMAL;
    default:
        return CG_PKT_NORMAL;
    }
}

int cg_rsp_is_console(const char *p, size_t n)
{
    if (n < 3 || p[0] != 'O' || (n - 1) % 2)
        return 0;
    for (size_t i = 1; i < n; i++)
        if (cg_hex_val((unsigned char)p[i]) < 0)
            return 0;
    return 1;
}

int cg_rsp_is_stop(const char *p, size_t n)
{
    if (n < 1)
        return 0;
    switch (p[0]) {
    case 'T':
    case 'S':
    case 'W':
    case 'X':
        return n >= 3 && cg_hex_val((unsigned char)p[1]) >= 0 &&
               cg_hex_val((unsigned char)p[2]) >= 0;
    case 'N':
        return n == 1;
    default:
        return 0;
    }
}
