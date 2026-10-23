#ifndef CG_SYS_H
#define CG_SYS_H

#include <stddef.h>
#include <stdint.h>

#define CG_NAME_MAX 64
#define CG_PATH_MAX 512

uint64_t cg_now_ns(void);
uint64_t cg_now_ms(void);
int cg_set_nonblock(int fd);

/* Instance names are [A-Za-z0-9_.-], 1..63 chars, not starting with '.'. */
int cg_valid_name(const char *name);
/* $XDG_RUNTIME_DIR/cogdb (falls back to /tmp/cogdb-<uid>). */
int cg_runtime_dir(char *out, size_t n);
/* <runtime>/<name>/<leaf>; leaf may be NULL for the directory. */
int cg_instance_path(const char *name, const char *leaf, char *out, size_t n);
int cg_mkdir_p(const char *path);

#endif
