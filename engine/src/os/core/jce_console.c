/*
 * jce_console.c -- cvar + command registry / dispatch (see jce_console.h).
 *
 * Process-global, single-thread.  cvars are individually heap-allocated and
 * referenced by a growable pointer table so a returned JceCvar* stays valid
 * for the process lifetime (table growth never moves the cvars themselves).
 */

#include <jce/os/core/jce_console.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "console"

struct JceCvar {
    char        name[64];
    char        help[128];
    JceCvarType type;
    uint32_t    flags;
    bool        b;
    int         i;
    float       f;
    char        s[256];   /* STRING value (also a scratch for formatting) */
};

typedef struct {
    char            name[64];
    char            help[128];
    JceConsoleCmdFn fn;
    void           *user;
} ConsoleCmd;

static JceCvar         **s_cvars;
static int               s_cvar_count, s_cvar_cap;
static ConsoleCmd       *s_cmds;
static int               s_cmd_count, s_cmd_cap;
static JceConsoleOutputFn s_out_fn;
static void             *s_out_user;

/* ── Output ────────────────────────────────────────────────────────────── */

void jce_console_set_output(JceConsoleOutputFn fn, void *user)
{
    s_out_fn = fn;
    s_out_user = user;
}

void jce_console_print(const char *text)
{
    if (!text) return;
    if (s_out_fn) s_out_fn(text, s_out_user);
    else          LOG_INFO(LOG_TAG, "%s", text);
}

static void console_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    jce_console_print(buf);
}

/* ── Small helpers ─────────────────────────────────────────────────────── */

static void copy_str(char *dst, int cap, const char *src)
{
    if (cap <= 0) return;
    int n = 0;
    if (src) { while (src[n] && n < cap - 1) { dst[n] = src[n]; ++n; } }
    dst[n] = '\0';
}

/* ── cvar registry ─────────────────────────────────────────────────────── */

static JceCvar *cvar_alloc(const char *name, JceCvarType type,
                           uint32_t flags, const char *help)
{
    if (s_cvar_count >= s_cvar_cap) {
        int nc = s_cvar_cap ? s_cvar_cap * 2 : 32;
        JceCvar **np = (JceCvar **)jce_realloc(s_cvars, (size_t)nc * sizeof(*np));
        if (!np) return NULL;
        s_cvars = np;
        s_cvar_cap = nc;
    }
    JceCvar *cv = (JceCvar *)jce_malloc(sizeof(*cv));
    if (!cv) return NULL;
    memset(cv, 0, sizeof *cv);
    copy_str(cv->name, sizeof cv->name, name);
    copy_str(cv->help, sizeof cv->help, help);
    cv->type = type;
    cv->flags = flags;
    s_cvars[s_cvar_count++] = cv;
    return cv;
}

JceCvar *jce_cvar_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_cvar_count; ++i)
        if (strcmp(s_cvars[i]->name, name) == 0) return s_cvars[i];
    return NULL;
}

/* Register or return existing (type must match on re-registration). */
static JceCvar *cvar_register(const char *name, JceCvarType type,
                              uint32_t flags, const char *help)
{
    if (!name || !name[0]) return NULL;
    JceCvar *ex = jce_cvar_find(name);
    if (ex) return (ex->type == type) ? ex : NULL;
    return cvar_alloc(name, type, flags, help);
}

/* The register_* wrappers set the default ONLY when the cvar is newly created
 * (cvar_register returns the existing one otherwise, leaving its value
 * untouched — the first registration wins). */
static bool s_just_created;
static JceCvar *cvar_register_tracked(const char *name, JceCvarType type,
                                      uint32_t flags, const char *help)
{
    s_just_created = (jce_cvar_find(name) == NULL);
    return cvar_register(name, type, flags, help);
}

JceCvar *jce_cvar_register_bool(const char *name, bool def, uint32_t flags, const char *help)
{
    JceCvar *cv = cvar_register_tracked(name, JCE_CVAR_BOOL, flags, help);
    if (cv && s_just_created) cv->b = def;
    return cv;
}

JceCvar *jce_cvar_register_int(const char *name, int def, uint32_t flags, const char *help)
{
    JceCvar *cv = cvar_register_tracked(name, JCE_CVAR_INT, flags, help);
    if (cv && s_just_created) cv->i = def;
    return cv;
}

JceCvar *jce_cvar_register_float(const char *name, float def, uint32_t flags, const char *help)
{
    JceCvar *cv = cvar_register_tracked(name, JCE_CVAR_FLOAT, flags, help);
    if (cv && s_just_created) cv->f = def;
    return cv;
}

