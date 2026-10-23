/* MCP server over stdio. Each tool is a row in k_tools: a schema and a
 * function that turns arguments into one control-socket line. Adding a
 * tool means adding a row. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "client.h"
#include "util/hex.h"
#include "util/sys.h"

#ifndef COGDB_VERSION
#define COGDB_VERSION "0.1.0"
#endif

static const char *g_name;

typedef struct {
    cg_buf_t line;   /* control command to send */
    int timeout_ms;  /* client-side wait */
    int hexdump;     /* reply is a MEM reply */
    const char *err; /* argument problem; nothing is sent */
} plan_t;

typedef void (*plan_fn)(cJSON *args, plan_t *p);

typedef struct {
    const char *name;
    const char *desc;
    const char *schema; /* JSON "properties" object */
    const char *required;
    plan_fn plan;
} tool_t;

/* ---- argument helpers -------------------------------------------------- */

static const char *s_arg(cJSON *a, const char *k)
{
    cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, k) : NULL;
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : NULL;
}

static long n_arg(cJSON *a, const char *k, long def)
{
    cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, k) : NULL;
    return cJSON_IsNumber(v) ? (long)v->valuedouble : def;
}

static int b_arg(cJSON *a, const char *k)
{
    cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, k) : NULL;
    return cJSON_IsTrue(v);
}

/* Addresses arrive as "0x1234", "1234" (hex) or a JSON number. */
static int addr_arg(cJSON *a, const char *k, char *out, size_t n)
{
    cJSON *v = a ? cJSON_GetObjectItemCaseSensitive(a, k) : NULL;
    if (cJSON_IsNumber(v)) {
        snprintf(out, n, "0x%llx", (unsigned long long)v->valuedouble);
        return 0;
    }
    uint64_t x;
    if (!cJSON_IsString(v) || cg_parse_u64(v->valuestring, 1, &x))
        return -1;
    snprintf(out, n, "0x%llx", (unsigned long long)x);
    return 0;
}

static int has_space(const char *s)
{
    return strpbrk(s, " \t\r\n") != NULL;
}

/* ---- tool plans -------------------------------------------------------- */

static void plan_status(cJSON *a, plan_t *p)
{
    (void)a;
    cg_buf_puts(&p->line, "STATUS");
}

static void plan_halt(cJSON *a, plan_t *p)
{
    long ms = n_arg(a, "timeout_ms", 3000);
    cg_buf_printf(&p->line, "HALT %ld", ms);
    p->timeout_ms = (int)ms + 3000;
}

static void plan_continue(cJSON *a, plan_t *p)
{
    long ms = n_arg(a, "wait_ms", 0);
    if (ms > 0) {
        cg_buf_printf(&p->line, "CONT %ld", ms);
        p->timeout_ms = (int)ms + 3000;
    } else {
        cg_buf_puts(&p->line, "CONT");
    }
}

static void plan_step(cJSON *a, plan_t *p)
{
    (void)a;
    cg_buf_puts(&p->line, "STEP");
}

static void plan_wait(cJSON *a, plan_t *p)
{
    long ms = n_arg(a, "timeout_ms", 30000);
    cg_buf_printf(&p->line, "WAIT %ld", ms);
    p->timeout_ms = (int)ms + 3000;
}

static void plan_regs(cJSON *a, plan_t *p)
{
    const char *names = s_arg(a, "names");
    if (names && has_space(names)) {
        p->err = "names is a comma list without spaces";
        return;
    }
    cg_buf_puts(&p->line, "REGS");
    if (b_arg(a, "all"))
        cg_buf_puts(&p->line, " all");
    else if (names)
        cg_buf_printf(&p->line, " %s", names);
}

static void plan_read_mem(cJSON *a, plan_t *p)
{
    char addr[32];
    long len = n_arg(a, "length", 64);
    if (addr_arg(a, "address", addr, sizeof(addr))) {
        p->err = "address must be hex (0x...) or a number";
        return;
    }
    cg_buf_printf(&p->line, "MEM %s %ld", addr, len);
    p->hexdump = 1;
}

static void plan_write_mem(cJSON *a, plan_t *p)
{
    char addr[32];
    const char *hex = s_arg(a, "hex");
    if (addr_arg(a, "address", addr, sizeof(addr)) || !hex || has_space(hex)) {
        p->err = "need address and hex (bytes in memory order, no spaces)";
        return;
    }
    cg_buf_printf(&p->line, "WMEM %s %s", addr, hex);
}

