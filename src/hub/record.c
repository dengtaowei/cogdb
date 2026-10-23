#include "record.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "util/sys.h"

struct cg_recorder {
    cg_bus_t *bus;
    FILE *fp;
    uint64_t sub;
    uint64_t t0;
};

static void json_str(FILE *fp, const char *s, size_t n)
{
    fputc('"', fp);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            fprintf(fp, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f)
            fputc(c, fp);
        else
            fprintf(fp, "\\u%04x", c);
    }
    fputc('"', fp);
}

static void on_event(void *ctx, const cg_event_t *ev)
{
    cg_recorder_t *r = ctx;
    fprintf(r->fp, "{\"seq\":%llu,\"ns\":%llu,\"type\":\"%s\",\"origin\":",
            (unsigned long long)ev->seq, (unsigned long long)(ev->ns - r->t0),
            cg_ev_name(ev->type));
    json_str(r->fp, ev->origin ? ev->origin : "", ev->origin ? strlen(ev->origin) : 0);
    if (ev->type == CG_EV_PACKET)
        fprintf(r->fp, ",\"dir\":\"%c\"", ev->dir);
    fputs(",\"data\":", r->fp);
    json_str(r->fp, ev->text, ev->len);
    fputs("}\n", r->fp);
    fflush(r->fp);
}

cg_recorder_t *cg_recorder_new(cg_bus_t *bus, const char *path)
{
    cg_recorder_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->fp = fopen(path, "a");
    if (!r->fp) {
        CG_ERR("record %s: %s", path, strerror(errno));
        free(r);
        return NULL;
    }
    r->bus = bus;
    r->t0 = cg_now_ns();
    r->sub = cg_bus_subscribe(bus, CG_EVM_ALL, on_event, r);
    return r;
}

void cg_recorder_free(cg_recorder_t *r)
{
    if (!r)
        return;
    cg_bus_unsubscribe(r->bus, r->sub);
    fclose(r->fp);
    free(r);
}
