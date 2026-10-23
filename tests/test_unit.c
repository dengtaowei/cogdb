/* Unit tests for the pure layers: framing, stop replies, target
 * descriptions, the event bus and policy. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub/events.h"
#include "hub/policy.h"
#include "rsp/rsp.h"
#include "rsp/stop.h"
#include "rsp/tdesc.h"
#include "util/hex.h"

static int g_fail, g_run;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_run++;                                                                    \
        if (!(cond)) {                                                              \
            g_fail++;                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                           \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

/* ---- rsp framing ------------------------------------------------------- */

typedef struct {
    cg_rsp_kind_t kind[16];
    char data[16][64];
    int n;
} sink_t;

static void sink(void *ctx, const cg_rsp_msg_t *m)
{
    sink_t *s = ctx;
    if (s->n >= 16)
        return;
    s->kind[s->n] = m->kind;
    snprintf(s->data[s->n], sizeof(s->data[0]), "%.*s", (int)m->len, m->data ? m->data : "");
    s->n++;
}

static void feed(cg_rsp_dec_t *d, sink_t *s, const char *bytes)
{
    cg_rsp_feed(d, (const uint8_t *)bytes, strlen(bytes), sink, s);
}

static void test_frame(void)
{
    cg_buf_t b;
    cg_buf_init(&b);
    cg_rsp_frame(&b, "g", 1);
    CHECK_STR(b.data, "$g#67");
    cg_buf_reset(&b);
    cg_rsp_frame(&b, "", 0);
    CHECK_STR(b.data, "$#00");
    cg_buf_free(&b);
}

static void test_decoder(void)
{
    cg_rsp_dec_t d;
    sink_t s = {0};
    cg_rsp_dec_init(&d);

    /* split across reads, acks and an interrupt around it */
    feed(&d, &s, "+$O");
    feed(&d, &s, "K#9");
    feed(&d, &s, "a-\x03");
    CHECK(s.n == 4);
    CHECK(s.kind[0] == CG_RSP_ACK);
    CHECK(s.kind[1] == CG_RSP_PACKET);
    CHECK_STR(s.data[1], "OK");
    CHECK(s.kind[2] == CG_RSP_NAK);
    CHECK(s.kind[3] == CG_RSP_INTERRUPT);

    /* bad checksum, then a notification */
    memset(&s, 0, sizeof(s));
    feed(&d, &s, "$OK#00%Stop:T05#");
    char cs[3];
    snprintf(cs, sizeof(cs), "%02x", cg_rsp_checksum("Stop:T05", 8));
    feed(&d, &s, cs);
    CHECK(s.n == 2);
    CHECK(s.kind[0] == CG_RSP_BADSUM);
    CHECK(s.kind[1] == CG_RSP_NOTIFY);
    CHECK_STR(s.data[1], "Stop:T05");

    /* an escaped '#' inside binary data does not end the frame */
    memset(&s, 0, sizeof(s));
    const char body[] = "X0,1:}\x03";
    cg_buf_t f;
    cg_buf_init(&f);
    cg_rsp_frame(&f, body, sizeof(body) - 1);
    feed(&d, &s, f.data);
    CHECK(s.n == 1 && s.kind[0] == CG_RSP_PACKET);
    cg_buf_free(&f);

    /* garbage between frames is ignored */
    memset(&s, 0, sizeof(s));
    feed(&d, &s, "noise$g#67");
    CHECK(s.n == 1 && s.kind[0] == CG_RSP_PACKET);
    CHECK_STR(s.data[0], "g");
    cg_rsp_dec_free(&d);
}

static void test_rle_escape(void)
{
    cg_buf_t b;
    cg_buf_init(&b);
    /* '0* ' = '0' + 3 more (' ' is 32, 32-29 = 3) */
    CHECK(cg_rsp_expand_rle("0* ", 3, &b) == 0);
    CHECK_STR(b.data, "0000");
    cg_buf_reset(&b);
    CHECK(cg_rsp_expand_rle("ab*\"c", 5, &b) == 0); /* '"' = 34 -> 5 more */
    CHECK_STR(b.data, "abbbbbbc");
    cg_buf_reset(&b);
    CHECK(cg_rsp_expand_rle("*!", 2, &b) != 0);

    cg_buf_reset(&b);
    const uint8_t raw[] = {'#', 'a', '}', '$', '*'};
    cg_rsp_escape(&b, raw, sizeof(raw));
    CHECK(b.len == 9);
    cg_buf_t back;
    cg_buf_init(&back);
    CHECK(cg_rsp_unescape(b.data, b.len, &back) == 0);
    CHECK(back.len == sizeof(raw) && memcmp(back.data, raw, sizeof(raw)) == 0);
    cg_buf_free(&back);
    cg_buf_free(&b);
}

