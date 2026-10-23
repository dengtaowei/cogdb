#include "stop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/hex.h"

static int hex2(const char *p)
{
    int hi = cg_hex_val((unsigned char)p[0]);
    int lo = cg_hex_val((unsigned char)p[1]);
    return (hi < 0 || lo < 0) ? -1 : hi << 4 | lo;
}

static void copy_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int is_hex_str(const char *p, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (cg_hex_val((unsigned char)p[i]) < 0)
            return 0;
    return 1;
}

static void parse_pair(cg_stop_t *s, const char *k, size_t kn, const char *v, size_t vn)
{
    static const char *const reasons[] = {
        "watch", "rwatch", "awatch", "swbreak", "hwbreak", "library",
        "replaylog", "fork", "vfork", "vforkdone", "exec", "create",
        "syscall_entry", "syscall_return", NULL,
    };
    if (kn == 6 && memcmp(k, "thread", 6) == 0) {
        copy_field(s->thread, sizeof(s->thread), v, vn);
        return;
    }
    if (kn == 4 && memcmp(k, "core", 4) == 0) {
        char tmp[16];
        copy_field(tmp, sizeof(tmp), v, vn);
        s->core = (int)strtol(tmp, NULL, 16);
        return;
    }
    for (int i = 0; reasons[i]; i++) {
        if (strlen(reasons[i]) == kn && memcmp(k, reasons[i], kn) == 0) {
            copy_field(s->reason, sizeof(s->reason), k, kn);
            if (kn >= 5 && memcmp(k + kn - 5, "watch", 5) == 0 && vn) {
                char tmp[24];
                copy_field(tmp, sizeof(tmp), v, vn);
                s->data_addr = strtoull(tmp, NULL, 16);
                s->has_data_addr = 1;
            }
            return;
        }
    }
    if (is_hex_str(k, kn) && kn <= 8 && s->nregs < CG_STOP_MAX_REGS) {
        char tmp[16];
        copy_field(tmp, sizeof(tmp), k, kn);
        cg_stop_reg_t *r = &s->regs[s->nregs++];
        r->regnum = (unsigned)strtoul(tmp, NULL, 16);
        copy_field(r->hex, sizeof(r->hex), v, vn);
    }
}

int cg_stop_parse(const char *p, size_t n, cg_stop_t *s)
{
    memset(s, 0, sizeof(*s));
    s->core = -1;
    if (n == 1 && p[0] == 'N') {
        s->kind = CG_STOP_NOTHREADS;
        return 0;
    }
    if (n < 3)
        return -1;
    int code = hex2(p + 1);
    if (code < 0)
        return -1;
    s->code = code;
    switch (p[0]) {
    case 'S':
        s->kind = CG_STOP_SIGNAL;
        return 0;
    case 'W':
        s->kind = CG_STOP_EXITED;
        return 0;
    case 'X':
        s->kind = CG_STOP_TERMINATED;
        return 0;
    case 'T':
        s->kind = CG_STOP_SIGNAL;
        break;
    default:
        return -1;
    }
    const char *q = p + 3, *end = p + n;
    while (q < end) {
        const char *semi = memchr(q, ';', (size_t)(end - q));
        if (!semi)
            semi = end;
        const char *colon = memchr(q, ':', (size_t)(semi - q));
        if (colon)
            parse_pair(s, q, (size_t)(colon - q), colon + 1, (size_t)(semi - colon - 1));
        else if (semi > q)
            parse_pair(s, q, (size_t)(semi - q), semi, 0);
        q = semi + 1;
    }
    return 0;
}

const char *cg_signal_name(int sig)
{
    /* GDB's target-independent signal numbering, not the host's. */
    switch (sig) {
    case 0: return "none";
    case 1: return "SIGHUP";
    case 2: return "SIGINT";
    case 3: return "SIGQUIT";
    case 4: return "SIGILL";
    case 5: return "SIGTRAP";
    case 6: return "SIGABRT";
    case 7: return "SIGEMT";
    case 8: return "SIGFPE";
    case 9: return "SIGKILL";
    case 10: return "SIGBUS";
    case 11: return "SIGSEGV";
    case 12: return "SIGSYS";
    case 13: return "SIGPIPE";
    case 14: return "SIGALRM";
    case 15: return "SIGTERM";
    default: return "?";
    }
}

int cg_stop_describe(const cg_stop_t *s, cg_buf_t *out)
{
    switch (s->kind) {
    case CG_STOP_EXITED:
        return cg_buf_printf(out, "exited code=%d", s->code);
    case CG_STOP_TERMINATED:
        return cg_buf_printf(out, "terminated signal=%d (%s)", s->code,
                             cg_signal_name(s->code));
    case CG_STOP_NOTHREADS:
        return cg_buf_puts(out, "no-threads");
    case CG_STOP_SIGNAL:
        break;
    }
    cg_buf_printf(out, "signal=%d (%s)", s->code, cg_signal_name(s->code));
    if (s->reason[0])
        cg_buf_printf(out, " reason=%s", s->reason);
    if (s->has_data_addr)
        cg_buf_printf(out, " addr=0x%llx", (unsigned long long)s->data_addr);
    if (s->thread[0])
        cg_buf_printf(out, " thread=%s", s->thread);
    if (s->core >= 0)
        cg_buf_printf(out, " core=%d", s->core);
    return 0;
}