JceCvar *jce_cvar_register_string(const char *name, const char *def, uint32_t flags, const char *help)
{
    JceCvar *cv = cvar_register_tracked(name, JCE_CVAR_STRING, flags, help);
    if (cv && s_just_created) copy_str(cv->s, sizeof cv->s, def ? def : "");
    return cv;
}

JceCvarType jce_cvar_type(const JceCvar *cv)  { return cv ? cv->type : JCE_CVAR_INT; }
const char *jce_cvar_name(const JceCvar *cv)  { return cv ? cv->name : ""; }
const char *jce_cvar_help(const JceCvar *cv)  { return cv ? cv->help : ""; }
uint32_t    jce_cvar_flags(const JceCvar *cv) { return cv ? cv->flags : 0; }

bool jce_cvar_get_bool(const JceCvar *cv)
{
    if (!cv) return false;
    switch (cv->type) {
        case JCE_CVAR_BOOL:   return cv->b;
        case JCE_CVAR_INT:    return cv->i != 0;
        case JCE_CVAR_FLOAT:  return cv->f != 0.0f;
        case JCE_CVAR_STRING: return cv->s[0] && strcmp(cv->s, "0") != 0;
    }
    return false;
}

int jce_cvar_get_int(const JceCvar *cv)
{
    if (!cv) return 0;
    switch (cv->type) {
        case JCE_CVAR_BOOL:   return cv->b ? 1 : 0;
        case JCE_CVAR_INT:    return cv->i;
        case JCE_CVAR_FLOAT:  return (int)cv->f;
        case JCE_CVAR_STRING: return (int)strtol(cv->s, NULL, 10);
    }
    return 0;
}

float jce_cvar_get_float(const JceCvar *cv)
{
    if (!cv) return 0.0f;
    switch (cv->type) {
        case JCE_CVAR_BOOL:   return cv->b ? 1.0f : 0.0f;
        case JCE_CVAR_INT:    return (float)cv->i;
        case JCE_CVAR_FLOAT:  return cv->f;
        case JCE_CVAR_STRING: return (float)strtod(cv->s, NULL);
    }
    return 0.0f;
}

const char *jce_cvar_get_string(const JceCvar *cv)
{
    return (cv && cv->type == JCE_CVAR_STRING) ? cv->s : "";
}

void jce_cvar_set_bool (JceCvar *cv, bool  v) { if (cv) cv->b = v; }
void jce_cvar_set_int  (JceCvar *cv, int   v) { if (cv) cv->i = v; }
void jce_cvar_set_float(JceCvar *cv, float v) { if (cv) cv->f = v; }
void jce_cvar_set_string(JceCvar *cv, const char *v)
{
    if (cv) copy_str(cv->s, sizeof cv->s, v ? v : "");
}

static bool parse_bool(const char *s, bool *out)
{
    if (!s) return false;
    if (!strcmp(s,"1")||!strcmp(s,"true")||!strcmp(s,"on") ||!strcmp(s,"yes")) { *out = true;  return true; }
    if (!strcmp(s,"0")||!strcmp(s,"false")||!strcmp(s,"off")||!strcmp(s,"no")) { *out = false; return true; }
    return false;
}

bool jce_cvar_set_from_string(const char *name, const char *value)
{
    JceCvar *cv = jce_cvar_find(name);
    if (!cv || !value) return false;
    if (cv->flags & JCE_CVAR_FLAG_READONLY) return false;
    switch (cv->type) {
        case JCE_CVAR_BOOL: {
            bool b;
            if (!parse_bool(value, &b)) return false;
            cv->b = b;
            return true;
        }
        case JCE_CVAR_INT: {
            char *end = NULL;
            long v = strtol(value, &end, 0);
            if (end == value) return false;
            cv->i = (int)v;
            return true;
        }
        case JCE_CVAR_FLOAT: {
            char *end = NULL;
            double v = strtod(value, &end);
            if (end == value) return false;
            cv->f = (float)v;
            return true;
        }
        case JCE_CVAR_STRING:
            copy_str(cv->s, sizeof cv->s, value);
            return true;
    }
    return false;
}

