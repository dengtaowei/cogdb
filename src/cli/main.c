#include <ctype.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "client.h"
#include "hub/hub.h"
#include "transport/transport.h"
#include "util/hex.h"
#include "util/log.h"
#include "util/sys.h"

#ifndef COGDB_VERSION
#define COGDB_VERSION "0.1.0"
#endif

int cg_mcp_main(const char *name);

static void usage(FILE *fp)
{
    fprintf(fp,
            "cogdb %s - share one GDB stub between a human's gdb and agents\n"
            "\n"
            "  cogdb serve --stub URI [options]    run a hub\n"
            "  cogdb list                          running hubs\n"
            "  cogdb mcp [-n NAME]                 MCP server (stdio) for a hub\n"
            "  cogdb [-n NAME] COMMAND [ARGS]      talk to a hub\n"
            "\n"
            "serve options:\n"
            "  -s, --stub URI            stub to hold (see below)\n"
            "  -n, --name NAME           instance name (default: default)\n"
            "  -p, --gdb-port N          port for `target remote` (default 4333, 0 = any)\n"
            "      --gdb-host ADDR       listen address (default 127.0.0.1)\n"
            "      --agent LEVEL         agent level with no gdb attached (default write)\n"
            "      --agent-with-gdb LEVEL  agent level while gdb is attached (default halt)\n"
            "      --detach MODE         gdb D/k: resume | keep | forward (default resume)\n"
            "      --takeover            a new gdb connection replaces the attached one\n"
            "      --record FILE         append every packet and event as JSON Lines\n"
            "      --timeout MS          stub reply timeout (default 10000)\n"
            "      --big-endian          target registers are big-endian\n"
            "      --flow-script FILE    gdb script defining flows (user commands)\n"
            "      --flows NAME,...      flows agents may run from that script\n"
            "      --flow-gdb PATH       gdb used to run flows (default gdb)\n"
            "  -v, --verbose             debug logging\n"
            "\n"
            "levels: read < halt < run < write\n"
            "\n"
            "stubs:\n",
            COGDB_VERSION);
    for (const cg_transport_t *const *t = cg_transport_all(); *t; t++)
        fprintf(fp, "  %s\n", (*t)->usage);
    fprintf(fp,
            "\n"
            "commands:\n"
            "  status | hello | events [N] [TYPES] | watch [TYPES]\n"
            "  halt [MS] | cont [WAIT_MS] | step | wait [MS]\n"
            "  regs [all|NAME,...] | reg NAME | wreg NAME VALUE\n"
            "  mem ADDR LEN | wmem ADDR HEX\n"
            "  break ADDR [sw|hw|write|read|access] [KIND] | unbreak ... | breaks\n"
            "  monitor TEXT | raw PACKET | kick | reconnect | shutdown\n"
            "  exec GDB-COMMAND   (in the user's gdb, after: new-ui mi PATH)\n"
            "  flow NAME [keep|resume]   (batch gdb runs a --flows command)\n"
            "\n"
            "event TYPES: comma list of link,client,run,stop,console,note,packet, or all\n");
}

