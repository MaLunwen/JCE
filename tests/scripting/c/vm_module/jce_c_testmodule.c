/*
 * jce_c_testmodule.c — a script module written in C, and compiled as C.
 *
 * THIS FILE IS THE SUBJECT, not scaffolding.  The claim scripting/c makes is
 * that a PURE C TRANSLATION UNIT can be an entity script: no C++ compiler, no
 * templates, no exceptions, no thunk generator.  A test that proved it with a
 * .cpp file would prove nothing at all, so tests/scripting/c/CMakeLists.txt
 * asserts LINKER_LANGUAGE C on both targets built from it, and this file uses
 * only <jce/script_vm/jce_script_vm_c.h> plus the C standard library.
 *
 * TWO TARGETS, ONE SOURCE, and the difference is one -D:
 *
 *   linked into the test executable  — reached through the ACCESSOR that
 *       JCE_C_MODULE_END emits, handed to jce_script_vm_cpp_add_module().
 *       This is the shape a shipped game uses: one binary, no plugin to find.
 *   built as a MODULE library        — reached through the EXPORTED entry
 *       symbol, loaded by jce_script_vm_cpp_load_library() from an absolute
 *       path.  This is the shape editor Play uses, and it is the only one
 *       that can fail the way a plugin fails: a statically linked stand-in
 *       keeps its code mapped whatever the registry does.
 *
 * The two carry DIFFERENT module and class names because the registry is
 * process-wide and refuses a duplicate of either — which is itself part of
 * what the suite pins (one registry, shared across both languages).
 *
 * HOW THE TEST SEES ANYTHING.  A script class has no return channel: the
 * engine calls it and discards everything but the status.  So the class
 * reports through JceScriptHost::log, the same sink the engine's own
 * diagnostics use, which the test installs a recording mock for.  A REAL C
 * script must not do this — `log` is one of the seven entries the shipped C
 * ABI deliberately does not export, and a script reaches the engine through
 * <jce/script_api/jce_script_api.h> instead.  Here the raw host is exactly
 * what is under test: that ctx->host and ctx->host_size arrive intact.
 */
#include <jce/script_vm/jce_script_vm_c.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(JCE_C_TESTMODULE_DLL)
#  define MODULE_NAME "c_testmodule_dll"
#  define CLASS_NAME  "CDllSpin"
#  define TAG         "dll"
#else
#  define MODULE_NAME "c_testmodule"
#  define CLASS_NAME  "CTick"
#  define TAG         "static"
#endif

typedef struct Ticker {
    const JceScriptHost *host;
    JceScriptEntity      entity;
    int                  updates;
    float                elapsed;
    char                 err[128];
} Ticker;

/* The host may be SHORT — the engine copies min(creator's host_size, its own
 * sizeof) over a zeroed table and forwards that clamped size.  Reading `log`
 * without checking the size is the over-read the whole short-host ABI exists
 * to prevent, so check it once, here, and store the result. */
static int host_reaches_log(const JceCScriptContext *ctx)
{
    const size_t need = offsetof(JceScriptHost, log) + sizeof(void (*)(void));
    if (!ctx || !ctx->host) return 0;
    if (ctx->host_size < need) return 0;
    return ctx->host->log != NULL;
}

static void say(Ticker *t, const char *what)
{
    char buf[160];
    if (!t->host || !t->host->log) return;
    snprintf(buf, sizeof buf, "%s:%s:%s", TAG, CLASS_NAME, what);
    t->host->log(t->host->user, buf);
}

static void *ticker_create(const JceCScriptContext *ctx)
{
    Ticker *t;

    /* struct_size FIRST, before any other member is touched: this struct was
     * allocated by the ENGINE and read by this module, so a module built
     * against a newer header would read past its end. */
    if (!ctx || ctx->struct_size < sizeof(JceCScriptContext)) return NULL;

    t = (Ticker *)calloc(1, sizeof *t);
    if (!t) return NULL;
    t->host   = host_reaches_log(ctx) ? ctx->host : NULL;
    t->entity = ctx->entity;
    say(t, "create");
    return t;
}

static void ticker_destroy(void *self)
{
    Ticker *t = (Ticker *)self;
    if (!t) return;
    say(t, "destroy");
    free(t);
}

static JceCStatus ticker_start(void *self)
{
    say((Ticker *)self, "on_start");
    return JCE_C_OK;
}

static JceCStatus ticker_update(void *self, float dt)
{
    Ticker *t = (Ticker *)self;
    char    buf[64];
    ++t->updates;
    t->elapsed += dt;
    snprintf(buf, sizeof buf, "on_update#%d", t->updates);
    say(t, buf);
    return JCE_C_OK;
}

static JceCStatus ticker_on_destroy(void *self)
{
    say((Ticker *)self, "on_destroy");
    return JCE_C_OK;
}

/* on_message is the one handler that deliberately RETURNS AN ERROR, so the
 * suite can pin that a C class reports failure the same way a C++ class does
 * — the status string, not an exception — and that THE FAILING-CALLBACK RULE
 * does not fire for on_message (jce_script_vm_cpp.c passes CB_NONE there, so
 * the message is logged and nothing is disabled). */
static JceCStatus ticker_message(void *self, const char *name,
                                 double number_arg, const char *str_arg)
{
    Ticker *t = (Ticker *)self;
    (void)number_arg;
    (void)str_arg;
    say(t, name ? name : "(null)");
    if (name && strcmp(name, "boom") == 0) {
        /* A per-instance buffer, not a local: the pointer must stay valid
         * until the next call into this instance. */
        snprintf(t->err, sizeof t->err, "C said no");
        return t->err;
    }
    return JCE_C_OK;
}

/* NOTE WHAT IS NOT HERE: no on_collision and no on_anim_event.  An omitted
 * handler is an omitted LINE, not a NULL to count out — which is the whole
 * reason the optional slots are named rather than positional.  A class that
 * declines a handler is the normal case. */
JCE_C_SCRIPT_CLASS_BEGIN(Ticker, CLASS_NAME, ticker_create, ticker_destroy)
    JCE_C_ON_UPDATE(ticker_update)
    JCE_C_ON_START(ticker_start)
    JCE_C_ON_DESTROY(ticker_on_destroy)
    JCE_C_ON_MESSAGE(ticker_message)
JCE_C_SCRIPT_CLASS_END()

JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(Ticker)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END(MODULE_NAME, jce_c_testmodule)
