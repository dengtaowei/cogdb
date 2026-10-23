#ifndef CG_BUF_H
#define CG_BUF_H

#include <stdarg.h>
#include <stddef.h>

/* Growable byte buffer. data is always NUL-terminated so text users can
 * treat it as a C string; binary users go by len. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cg_buf_t;

void cg_buf_init(cg_buf_t *b);
void cg_buf_free(cg_buf_t *b);
void cg_buf_reset(cg_buf_t *b);
int cg_buf_reserve(cg_buf_t *b, size_t extra);
int cg_buf_append(cg_buf_t *b, const void *data, size_t n);
int cg_buf_putc(cg_buf_t *b, char c);
int cg_buf_puts(cg_buf_t *b, const char *s);
int cg_buf_printf(cg_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int cg_buf_vprintf(cg_buf_t *b, const char *fmt, va_list ap);
/* Drop the first n bytes. */
void cg_buf_consume(cg_buf_t *b, size_t n);

#endif