static void test_classify(void)
{
    CHECK(cg_rsp_classify("c", 1) == CG_PKT_RESUME);
    CHECK(cg_rsp_classify("vCont;c", 7) == CG_PKT_RESUME);
    CHECK(cg_rsp_classify("vCont?", 6) == CG_PKT_NORMAL);
    CHECK(cg_rsp_classify("k", 1) == CG_PKT_NOREPLY);
    CHECK(cg_rsp_classify("g", 1) == CG_PKT_NORMAL);
    CHECK(cg_rsp_is_console("O48690a", 7));
    CHECK(!cg_rsp_is_console("OK", 2));
    CHECK(cg_rsp_is_stop("T05", 3));
    CHECK(cg_rsp_is_stop("W00", 3));
    CHECK(!cg_rsp_is_stop("OK", 2));
    CHECK(!cg_rsp_is_stop("E01", 3));
}

/* ---- stop replies ------------------------------------------------------ */

static void test_stop(void)
{
    cg_stop_t s;
    const char *t = "T05thread:p1f.1f;core:2;10:3611400000000000;swbreak:;";
    CHECK(cg_stop_parse(t, strlen(t), &s) == 0);
    CHECK(s.kind == CG_STOP_SIGNAL && s.code == 5);
    CHECK_STR(s.thread, "p1f.1f");
    CHECK(s.core == 2);
    CHECK_STR(s.reason, "swbreak");
    CHECK(s.nregs == 1 && s.regs[0].regnum == 0x10);
    CHECK_STR(s.regs[0].hex, "3611400000000000");

    const char *w = "T05watch:20001000;";
    CHECK(cg_stop_parse(w, strlen(w), &s) == 0);
    CHECK_STR(s.reason, "watch");
    CHECK(s.has_data_addr && s.data_addr == 0x20001000);

    CHECK(cg_stop_parse("W01", 3, &s) == 0 && s.kind == CG_STOP_EXITED && s.code == 1);
    CHECK(cg_stop_parse("X09", 3, &s) == 0 && s.kind == CG_STOP_TERMINATED);
    CHECK(cg_stop_parse("OK", 2, &s) != 0);

    cg_buf_t b;
    cg_buf_init(&b);
    cg_stop_parse("T02thread:01;", 13, &s);
    cg_stop_describe(&s, &b);
    CHECK_STR(b.data, "signal=2 (SIGINT) thread=01");
    cg_buf_free(&b);
}

/* ---- target descriptions ----------------------------------------------- */

