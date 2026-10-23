#ifndef CG_HEX_H
#define CG_HEX_H

#include <stddef.h>
#include <stdint.h>

#include "buf.h"

int cg_hex_val(int c);
/* Appends 2n lowercase hex digits. */
int cg_hex_encode(cg_buf_t *out, const uint8_t *data, size_t n);
/* Decodes hexlen digits (must be even). Returns bytes written or -1. */
long cg_hex_decode(const char *hex, size_t hexlen, uint8_t *out, size_t cap);
/* Parses an unsigned number: 0x-prefixed or plain decimal, or bare hex when
 * force_hex. Returns 0 on success. */
int cg_parse_u64(const char *s, int force_hex, uint64_t *out);

/* Canonical "addr: bytes |ascii|" dump, 16 bytes per line. */
int cg_hexdump(cg_buf_t *out, uint64_t addr, const uint8_t *data, size_t n);

#endif
