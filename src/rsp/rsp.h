#ifndef CG_RSP_H
#define CG_RSP_H

/* GDB Remote Serial Protocol framing. Pure functions and a byte-at-a-time
 * decoder; no I/O. Payloads are handed out raw (run-length encoding and
 * binary escapes intact) so a proxy can forward them byte-exact. */

#include <stddef.h>
#include <stdint.h>

#include "util/buf.h"

#define CG_RSP_MAX_PAYLOAD (256 * 1024)

typedef enum {
    CG_RSP_PACKET,    /* $payload#cs, checksum good */
    CG_RSP_NOTIFY,    /* %payload#cs */
    CG_RSP_ACK,       /* + */
    CG_RSP_NAK,       /* - */
    CG_RSP_INTERRUPT, /* 0x03 */
    CG_RSP_BADSUM,    /* framed packet whose checksum did not match */
} cg_rsp_kind_t;

typedef struct {
    cg_rsp_kind_t kind;
    const char *data; /* PACKET/NOTIFY/BADSUM payload, valid during the call */
    size_t len;
} cg_rsp_msg_t;

typedef void (*cg_rsp_sink_fn)(void *ctx, const cg_rsp_msg_t *m);

typedef struct {
    int state;
    int notify;
    int csum_digits;
    uint8_t want;
    cg_buf_t body;
} cg_rsp_dec_t;

void cg_rsp_dec_init(cg_rsp_dec_t *d);
void cg_rsp_dec_free(cg_rsp_dec_t *d);
void cg_rsp_dec_reset(cg_rsp_dec_t *d);
/* Every complete message is passed to sink. Bytes outside a frame other than
 * + - and 0x03 are dropped. */
void cg_rsp_feed(cg_rsp_dec_t *d, const uint8_t *data, size_t n,
                 cg_rsp_sink_fn sink, void *ctx);

uint8_t cg_rsp_checksum(const char *data, size_t n);
/* Appends $payload#cs. payload must already be escaped. */
int cg_rsp_frame(cg_buf_t *out, const char *payload, size_t n);
/* Appends binary data with # $ } * escaped. */
int cg_rsp_escape(cg_buf_t *out, const uint8_t *data, size_t n);
/* Expands run-length encoding. Returns 0 or -1 on malformed input. */
int cg_rsp_expand_rle(const char *in, size_t n, cg_buf_t *out);
/* Undoes binary escaping. */
int cg_rsp_unescape(const char *in, size_t n, cg_buf_t *out);

/* Packet classification used by the session layer. */
typedef enum {
    CG_PKT_NORMAL,  /* one reply */
    CG_PKT_RESUME,  /* c C s S vCont; ... reply is a stop reply */
    CG_PKT_NOREPLY, /* k R r: stubs may stay silent */
} cg_pkt_class_t;

cg_pkt_class_t cg_rsp_classify(const char *payload, size_t n);
/* "O<hex>" console output (not "OK"). */
int cg_rsp_is_console(const char *payload, size_t n);
/* T S W X N: a stop or exit reply. */
int cg_rsp_is_stop(const char *payload, size_t n);
int cg_rsp_starts(const char *payload, size_t n, const char *prefix);

#endif
