/*
 * jce_c_testmodule_two.c — a SECOND native script module, statically linked
 * into the same executable as the first.
 *
 * ── WHAT THIS FILE EXISTS TO MAKE POSSIBLE ───────────────────────────────
 *
 * Its content is deliberately trivial.  The thing under test is that it can
 * be in the binary AT ALL.
 *
 * JCE_C_MODULE_END and JCE_CPP_MODULE_END both emit the SHARED-OBJECT entry
 * point `jce_cpp_script_module` — one external symbol, because one loader
 * looks up one name — and they emitted it unconditionally.  So two native
 * script modules statically linked into one binary did not link:
 *
 *     es_prop_surface.c.obj : error LNK2005: jce_cpp_script_module already
 *                             defined in es_flower_sway.cpp.obj
 *     elemental_serenity.exe : fatal error LNK1169
 *
 * measured on elemental_serenity the first time it compiled a C script beside
 * its C++ one.  That is not an exotic arrangement — it is exactly what
 * jce_script_vm_c.h asks a project with both languages to do ("a project that
 * wants both publishes TWO modules — two add_module() calls"), and a shipped
 * game prefers static modules precisely because there is then no plugin to
 * find.  The documented shape did not build.
 *
 * The fix is JCE_SCRIPT_MODULE_NO_ENTRY, defined on the compile that goes
 * INTO a binary, where the exported entry is dead code: a statically linked
 * host reaches the module through the accessor and
 * jce_script_vm_cpp_add_module(), and nothing dlsym()s a symbol out of its own
 * process image.  tests/scripting/c/CMakeLists.txt defines it for the test
 * EXECUTABLE and deliberately does NOT define it for the MODULE library
 * target, where the entry is the only door.
 *
 * SO THIS FILE IS THE MUTATION TARGET, and the mutation is a BUILD failure
 * rather than a red assertion: drop JCE_SCRIPT_MODULE_NO_ENTRY from the test
 * target and this translation unit stops linking.  A test that only ran would
 * not have caught the defect, because the defect is that the program could not
 * be produced.
 *
 * TWO MODULES, TWO NAMES.  The registry is process-wide and refuses a
 * duplicate module name OR class name, so nothing here may be spelled the way
 * jce_c_testmodule.c spells it.  That refusal is itself part of the design
 * (one registry serves both languages) and the suite pins it elsewhere.
 */
#include <jce/script_vm/jce_script_vm_c.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TwoState {
    const JceScriptHost *host;
    int                  updates;
} TwoState;

static void two_say(TwoState *t, const char *what)
{
    char buf[96];
    if (!t || !t->host || !t->host->log) return;
    snprintf(buf, sizeof buf, "second:CTwo:%s", what);
    t->host->log(t->host->user, buf);
}

static void *two_create(const JceCScriptContext *ctx)
{
    TwoState  *t;
    const size_t need = offsetof(JceScriptHost, log) + sizeof(void (*)(void));

    if (!ctx || ctx->struct_size < sizeof(JceCScriptContext)) return NULL;
    t = (TwoState *)calloc(1, sizeof *t);
    if (!t) return NULL;
    /* Same short-host check the first module makes: the engine forwards a
     * host CLAMPED to min(creator's size, its own), so `log` may not be
     * there. */
    if (ctx->host && ctx->host_size >= need && ctx->host->log)
        t->host = ctx->host;
    two_say(t, "create");
    return t;
}

static void two_destroy(void *self)
{
    TwoState *t = (TwoState *)self;
    if (!t) return;
    two_say(t, "destroy");
    free(t);
}

static JceCStatus two_start(void *self)
{
    two_say((TwoState *)self, "on_start");
    return JCE_C_OK;
}

static JceCStatus two_update(void *self, float dt)
{
    TwoState *t = (TwoState *)self;
    char      buf[64];
    (void)dt;
    ++t->updates;
    snprintf(buf, sizeof buf, "on_update#%d", t->updates);
    two_say(t, buf);
    return JCE_C_OK;
}

JCE_C_SCRIPT_CLASS_BEGIN(CTwo, "CTwo", two_create, two_destroy)
    JCE_C_ON_START(two_start)
    JCE_C_ON_UPDATE(two_update)
JCE_C_SCRIPT_CLASS_END()

JCE_C_MODULE_BEGIN()
    JCE_C_MODULE_CLASS(CTwo)
JCE_C_MODULE_GLOBALS()
JCE_C_MODULE_END("c_testmodule_two", jce_c_testmodule_two)
