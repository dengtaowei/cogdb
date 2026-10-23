#include "gdbcmd.h"

#include <ctype.h>
#include <string.h>

/* Read-only commands, matched on the whole first word. Frame selection
 * (frame, up, down) is left out: it changes what the human sees. */
static const char *const k_read[] = {
    "bt", "backtrace", "where", "info", "i", "x", "p", "print", "output", "ptype", "whatis",
    "list", "l", "disassemble", "show", NULL,
};

/* Host side effects or gdb self-modification. gdb accepts any unique
 * prefix, so a first word that is a prefix of one of these (two letters or
 * more) is refused too. `monitor` is here because `monitor cogdb allow`
 * would let an agent grant itself rights. */
static const char *const k_blocked[] = {
    "shell", "pipe", "make", "cd", "dump", "append", "source", "define", "document",
    "monitor", "python", "guile", "compile", "alias", "new-ui", "interpreter-exec", NULL,
};

static int in(const char *const *list, const char *w)
{
    for (int i = 0; list[i]; i++)
        if (strcmp(list[i], w) == 0)
            return 1;
    return 0;
}

static int prefix_of_blocked(const char *w)
{
    size_t k = strlen(w);
    if (k < 2)
        return 0;
    for (int i = 0; k_blocked[i]; i++)
        if (strncmp(k_blocked[i], w, k) == 0)
            return 1;
    return 0;
}

/* An expression is side-effect free if it neither assigns nor calls. */
static int pure_expr(const char *e)
{
    for (const char *p = e; *p; p++) {
        if (*p == '=') {
            char prev = p > e ? p[-1] : ' ';
            if (p[1] == '=') {
                p++;
                continue;
            }
            if (prev != '<' && prev != '>' && prev != '!')
                return 0;
        }
        if ((p[0] == '+' && p[1] == '+') || (p[0] == '-' && p[1] == '-'))
            return 0;
        if (*p == '(') {
            const char *q = p;
            while (q > e && q[-1] == ' ')
                q--;
            if (q > e && (isalnum((unsigned char)q[-1]) || q[-1] == '_' || q[-1] == ')' ||
                          q[-1] == ']'))
                return 0;
        }
    }
    return 1;
}

cg_gdbcmd_verdict_t cg_gdbcmd_classify(const char *cmd, cg_level_t *level)
{
    for (const char *p = cmd; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f)
            return CG_GDBCMD_BAD;
    while (*cmd == ' ')
        cmd++;
    if (!*cmd)
        return CG_GDBCMD_BAD;
    if (*cmd == '!' || *cmd == '|')
        return CG_GDBCMD_BLOCKED;

    char w[32];
    size_t k = 0;
    while (cmd[k] && cmd[k] != ' ' && cmd[k] != '/' && k < sizeof(w) - 1) {
        w[k] = cmd[k];
        k++;
    }
    w[k] = '\0';
    if (in(k_blocked, w) || prefix_of_blocked(w))
        return CG_GDBCMD_BLOCKED;

    *level = CG_LVL_WRITE;
    if (!in(k_read, w))
        return CG_GDBCMD_OK;
    const char *rest = cmd + k;
    int is_expr = !strcmp(w, "p") || !strcmp(w, "print") || !strcmp(w, "output") ||
                  !strcmp(w, "x");
    if (!is_expr || pure_expr(rest))
        *level = CG_LVL_READ;
    return CG_GDBCMD_OK;
}
