#include "transport.h"

#include <stdio.h>
#include <string.h>

static const cg_transport_t *const k_transports[] = {
    &cg_transport_tcp,
    &cg_transport_unix,
    &cg_transport_serial,
    NULL,
};

const cg_transport_t *const *cg_transport_all(void)
{
    return k_transports;
}

const cg_transport_t *cg_transport_lookup(const char *uri, const char **addr)
{
    for (int i = 0; k_transports[i]; i++) {
        size_t k = strlen(k_transports[i]->scheme);
        if (strncmp(uri, k_transports[i]->scheme, k) == 0 && uri[k] == ':') {
            *addr = uri + k + 1;
            return k_transports[i];
        }
    }
    /* host:port or :port */
    if (strchr(uri, ':') && uri[0] != '/') {
        *addr = uri;
        return &cg_transport_tcp;
    }
    return NULL;
}

int cg_transport_open(const char *uri, char *err, size_t errn)
{
    const char *addr = NULL;
    const cg_transport_t *t = cg_transport_lookup(uri, &addr);
    if (!t) {
        snprintf(err, errn, "unknown transport in '%s'", uri);
        return -1;
    }
    return t->open(addr, err, errn);
}
