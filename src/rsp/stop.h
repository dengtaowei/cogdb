#ifndef CG_STOP_H
#define CG_STOP_H

#include <stddef.h>
#include <stdint.h>

#include "util/buf.h"

#define CG_STOP_MAX_REGS 16

typedef enum {
    CG_STOP_SIGNAL,     /* S / T */
    CG_STOP_EXITED,     /* W */
    CG_STOP_TERMINATED, /* X */
    CG_STOP_NOTHREADS,  /* N */
} cg_stop_kind_t;

typedef struct {
    unsigned regnum;
    char hex[66]; /* target byte order, as sent */
} cg_stop_reg_t;

typedef struct {
    cg_stop_kind_t kind;
    int code;           /* signal number or exit status */
    char reason[16];    /* swbreak, hwbreak, watch, rwatch, awatch, ... or "" */
    uint64_t data_addr; /* watch* address */
    int has_data_addr;
    char thread[48];
    int core; /* -1 if not reported */
    cg_stop_reg_t regs[CG_STOP_MAX_REGS];
    int nregs;
} cg_stop_t;

/* payload must be RLE-expanded. Returns 0 on success. */
int cg_stop_parse(const char *payload, size_t n, cg_stop_t *out);

/* One-line, key=value summary without registers, e.g.
 * "signal=5 (SIGTRAP) reason=swbreak thread=p1.1 core=0". */
int cg_stop_describe(const cg_stop_t *s, cg_buf_t *out);
const char *cg_signal_name(int sig);

#endif