static const char k_root[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target>\n"
    "  <architecture>riscv:rv32</architecture>\n"
    "  <!-- <reg name=\"commented\" bitsize=\"32\"/> -->\n"
    "  <xi:include href=\"core.xml\"/>\n"
    "  <feature name=\"csr\">\n"
    "    <reg name=\"mstatus\" bitsize=\"32\" regnum=\"833\" group=\"csr\"/>\n"
    "  </feature>\n"
    "</target>\n";

static const char k_core[] =
    "<feature name=\"org.gnu.gdb.riscv.cpu\">\n"
    "  <reg name='zero' bitsize='32' regnum='0'/>\n"
    "  <reg name=\"ra\" bitsize=\"32\"/>\n"
    "  <reg name=\"pc\" bitsize=\"32\" regnum=\"32\"/>\n"
    "</feature>\n";

static const char *lookup(void *ctx, const char *href, size_t *len)
{
    (void)ctx;
    const char *doc = !strcmp(href, "target.xml") ? k_root : !strcmp(href, "core.xml") ? k_core : NULL;
    if (doc)
        *len = strlen(doc);
    return doc;
}

static void count_href(void *ctx, const char *href)
{
    (void)href;
    (*(int *)ctx)++;
}

static void test_tdesc(void)
{
    int n = 0;
    cg_tdesc_includes(k_root, strlen(k_root), count_href, &n);
    CHECK(n == 1);

    cg_tdesc_t t;
    cg_tdesc_init(&t);
    CHECK(cg_tdesc_build(&t, "target.xml", lookup, NULL) == 0);
    CHECK_STR(t.arch, "riscv:rv32");
    CHECK(t.n == 4);
    CHECK(cg_tdesc_find(&t, "commented") == NULL);
    const cg_reg_t *ra = cg_tdesc_find(&t, "RA");
    CHECK(ra && ra->regnum == 1 && ra->bitsize == 32);
    const cg_reg_t *pc = cg_tdesc_find(&t, "pc");
    CHECK(pc && pc->regnum == 32);
    /* sorted by regnum, even though mstatus came first in the document */
    CHECK_STR(t.regs[0].name, "zero");
    CHECK_STR(t.regs[3].name, "mstatus");
    CHECK(cg_tdesc_by_num(&t, 32) == pc);
    cg_tdesc_free(&t);
}

/* ---- event bus --------------------------------------------------------- */

typedef struct {
    int stops;
    int all;
    uint64_t self_id;
    cg_bus_t *bus;
} counter_t;

static void count_stop(void *ctx, const cg_event_t *ev)
{
    counter_t *c = ctx;
    if (ev->type == CG_EV_STOP)
        c->stops++;
    /* unsubscribing from inside a callback must be safe */
    cg_bus_unsubscribe(c->bus, c->self_id);
}

static void count_all(void *ctx, const cg_event_t *ev)
{
    (void)ev;
    ((counter_t *)ctx)->all++;
}

static void test_bus(void)
{
    cg_bus_t *b = cg_bus_new(4);
    counter_t once = {0}, every = {0};
    once.bus = b;
    once.self_id = cg_bus_subscribe(b, CG_EVM(CG_EV_STOP), count_stop, &once);
    cg_bus_subscribe(b, CG_EVM_ALL, count_all, &every);

    cg_bus_emitf(b, CG_EV_RUN, "agent", "c");
    cg_bus_emitf(b, CG_EV_STOP, "target", "signal=5");
    cg_bus_emitf(b, CG_EV_STOP, "target", "signal=2");
    cg_bus_packet(b, "gdb", '>', "g", 1);
    cg_bus_emitf(b, CG_EV_NOTE, "hub", "line\nbreak");
    CHECK(once.stops == 1);
    CHECK(every.all == 5);

    cg_buf_t h;
    cg_buf_init(&h);
    cg_bus_history(b, 10, CG_EVM_DEFAULT, &h);
    /* ring holds 4: stop, stop, packet, note; packet filtered out */
    CHECK(strstr(h.data, " run ") == NULL);
    CHECK(strstr(h.data, "signal=2") != NULL);
    CHECK(strstr(h.data, "packet") == NULL);
    CHECK(strstr(h.data, "line\\nbreak") != NULL);
    cg_buf_reset(&h);
    cg_bus_history(b, 1, CG_EVM(CG_EV_STOP), &h);
    CHECK(strstr(h.data, "signal=2") && !strstr(h.data, "signal=5"));
    cg_buf_free(&h);

    unsigned m;
    CHECK(cg_ev_parse_mask("stop,run", &m) == 0 && m == (CG_EVM(CG_EV_STOP) | CG_EVM(CG_EV_RUN)));
    CHECK(cg_ev_parse_mask("bogus", &m) != 0);
    cg_bus_free(b);
}

/* ---- policy ------------------------------------------------------------ */

static void test_policy(void)
{
    cg_policy_t p;
    cg_policy_default(&p);
    CHECK(cg_policy_effective(&p, 0) == CG_LVL_WRITE);
    CHECK(cg_policy_effective(&p, 1) == CG_LVL_HALT);
    p.agent = CG_LVL_READ;
    CHECK(cg_policy_effective(&p, 1) == CG_LVL_READ);
    cg_level_t l;
    CHECK(cg_level_parse("run", &l) == 0 && l == CG_LVL_RUN);
    CHECK(cg_level_parse("root", &l) != 0);
}

static void test_hex(void)
{
    uint64_t v;
    CHECK(cg_parse_u64("0x10", 0, &v) == 0 && v == 16);
    CHECK(cg_parse_u64("10", 0, &v) == 0 && v == 10);
    CHECK(cg_parse_u64("10", 1, &v) == 0 && v == 16);
    CHECK(cg_parse_u64("1z", 1, &v) != 0);
    uint8_t out[4];
    CHECK(cg_hex_decode("dead", 4, out, 4) == 2 && out[0] == 0xde);
    CHECK(cg_hex_decode("abc", 3, out, 4) < 0);
}

int main(void)
{
    test_frame();
    test_decoder();
    test_rle_escape();
    test_classify();
    test_stop();
    test_tdesc();
    test_bus();
    test_policy();
    test_hex();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