static int serve_main(int argc, char **argv)
{
    enum {
        OPT_GDB_HOST = 256,
        OPT_AGENT,
        OPT_AGENT_GDB,
        OPT_DETACH,
        OPT_TAKEOVER,
        OPT_RECORD,
        OPT_TIMEOUT,
        OPT_BIG,
        OPT_FLOW_SCRIPT,
        OPT_FLOWS,
        OPT_FLOW_GDB,
    };
    static const struct option opts[] = {
        {"stub", required_argument, NULL, 's'},
        {"name", required_argument, NULL, 'n'},
        {"gdb-port", required_argument, NULL, 'p'},
        {"gdb-host", required_argument, NULL, OPT_GDB_HOST},
        {"agent", required_argument, NULL, OPT_AGENT},
        {"agent-with-gdb", required_argument, NULL, OPT_AGENT_GDB},
        {"detach", required_argument, NULL, OPT_DETACH},
        {"takeover", no_argument, NULL, OPT_TAKEOVER},
        {"record", required_argument, NULL, OPT_RECORD},
        {"timeout", required_argument, NULL, OPT_TIMEOUT},
        {"big-endian", no_argument, NULL, OPT_BIG},
        {"flow-script", required_argument, NULL, OPT_FLOW_SCRIPT},
        {"flows", required_argument, NULL, OPT_FLOWS},
        {"flow-gdb", required_argument, NULL, OPT_FLOW_GDB},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    cg_hub_cfg_t cfg;
    cg_hub_cfg_default(&cfg);
    cfg.name = "default";
    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "s:n:p:vh", opts, NULL)) != -1) {
        uint64_t v;
        switch (c) {
        case 's': cfg.stub_uri = optarg; break;
        case 'n': cfg.name = optarg; break;
        case 'p':
            if (cg_parse_u64(optarg, 0, &v) || v > 65535) {
                fprintf(stderr, "cogdb: bad --gdb-port\n");
                return 2;
            }
            cfg.gdb.port = (int)v;
            break;
        case OPT_GDB_HOST: cfg.gdb.host = optarg; break;
        case OPT_AGENT:
        case OPT_AGENT_GDB:
            if (cg_level_parse(optarg, c == OPT_AGENT ? &cfg.policy.agent
                                                      : &cfg.policy.agent_with_gdb)) {
                fprintf(stderr, "cogdb: level must be read, halt, run or write\n");
                return 2;
            }
            break;
        case OPT_DETACH:
            if (cg_detach_parse(optarg, &cfg.gdb.detach)) {
                fprintf(stderr, "cogdb: --detach must be resume, keep or forward\n");
                return 2;
            }
            break;
        case OPT_TAKEOVER: cfg.gdb.takeover = 1; break;
        case OPT_RECORD: cfg.record_path = optarg; break;
        case OPT_TIMEOUT:
            if (cg_parse_u64(optarg, 0, &v) || v == 0) {
                fprintf(stderr, "cogdb: bad --timeout\n");
                return 2;
            }
            cfg.timeout_ms = (unsigned)v;
            break;
        case OPT_BIG: cfg.big_endian = 1; break;
        case OPT_FLOW_SCRIPT: cfg.flow.script = optarg; break;
        case OPT_FLOWS: cfg.flow.names = optarg; break;
        case OPT_FLOW_GDB: cfg.flow.gdb = optarg; break;
        case 'v': cg_log_level = CG_LOG_DEBUG; break;
        case 'h': usage(stdout); return 0;
        default: return 2;
        }
    }
    if (!cfg.stub_uri) {
        fprintf(stderr, "cogdb serve: --stub is required (e.g. --stub :3333)\n");
        return 2;
    }
    if (!cg_valid_name(cfg.name)) {
        fprintf(stderr, "cogdb serve: invalid --name\n");
        return 2;
    }
    const char *addr;
    if (!cg_transport_lookup(cfg.stub_uri, &addr)) {
        fprintf(stderr, "cogdb serve: unknown stub URI '%s'\n", cfg.stub_uri);
        return 2;
    }
    return cg_hub_run(&cfg);
}

static int print_chunk(void *ctx, const char *data, size_t n)
{
    (void)ctx;
    fwrite(data, 1, n, stdout);
    fflush(stdout);
    return 0;
}