int jce_cvar_format_value(const JceCvar *cv, char *out, int cap)
{
    if (!cv || !out || cap <= 0) { if (out && cap > 0) out[0] = '\0'; return 0; }
    int n = 0;
    switch (cv->type) {
        case JCE_CVAR_BOOL:   n = snprintf(out, (size_t)cap, "%s", cv->b ? "true" : "false"); break;
        case JCE_CVAR_INT:    n = snprintf(out, (size_t)cap, "%d", cv->i); break;
        case JCE_CVAR_FLOAT:  n = snprintf(out, (size_t)cap, "%g", (double)cv->f); break;
        case JCE_CVAR_STRING: n = snprintf(out, (size_t)cap, "%s", cv->s); break;
    }
    if (n < 0) { out[0] = '\0'; return 0; }
    if (n >= cap) n = cap - 1;
    return n;
}

int      jce_cvar_count(void)        { return s_cvar_count; }
JceCvar *jce_cvar_at(int index)
{
    return (index >= 0 && index < s_cvar_count) ? s_cvars[index] : NULL;
}

/* ── Commands ──────────────────────────────────────────────────────────── */

static ConsoleCmd *cmd_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < s_cmd_count; ++i)
        if (strcmp(s_cmds[i].name, name) == 0) return &s_cmds[i];
    return NULL;
}

bool jce_console_register_cmd(const char *name, JceConsoleCmdFn fn,
                              void *user, const char *help)
{
    if (!name || !name[0] || !fn) return false;
    ConsoleCmd *ex = cmd_find(name);
    if (ex) { ex->fn = fn; ex->user = user; copy_str(ex->help, sizeof ex->help, help); return true; }
    if (s_cmd_count >= s_cmd_cap) {
        int nc = s_cmd_cap ? s_cmd_cap * 2 : 16;
        ConsoleCmd *np = (ConsoleCmd *)jce_realloc(s_cmds, (size_t)nc * sizeof(*np));
        if (!np) return false;
        s_cmds = np;
        s_cmd_cap = nc;
    }
    ConsoleCmd *c = &s_cmds[s_cmd_count++];
    memset(c, 0, sizeof *c);
    copy_str(c->name, sizeof c->name, name);
    copy_str(c->help, sizeof c->help, help);
    c->fn = fn;
    c->user = user;
    return true;
}

/* ── Exec ──────────────────────────────────────────────────────────────── */

#define CONSOLE_MAX_ARGV 32

/* Tokenize `line` (in place into `buf`) into argv; supports "quoted" tokens. */
static int tokenize(char *buf, const char **argv, int max_argv)
{
    int argc = 0;
    char *p = buf;
    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') {
            ++p;
            argv[argc++] = p;
            while (*p && *p != '"') ++p;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

bool jce_console_exec(const char *line)
{
    if (!line) return false;
    char buf[1024];
    copy_str(buf, sizeof buf, line);

    const char *argv[CONSOLE_MAX_ARGV];
    int argc = tokenize(buf, argv, CONSOLE_MAX_ARGV);
    if (argc == 0) return false;

    /* Built-in: help / list cvars + commands. */
    if (!strcmp(argv[0], "help") || !strcmp(argv[0], "list")) {
        console_printf("cvars (%d):", s_cvar_count);
        for (int i = 0; i < s_cvar_count; ++i) {
            char val[256];
            jce_cvar_format_value(s_cvars[i], val, sizeof val);
            console_printf("  %s = %s    %s", s_cvars[i]->name, val, s_cvars[i]->help);
        }
        console_printf("commands (%d):", s_cmd_count);
        for (int i = 0; i < s_cmd_count; ++i)
            console_printf("  %s    %s", s_cmds[i].name, s_cmds[i].help);
        return true;
    }

    ConsoleCmd *cmd = cmd_find(argv[0]);
    if (cmd) {
        cmd->fn(argc, argv, cmd->user);
        return true;
    }

    JceCvar *cv = jce_cvar_find(argv[0]);
    if (cv) {
        if (argc >= 2) {
            if (!jce_cvar_set_from_string(argv[0], argv[1]))
                console_printf("cannot set '%s' to '%s'", argv[0], argv[1]);
        } else {
            char val[256];
            jce_cvar_format_value(cv, val, sizeof val);
            console_printf("%s = %s", argv[0], val);
        }
        return true;
    }

    console_printf("unknown command or cvar: %s", argv[0]);
    return false;
}

void jce_console_shutdown(void)
{
    for (int i = 0; i < s_cvar_count; ++i) jce_free(s_cvars[i]);
    jce_free(s_cvars);
    s_cvars = NULL;
    s_cvar_count = s_cvar_cap = 0;
    jce_free(s_cmds);
    s_cmds = NULL;
    s_cmd_count = s_cmd_cap = 0;
    s_out_fn = NULL;
    s_out_user = NULL;
}
