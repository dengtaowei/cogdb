#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cg_buf_init(cg_buf_t *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void cg_buf_free(cg_buf_t *b)
{
    free(b->data);
    cg_buf_init(b);
}

void cg_buf_reset(cg_buf_t *b)
{
    b->len = 0;
    if (b->data)
        b->data[0] = '\0';
}

int cg_buf_reserve(cg_buf_t *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int cg_buf_append(cg_buf_t *b, const void *data, size_t n)
{
    if (cg_buf_reserve(b, n) != 0)
        return -1;
    if (n)
        memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int cg_buf_putc(cg_buf_t *b, char c)
{
    return cg_buf_append(b, &c, 1);
}

int cg_buf_puts(cg_buf_t *b, const char *s)
{
    return cg_buf_append(b, s, strlen(s));
}

int cg_buf_vprintf(cg_buf_t *b, const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0 || cg_buf_reserve(b, (size_t)n) != 0)
        return -1;
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
    return 0;
}

int cg_buf_printf(cg_buf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = cg_buf_vprintf(b, fmt, ap);
    va_end(ap);
    return rc;
}

void cg_buf_consume(cg_buf_t *b, size_t n)
{
    if (n >= b->len) {
        cg_buf_reset(b);
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
    b->data[b->len] = '\0';
}
