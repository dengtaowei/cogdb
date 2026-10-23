#ifndef CG_REGMAP_H
#define CG_REGMAP_H

/* Register names and byte order for the attached target. Pulls the target
 * description over qXfer the first time somebody needs names and drops it
 * when the link goes down. */

#include <stddef.h>
#include <stdint.h>

#include "rsp/tdesc.h"
#include "target.h"
#include "util/buf.h"

typedef struct cg_regmap cg_regmap_t;
typedef void (*cg_regmap_ready_fn)(void *ctx, int ok);

cg_regmap_t *cg_regmap_new(cg_target_t *t, cg_bus_t *bus, int big_endian);
void cg_regmap_free(cg_regmap_t *rm);

/* Calls fn once names are known (ok=1) or cannot be had (ok=0). May call
 * synchronously. */
void cg_regmap_ensure(cg_regmap_t *rm, cg_regmap_ready_fn fn, void *ctx);
void cg_regmap_forget(cg_regmap_t *rm, void *ctx);

/* NULL if no description is loaded. */
const cg_tdesc_t *cg_regmap_tdesc(const cg_regmap_t *rm);

#define CG_REGMAP_MAX_MISSING 512

/* Formats a 'g' reply (RLE-expanded) as "name=value" lines. which is
 * NULL/"" for the scalar registers in g plus the pc, "all", or a comma list
 * of names. Stubs may send a g that stops early (XuanTie sends only x0-x31);
 * wanted registers past its end go to missing[] for reading with 'p'.
 * Returns how many were put there. */
size_t cg_regmap_format_g(const cg_regmap_t *rm, const char *g, size_t n, const char *which,
                          cg_buf_t *out, unsigned *missing, size_t cap);
/* Target-order hex to "0x..." (<= 64 bits) or raw hex; "unavailable" for x's. */
int cg_regmap_value(const cg_regmap_t *rm, const char *hex, size_t n, cg_buf_t *out);
/* Value to target-order hex of bitsize bits. */
int cg_regmap_encode(const cg_regmap_t *rm, uint64_t value, unsigned bitsize, cg_buf_t *out);
/* Software breakpoint kind for Z0 when the caller gives none. */
unsigned cg_regmap_default_bp_kind(const cg_regmap_t *rm);
/* Register that holds the program counter, or NULL. */
const cg_reg_t *cg_regmap_pc(const cg_regmap_t *rm);

#endif
