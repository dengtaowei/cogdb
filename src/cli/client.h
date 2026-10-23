#ifndef CG_CLIENT_H
#define CG_CLIENT_H

/* Client side of the control socket, shared by the CLI and MCP server. */

#include <stddef.h>

#include "util/buf.h"

/* Sends one line, reads the reply until the hub closes. Returns 0 if the
 * exchange happened (the reply may still be "ERR ..."), -1 if the hub is
 * unreachable or too slow. */
int cg_ctl_request(const char *name, const char *line, cg_buf_t *reply, int timeout_ms);

/* Sends one line and hands every chunk to fn until EOF or fn returns
 * non-zero. */
int cg_ctl_stream(const char *name, const char *line,
                  int (*fn)(void *ctx, const char *data, size_t n), void *ctx);

/* given, else $COGDB_INSTANCE, else the only running instance. */
int cg_pick_instance(const char *given, char *out, size_t n);
/* One "name<TAB>control.sock path" per running instance. */
int cg_list_instances(cg_buf_t *out);

#endif
