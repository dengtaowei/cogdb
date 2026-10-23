#ifndef CG_LOG_H
#define CG_LOG_H

typedef enum {
    CG_LOG_ERROR = 0,
    CG_LOG_WARN,
    CG_LOG_INFO,
    CG_LOG_DEBUG,
} cg_log_level_t;

extern cg_log_level_t cg_log_level;

void cg_log(cg_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define CG_ERR(...)   cg_log(CG_LOG_ERROR, __VA_ARGS__)
#define CG_WARN(...)  cg_log(CG_LOG_WARN, __VA_ARGS__)
#define CG_INFO(...)  cg_log(CG_LOG_INFO, __VA_ARGS__)
#define CG_DEBUG(...) cg_log(CG_LOG_DEBUG, __VA_ARGS__)

#endif
