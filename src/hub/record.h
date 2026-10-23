#ifndef CG_RECORD_H
#define CG_RECORD_H

/* Session recorder: every bus event, packets included, as JSON Lines.
 *
 *   {"seq":12,"ns":81234,"type":"packet","origin":"gdb","dir":">","data":"g"}
 */

#include "events.h"

typedef struct cg_recorder cg_recorder_t;

cg_recorder_t *cg_recorder_new(cg_bus_t *bus, const char *path);
void cg_recorder_free(cg_recorder_t *r);

#endif