static void plan_write_reg(cJSON *a, plan_t *p)
{
    const char *name = s_arg(a, "name"), *value = s_arg(a, "value");
    if (!name || !value || has_space(name) || has_space(value)) {
        p->err = "need name and value";
        return;
    }
    cg_buf_printf(&p->line, "WREG %s %s", name, value);
}

static void plan_breakpoint(cJSON *a, plan_t *p)
{
    const char *action = s_arg(a, "action");
    if (!action || !strcmp(action, "list")) {
        cg_buf_puts(&p->line, "BREAKS");
        return;
    }
    char addr[32];
    if (addr_arg(a, "address", addr, sizeof(addr))) {
        p->err = "address must be hex (0x...) or a number";
        return;
    }
    const char *type = s_arg(a, "type");
    long kind = n_arg(a, "kind", 0);
    if (type && has_space(type)) {
        p->err = "bad type";
        return;
    }
    cg_buf_printf(&p->line, "%s %s %s", !strcmp(action, "clear") ? "UNBREAK" : "BREAK", addr,
                  type ? type : "sw");
    if (kind > 0)
        cg_buf_printf(&p->line, " %ld", kind);
}

static void plan_monitor(cJSON *a, plan_t *p)
{
    const char *cmd = s_arg(a, "command");
    if (!cmd || strpbrk(cmd, "\r\n")) {
        p->err = "need a one-line command";
        return;
    }
    cg_buf_printf(&p->line, "MONITOR %s", cmd);
    p->timeout_ms = 130000;
}

static void plan_exec(cJSON *a, plan_t *p)
{
    const char *cmd = s_arg(a, "command");
    if (!cmd || strpbrk(cmd, "\r\n")) {
        p->err = "need a one-line command";
        return;
    }
    cg_buf_printf(&p->line, "EXEC %s", cmd);
    p->timeout_ms = 130000;
}

static void plan_events(cJSON *a, plan_t *p)
{
    const char *types = s_arg(a, "types");
    if (types && has_space(types)) {
        p->err = "types is a comma list without spaces";
        return;
    }
    cg_buf_printf(&p->line, "EVENTS %ld", n_arg(a, "last", 50));
    if (types)
        cg_buf_printf(&p->line, " %s", types);
}

static void plan_raw(cJSON *a, plan_t *p)
{
    const char *pkt = s_arg(a, "packet");
    if (!pkt || strpbrk(pkt, "\r\n")) {
        p->err = "need a packet payload (without $ and checksum)";
        return;
    }
    cg_buf_printf(&p->line, "RAW %s", pkt);
}

#define NO_PROPS "{}"

