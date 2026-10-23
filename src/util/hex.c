#include "hex.h"

#include <errno.h>
#include <stdlib.h>

int cg_hex_val(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int cg_hex_encode(cg_buf_t *out, const uint8_t *data, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    if (cg_buf_reserve(out, n * 2) != 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        out->data[out->len++] = digits[data[i] >> 4];
        out->data[out->len++] = digits[data[i] & 0xf];
    }
    out->data[out->len] = '\0';
    return 0;
}

long cg_hex_decode(const char *hex, size_t hexlen, uint8_t *out, size_t cap)
{
    if (hexlen % 2 || hexlen / 2 > cap)
        return -1;
    for (size_t i = 0; i < hexlen / 2; i++) {
        int hi = cg_hex_val((unsigned char)hex[2 * i]);
        int lo = cg_hex_val((unsigned char)hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return (long)(hexlen / 2);
}

int cg_parse_u64(const char *s, int force_hex, uint64_t *out)
{
    if (!s || !*s)
        return -1;
    int base = 10;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        base = 16;
    else if (force_hex)
        base = 16;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, base);
    if (errno || !end || *end || end == s || *s == '-')
        return -1;
    *out = v;
    return 0;
}

int cg_hexdump(cg_buf_t *out, uint64_t addr, const uint8_t *data, size_t n)
{
    for (size_t off = 0; off < n; off += 16) {
        size_t row = n - off < 16 ? n - off : 16;
        if (cg_buf_printf(out, "%016llx: ", (unsigned long long)(addr + off)) != 0)
            return -1;
        for (size_t i = 0; i < 16; i++) {
            if (i < row)
                cg_buf_printf(out, "%02x ", data[off + i]);
            else
                cg_buf_puts(out, "   ");
        }
        cg_buf_putc(out, '|');
        for (size_t i = 0; i < row; i++) {
            uint8_t c = data[off + i];
            cg_buf_putc(out, (c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        cg_buf_puts(out, "|\n");
    }
    return 0;
}
