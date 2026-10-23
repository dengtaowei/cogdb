#ifndef CG_TDESC_H
#define CG_TDESC_H

/* GDB target description XML: just enough to name registers. Handles
 * <architecture>, <reg> (name, bitsize, regnum, group) and xi:include. */

#include <stddef.h>

typedef struct {
    char name[32];
    char group[16];
    unsigned regnum;
    unsigned bitsize;
} cg_reg_t;

typedef struct {
    cg_reg_t *regs; /* sorted by regnum after build */
    size_t n, cap;
    char arch[48];
} cg_tdesc_t;

/* Returns the document for href, or NULL if unknown. */
typedef const char *(*cg_tdesc_lookup_fn)(void *ctx, const char *href, size_t *len);
typedef void (*cg_tdesc_href_fn)(void *ctx, const char *href);

void cg_tdesc_init(cg_tdesc_t *t);
void cg_tdesc_free(cg_tdesc_t *t);

/* Reports every xi:include href in one document. */
void cg_tdesc_includes(const char *xml, size_t n, cg_tdesc_href_fn fn, void *ctx);
/* Parses root and its includes, in document order. */
int cg_tdesc_build(cg_tdesc_t *t, const char *root_href, cg_tdesc_lookup_fn lookup, void *ctx);

const cg_reg_t *cg_tdesc_find(const cg_tdesc_t *t, const char *name);
const cg_reg_t *cg_tdesc_by_num(const cg_tdesc_t *t, unsigned regnum);

#endif