static const tool_t k_tools[] = {
    {"gdb_status",
     "Hub and target state: stub link, running/stopped, last stop, whether a human gdb is "
     "attached, and what agents may currently do (policy level).",
     NO_PROPS, NULL, plan_status},
    {"gdb_halt",
     "Stop a running target (like Ctrl-C in gdb) and report where it stopped. Works while a "
     "human gdb is attached; they will see the stop too.",
     "{\"timeout_ms\":{\"type\":\"integer\"}}", NULL, plan_halt},
    {"gdb_continue",
     "Resume the target. With wait_ms, block until it stops (breakpoint, fault) or the wait "
     "expires. Usually refused while a human gdb is attached.",
     "{\"wait_ms\":{\"type\":\"integer\",\"description\":\"0 = return immediately\"}}", NULL,
     plan_continue},
    {"gdb_step", "Single-step one instruction and report the new stop.", NO_PROPS, NULL,
     plan_step},
    {"gdb_wait_stop",
     "Wait until the target stops; returns at once if it is not running.",
     "{\"timeout_ms\":{\"type\":\"integer\"}}", NULL, plan_wait},
    {"gdb_read_registers",
     "Read registers by name (from the stub's target description). Default: all registers up "
     "to 64 bits.",
     "{\"names\":{\"type\":\"string\",\"description\":\"comma list, e.g. pc,sp,a0\"},"
     "\"all\":{\"type\":\"boolean\",\"description\":\"include vector/float registers\"}}",
     NULL, plan_regs},
    {"gdb_read_memory", "Read target memory (halted target), shown as a hexdump.",
     "{\"address\":{\"type\":\"string\",\"description\":\"hex, e.g. 0x20000000\"},"
     "\"length\":{\"type\":\"integer\",\"description\":\"bytes, max 65536\"}}",
     "[\"address\",\"length\"]", plan_read_mem},
    {"gdb_write_memory", "Write bytes to target memory (needs the write level).",
     "{\"address\":{\"type\":\"string\"},"
     "\"hex\":{\"type\":\"string\",\"description\":\"bytes in memory order, e.g. deadbeef\"}}",
     "[\"address\",\"hex\"]", plan_write_mem},
    {"gdb_write_register", "Set a register by name (needs the write level).",
     "{\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"string\","
     "\"description\":\"0x... or decimal\"}}",
     "[\"name\",\"value\"]", plan_write_reg},
    {"gdb_breakpoint",
     "Set, clear or list breakpoints/watchpoints inserted by agents. type: sw (default), hw, "
     "write, read, access. kind: instruction length for sw/hw, byte length for watchpoints.",
     "{\"action\":{\"type\":\"string\",\"enum\":[\"set\",\"clear\",\"list\"]},"
     "\"address\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},"
     "\"kind\":{\"type\":\"integer\"}}",
     "[\"action\"]", plan_breakpoint},
    {"gdb_monitor",
     "Run a stub monitor command (OpenOCD: 'reset halt', 'reg', 'mdw 0x0 4'; needs write).",
     "{\"command\":{\"type\":\"string\"}}", "[\"command\"]", plan_monitor},
    {"gdb_exec",
     "Run a gdb CLI command inside the user's own gdb (its symbols and target), e.g. 'bt', "
     "'info symbol $pc', 'x/4xw 0x20000000', 'load'. Needs the user to have run "
     "'new-ui mi PATH' (see gdb_status gdb_mi_path). Reporting commands need read; anything "
     "else needs write. Fails while the target runs; halt first.",
     "{\"command\":{\"type\":\"string\"}}", "[\"command\"]", plan_exec},
    {"gdb_events",
     "Recent hub events: stops, runs, console output, gdb attach/detach, notes. types narrows "
     "it (comma list of link,client,run,stop,console,note,packet or all).",
     "{\"last\":{\"type\":\"integer\"},\"types\":{\"type\":\"string\"}}", NULL, plan_events},
    {"gdb_raw_packet",
     "Send one RSP packet payload and return the reply (needs write). For things the other "
     "tools do not cover, e.g. qXfer or vendor q packets.",
     "{\"packet\":{\"type\":\"string\"}}", "[\"packet\"]", plan_raw},
};

#define NTOOLS (sizeof(k_tools) / sizeof(k_tools[0]))

/* ---- JSON-RPC ---------------------------------------------------------- */

static void write_json(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s)
        return;
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(s);
}

static void send_result(cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(resp, "result", result);
    write_json(resp);
    cJSON_Delete(resp);
}

static void send_error(cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    cJSON *e = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    write_json(resp);
    cJSON_Delete(resp);
}

static void send_text(cJSON *id, const char *text, int is_error)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    if (is_error)
        cJSON_AddTrueToObject(result, "isError");
    send_result(id, result);
}

static void handle_initialize(cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", "2024-11-05");
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(r, "capabilities"), "tools");
    cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
    cJSON_AddStringToObject(info, "name", "cogdb");
    cJSON_AddStringToObject(info, "version", COGDB_VERSION);
    send_result(id, r);
}

static void handle_list(cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *tools = cJSON_AddArrayToObject(r, "tools");
    for (size_t i = 0; i < NTOOLS; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", k_tools[i].name);
        cJSON_AddStringToObject(t, "description", k_tools[i].desc);
        cJSON *schema = cJSON_AddObjectToObject(t, "inputSchema");
        cJSON_AddStringToObject(schema, "type", "object");
        cJSON *props = cJSON_Parse(k_tools[i].schema);
        cJSON_AddItemToObject(schema, "properties", props ? props : cJSON_CreateObject());
        if (k_tools[i].required) {
            cJSON *req = cJSON_Parse(k_tools[i].required);
            if (req)
                cJSON_AddItemToObject(schema, "required", req);
        }
        cJSON_AddItemToArray(tools, t);
    }
    send_result(id, r);
}