/* MEM replies carry hex; show them the way people read memory. */
static void print_mem(const char *reply)
{
    const char *nl = strchr(reply, '\n');
    const char *ap = strstr(reply, "addr=");
    if (!nl || !ap) {
        fputs(reply, stdout);
        return;
    }
    uint64_t addr = strtoull(ap + 5, NULL, 16);
    const char *hex = nl + 1;
    size_t hl = strcspn(hex, "\n");
    uint8_t *bytes = malloc(hl / 2 + 1);
    long got = bytes ? cg_hex_decode(hex, hl, bytes, hl / 2 + 1) : -1;
    if (got < 0) {
        fputs(reply, stdout);
        free(bytes);
        return;
    }
    cg_buf_t dump;
    cg_buf_init(&dump);
    cg_hexdump(&dump, addr, bytes, (size_t)got);
    fwrite(reply, 1, (size_t)(nl - reply + 1), stdout);
    fputs(dump.data ? dump.data : "", stdout);
    cg_buf_free(&dump);
    free(bytes);
}

static int timeout_for(const char *verb, const char *args)
{
    if (!strcasecmp(verb, "monitor") || !strcasecmp(verb, "exec"))
        return 130000;
    if (!strcasecmp(verb, "flow"))
        return 310000;
    if (!strcasecmp(verb, "wait") || !strcasecmp(verb, "halt") || !strcasecmp(verb, "cont")) {
        long ms = args && *args ? strtol(args, NULL, 10) : 0;
        if (ms <= 0)
            ms = !strcasecmp(verb, "halt") ? 3000 : 30000;
        return (int)ms + 3000;
    }
    return 15000;
}

static int client_main(const char *name, int argc, char **argv)
{
    char inst[CG_NAME_MAX];
    if (cg_pick_instance(name, inst, sizeof(inst)) != 0) {
        fprintf(stderr, "cogdb: no hub selected (use -n NAME; `cogdb list` shows running hubs)\n");
        return 3;
    }
    cg_buf_t line, args;
    cg_buf_init(&line);
    cg_buf_init(&args);
    for (const char *p = argv[0]; *p; p++)
        cg_buf_putc(&line, (char)toupper((unsigned char)*p));
    for (int i = 1; i < argc; i++) {
        cg_buf_printf(&args, "%s%s", i > 1 ? " " : "", argv[i]);
        cg_buf_printf(&line, " %s", argv[i]);
    }

    int rc;
    if (!strcasecmp(argv[0], "watch")) {
        rc = cg_ctl_stream(inst, line.data, print_chunk, NULL) == 0 ? 0 : 3;
    } else {
        cg_buf_t reply;
        cg_buf_init(&reply);
        if (cg_ctl_request(inst, line.data, &reply, timeout_for(argv[0], args.data)) != 0) {
            fprintf(stderr, "cogdb: hub '%s' not reachable\n", inst);
            rc = 3;
        } else {
            const char *r = reply.data ? reply.data : "";
            if (!strcasecmp(argv[0], "mem") && !strncmp(r, "OK", 2))
                print_mem(r);
            else
                fputs(r, stdout);
            rc = strncmp(r, "OK", 2) == 0 ? 0 : 1;
        }
        cg_buf_free(&reply);
    }
    cg_buf_free(&line);
    cg_buf_free(&args);
    return rc;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--name")) && i + 1 < argc) {
            name = argv[i + 1];
            i += 2;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--version")) {
            printf("cogdb %s\n", COGDB_VERSION);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    const char *cmd = argv[i];
    if (!strcmp(cmd, "serve"))
        return serve_main(argc - i, argv + i);
    if (!strcmp(cmd, "list")) {
        cg_buf_t out;
        cg_buf_init(&out);
        int n = cg_list_instances(&out);
        fputs(n > 0 ? out.data : "(no running hubs)\n", stdout);
        cg_buf_free(&out);
        return 0;
    }
    if (!strcmp(cmd, "mcp")) {
        for (int j = i + 1; j + 1 < argc; j++)
            if (!strcmp(argv[j], "-n") || !strcmp(argv[j], "--name"))
                name = argv[j + 1];
        return cg_mcp_main(name);
    }
    if (!strcmp(cmd, "help")) {
        usage(stdout);
        return 0;
    }
    return client_main(name, argc - i, argv + i);
}
