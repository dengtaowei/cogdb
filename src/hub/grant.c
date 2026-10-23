#include "grant.h"

#include <stdlib.h>
#include <string.h>

#include "util/hex.h"
#include "util/sys.h"

struct cg_grant {
    cg_loop_t *loop;
    cg_bus_t *bus;
    uint64_t sub;
    uint64_t timer;
    uint64_t expires_ms;
    cg_level_t level;
    int active;
};

static const char k_usage[] =
    "usage: monitor cogdb allow halt|run|write [SECONDS]   (default 300)\n"
    "       monitor cogdb revoke\n"
    "       monitor cogdb status\n";

static void end_grant(cg_grant_t *g, const char *why)
{
    if (!g->active)
        return;
    g->active = 0;
    if (g->timer)
        cg_loop_cancel(g->loop, g->timer);
    g->timer = 0;
    cg_bus_emitf(g->bus, CG_EV_NOTE, "gdb", "agent grant ended: %s", why);
}

static void expired(void *ctx)
{
    cg_grant_t *g = ctx;
    g->timer = 0;
    end_grant(g, "expired");
}

static void on_client(void *ctx, const cg_event_t *ev)
{
    if (strncmp(ev->text, "detached", 8) == 0 || strncmp(ev->text, "dropped", 7) == 0)
        end_grant(ctx, "gdb left");
}

cg_grant_t *cg_grant_new(cg_loop_t *loop, cg_bus_t *bus)
{
    cg_grant_t *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->loop = loop;
    g->bus = bus;
    g->sub = cg_bus_subscribe(bus, CG_EVM(CG_EV_CLIENT), on_client, g);
    return g;
}

void cg_grant_free(cg_grant_t *g)
{
    if (!g)
        return;
    if (g->timer)
        cg_loop_cancel(g->loop, g->timer);
    cg_bus_unsubscribe(g->bus, g->sub);
    free(g);
}

int cg_grant_level(const cg_grant_t *g, cg_level_t *out)
{
    if (!g || !g->active)
        return 0;
    *out = g->level;
    return 1;
}

unsigned cg_grant_remaining_s(const cg_grant_t *g)
{
    if (!g || !g->active)
        return 0;
    uint64_t now = cg_now_ms();
    return now >= g->expires_ms ? 0 : (unsigned)((g->expires_ms - now + 999) / 1000);
}

static int next_word(const char **s, char *w, size_t cap)
{
    while (**s == ' ')
        (*s)++;
    size_t k = strcspn(*s, " ");
    if (k == 0 || k >= cap)
        return 0;
    memcpy(w, *s, k);
    w[k] = '\0';
    *s += k;
    return 1;
}

static void allow(cg_grant_t *g, const char *args, cg_buf_t *out)
{
    char lv[16], secs[24], extra[8];
    cg_level_t level;
    uint64_t s = CG_GRANT_DEFAULT_S;
    if (!next_word(&args, lv, sizeof(lv)) || cg_level_parse(lv, &level) != 0 ||
        level == CG_LVL_READ) {
        cg_buf_puts(out, k_usage);
        return;
    }
    if (next_word(&args, secs, sizeof(secs)) &&
        (cg_parse_u64(secs, 0, &s) != 0 || s == 0 || s > CG_GRANT_MAX_S)) {
        cg_buf_printf(out, "cogdb: SECONDS must be 1..%d\n", CG_GRANT_MAX_S);
        return;
    }
    if (next_word(&args, extra, sizeof(extra))) {
        cg_buf_puts(out, k_usage);
        return;
    }
    if (g->timer)
        cg_loop_cancel(g->loop, g->timer);
    g->active = 1;
    g->level = level;
    g->expires_ms = cg_now_ms() + s * 1000;
    g->timer = cg_loop_timer(g->loop, s * 1000, expired, g);
    cg_bus_emitf(g->bus, CG_EV_NOTE, "gdb", "agents granted %s for %llus", cg_level_name(level),
                 (unsigned long long)s);
    cg_buf_printf(out, "cogdb: agents may %s for %llus (until then, or until gdb detaches)\n",
                  cg_level_name(level), (unsigned long long)s);
}

void cg_grant_monitor(cg_grant_t *g, const char *args, cg_buf_t *out)
{
    char verb[16];
    if (!next_word(&args, verb, sizeof(verb))) {
        cg_buf_puts(out, k_usage);
    } else if (strcmp(verb, "allow") == 0) {
        allow(g, args, out);
    } else if (strcmp(verb, "revoke") == 0) {
        int had = g->active;
        end_grant(g, "revoked");
        cg_buf_puts(out, had ? "cogdb: grant revoked\n" : "cogdb: no grant in force\n");
    } else if (strcmp(verb, "status") == 0) {
        if (g->active)
            cg_buf_printf(out, "cogdb: agents may %s for %us more\n", cg_level_name(g->level),
                          cg_grant_remaining_s(g));
        else
            cg_buf_puts(out, "cogdb: no grant in force\n");
    } else {
        cg_buf_puts(out, k_usage);
    }
}