static char *mem_text(const char *reply)
{
    const char *nl = strchr(reply, '\n');
    const char *ap = strstr(reply, "addr=");
    if (!nl || !ap)
        return strdup(reply);
    uint64_t addr = strtoull(ap + 5, NULL, 16);
    const char *hex = nl + 1;
    size_t hl = strcspn(hex, "\n");
    uint8_t *bytes = malloc(hl / 2 + 1);
    long got = bytes ? cg_hex_decode(hex, hl, bytes, hl / 2 + 1) : -1;
    cg_buf_t out;
    cg_buf_init(&out);
    cg_buf_append(&out, reply, (size_t)(nl - reply + 1));
    if (got >= 0)
        cg_hexdump(&out, addr, bytes, (size_t)got);
    else
        cg_buf_puts(&out, hex);
    free(bytes);
    return out.data ? out.data : strdup("");
}

static void handle_call(cJSON *id, cJSON *params)
{
    cJSON *jn = params ? cJSON_GetObjectItemCaseSensitive(params, "name") : NULL;
    cJSON *args = params ? cJSON_GetObjectItemCaseSensitive(params, "arguments") : NULL;
    if (!cJSON_IsString(jn)) {
        send_error(id, -32602, "missing tool name");
        return;
    }
    const tool_t *tool = NULL;
    for (size_t i = 0; i < NTOOLS; i++)
        if (!strcmp(k_tools[i].name, jn->valuestring))
            tool = &k_tools[i];
    if (!tool) {
        send_error(id, -32601, "unknown tool");
        return;
    }
    plan_t p = {.timeout_ms = 15000};
    cg_buf_init(&p.line);
    tool->plan(args, &p);
    if (p.err) {
        send_text(id, p.err, 1);
        cg_buf_free(&p.line);
        return;
    }
    char inst[CG_NAME_MAX];
    if (cg_pick_instance(g_name, inst, sizeof(inst)) != 0) {
        send_text(id, "no cogdb hub found: start one with `cogdb serve --stub ... --name NAME` "
                      "and pass -n NAME to `cogdb mcp`", 1);
        cg_buf_free(&p.line);
        return;
    }
    cg_buf_t reply;
    cg_buf_init(&reply);
    if (cg_ctl_request(inst, p.line.data, &reply, p.timeout_ms) != 0) {
        send_text(id, "hub not reachable or did not answer in time", 1);
    } else {
        const char *r = reply.data ? reply.data : "";
        int ok = strncmp(r, "OK", 2) == 0;
        if (ok && p.hexdump) {
            char *t = mem_text(r);
            send_text(id, t, 0);
            free(t);
        } else {
            send_text(id, r, !ok);
        }
    }
    cg_buf_free(&reply);
    cg_buf_free(&p.line);
}

static void dispatch(cJSON *msg)
{
    cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (!cJSON_IsString(method)) {
        if (id)
            send_error(id, -32600, "missing method");
        return;
    }
    const char *m = method->valuestring;
    if (!strcmp(m, "initialize"))
        handle_initialize(id);
    else if (!strcmp(m, "tools/list"))
        handle_list(id);
    else if (!strcmp(m, "tools/call"))
        handle_call(id, cJSON_GetObjectItemCaseSensitive(msg, "params"));
    else if (!strcmp(m, "ping"))
        send_result(id, cJSON_CreateObject());
    else if (id && strncmp(m, "notifications/", 14) != 0)
        send_error(id, -32601, "method not found");
}

int cg_mcp_main(const char *name)
{
    g_name = name;
    cg_buf_t in;
    cg_buf_init(&in);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        cg_buf_append(&in, chunk, (size_t)n);
        for (;;) {
            char *nl = memchr(in.data, '\n', in.len);
            if (!nl)
                break;
            *nl = '\0';
            cJSON *msg = cJSON_Parse(in.data);
            if (msg) {
                dispatch(msg);
                cJSON_Delete(msg);
            }
            cg_buf_consume(&in, (size_t)(nl - in.data) + 1);
        }
        if (in.len > (1u << 22))
            cg_buf_reset(&in);
    }
    cg_buf_free(&in);
    return 0;
}
