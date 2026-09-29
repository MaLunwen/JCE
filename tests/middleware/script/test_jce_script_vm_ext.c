/*
 * test_jce_script_vm_ext.c — the extension registry: which VM a script PATH
 * belongs to, and the refusals that keep that answer from being a guess.
 *
 * The registry exists so `bob.lua` and `turret.py` can sit in one scene.  The
 * mapping is NOT a table in an engine file — a backend claims its own
 * extension from inside its own register(), the same way it claims its
 * language name — so a sixth backend needs no engine edit.  That property is
 * only real if the claim path is a public call with public refusals, which is
 * what this file pins.
 *
 * WHAT THIS FILE FORBIDS, precisely:
 *
 *   1. A claim for a language that is NOT REGISTERED.  Accepting one would
 *      make ".py -> python" resolve in a build with no Python backend, and
 *      the failure would surface as "the VM for python could not be created"
 *      at the first entity instead of "nothing claims .py" — the harder of
 *      the two to diagnose.  Refusing the claim is what keeps the language
 *      list a sound test for "is this backend in this executable".
 *
 *   2. A SECOND claim on an extension already claimed.  A live script may
 *      already be running in the first claimant's VM; silently repointing
 *      ".lua" at another language mid-process would leave those instances
 *      being dispatched into a VM that never issued them.
 *
 *   3. A claim that could never match: an interior dot ("tar.gz"), a path
 *      separator, an over-long extension.  language_for_path() matches the
 *      text after the LAST dot of the LAST path component, so those claims
 *      are unreachable, and an unreachable claim that reports success is the
 *      exact shape of failure this whole subsystem is built to refuse.
 *
 *   4. Silently selecting a registered language that claimed nothing.  A
 *      backend that forgot to claim must be unreachable by path, not "close
 *      enough by name".
 *
 *      NOTE THE CPP BACKEND IS NO LONGER AN EXAMPLE OF THE BENIGN CASE.  It
 *      used to be cited here as a backend legitimately claiming nothing
 *      because its "path" is a class name — and it was refused before any VM
 *      was asked, in a real four-language scene, for exactly that reason.  It
 *      claims ".jcecpp" now and strips it when resolving the class.  What
 *      stays true, and is what the cases below actually test, is that a
 *      path with NO extension resolves to nothing at all.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script_vm.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- a minimal, fully populated probe VM ----------------------------- */

typedef struct ExtProbeScript {
    JceScriptVMHeader hdr;
} ExtProbeScript;

static ExtProbeScript     g_probe_handle;
static const JceScriptVM *g_probe_self;

static JceScript *ep_create_sized(const JceScriptHost *h, size_t n)
{
    (void)h; (void)n;
    memset(&g_probe_handle, 0, sizeof g_probe_handle);
    g_probe_handle.hdr.vm = g_probe_self;
    return (JceScript *)&g_probe_handle;
}
static void ep_destroy(JceScript *s) { (void)s; }
static JceScriptInstance ep_instantiate(JceScript *s, const char *p,
                                        JceScriptEntity o)
{ (void)s; (void)p; (void)o; return 1u; }
static JceScriptInstance ep_instantiate_source(JceScript *s, const char *n,
                                               const char *src,
                                               JceScriptEntity o)
{ (void)s; (void)n; (void)src; (void)o; return 1u; }
static void ep_call_start(JceScript *s, JceScriptInstance i) { (void)s; (void)i; }
static void ep_call_update(JceScript *s, JceScriptInstance i, float dt)
{ (void)s; (void)i; (void)dt; }
static void ep_release(JceScript *s, JceScriptInstance i) { (void)s; (void)i; }
static void ep_call_collision(JceScript *s, JceScriptInstance i,
                              JceScriptEntity o)
{ (void)s; (void)i; (void)o; }
static void ep_call_message(JceScript *s, JceScriptInstance i, const char *m,
                            double n, const char *str)
{ (void)s; (void)i; (void)m; (void)n; (void)str; }
static void ep_call_anim_event(JceScript *s, JceScriptInstance i, uint32_t id,
                               const char *n, float f0, float f1, int i0)
{ (void)s; (void)i; (void)id; (void)n; (void)f0; (void)f1; (void)i0; }
static bool ep_call_named(JceScript *s, const char *f, JceScriptEntity e)
{ (void)s; (void)f; (void)e; return false; }
static bool ep_call_named_num(JceScript *s, const char *f, JceScriptEntity e,
                              double v)
{ (void)s; (void)f; (void)e; (void)v; return false; }
static bool ep_call_named_str(JceScript *s, const char *f, JceScriptEntity e,
                              const char *str)
{ (void)s; (void)f; (void)e; (void)str; return false; }
static int  ep_instance_count(const JceScript *s) { (void)s; return 0; }
static void ep_update_coroutines(JceScript *s, float dt) { (void)s; (void)dt; }
static JceScriptModule ep_compile_module(JceScript *s, const char *n,
                                         const char *src, size_t len)
{ (void)s; (void)n; (void)src; (void)len; return 0u; }
static void ep_rebind_instance(JceScript *s, JceScriptInstance i,
                               JceScriptModule m)
{ (void)s; (void)i; (void)m; }
/* An explicit no-op, which is what jce_script_vm_register demands of a
   slot a runtime does not implement: every slot must be non-NULL or the
   whole table is refused.  This probe is about extension claiming, not
   about lifecycle, so the body is deliberately empty. */
static void ep_call_fixed_update(JceScript *s, JceScriptInstance i,
                                 float dt)
{ (void)s; (void)i; (void)dt; }

static void ep_release_module(JceScript *s, JceScriptModule m)
{ (void)s; (void)m; }

static void ep_fill(JceScriptVM *vm, const char *language)
{
    memset(vm, 0, sizeof *vm);
    vm->struct_size        = sizeof(JceScriptVM);
    vm->language           = language;
    vm->create_sized       = ep_create_sized;
    vm->destroy            = ep_destroy;
    vm->instantiate        = ep_instantiate;
    vm->instantiate_source = ep_instantiate_source;
    vm->call_start         = ep_call_start;
    vm->call_update        = ep_call_update;
    vm->release            = ep_release;
    vm->call_collision     = ep_call_collision;
    vm->call_message       = ep_call_message;
    vm->call_anim_event    = ep_call_anim_event;
    vm->call_named         = ep_call_named;
    vm->call_named_num     = ep_call_named_num;
    vm->call_named_str     = ep_call_named_str;
    vm->instance_count     = ep_instance_count;
    vm->update_coroutines  = ep_update_coroutines;
    vm->compile_module     = ep_compile_module;
    vm->rebind_instance    = ep_rebind_instance;
    vm->release_module     = ep_release_module;
    vm->call_fixed_update  = ep_call_fixed_update;
}

/* The registry is process-global and never unregisters (live handles hold
 * pointers into it), so each probe language is registered exactly once, here,
 * and every test below reuses them.  `extlang_b` deliberately claims NOTHING. */
static JceScriptVM g_vm_a;   /* "extlang_a", claims .extA / .EXT2 */
static JceScriptVM g_vm_b;   /* "extlang_b", claims nothing       */
static bool        g_probes_registered;

static void register_probes_once(void)
{
    if (g_probes_registered) return;
    g_probes_registered = true;
    ep_fill(&g_vm_a, "extlang_a");
    ep_fill(&g_vm_b, "extlang_b");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_register(&g_vm_a),
        "the probe VM 'extlang_a' was refused; every later assertion in this "
        "file would then be testing the absence of a registration rather than "
        "the extension registry");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_register(&g_vm_b),
        "the probe VM 'extlang_b' was refused");
}

void setUp(void)    { register_probes_once(); }
void tearDown(void) {}

/* ------------------------------------------------------------------ *
 *  The built-in claim.  A stock build resolves .lua with no wiring.
 * ------------------------------------------------------------------ */
static void test_lua_claims_its_own_extension(void)
{
    const char *lang = jce_script_vm_language_for_path("assets/scripts/bob.lua");
    TEST_ASSERT_NOT_NULL_MESSAGE(lang,
        "nothing claims '.lua' — the built-in registers its own claim in "
        "vm_register_builtin_once, so a stock build resolves .lua with no "
        "application wiring at all");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(JCE_SCRIPT_VM_DEFAULT_LANGUAGE, lang,
        "'.lua' resolved to something other than the built-in language");
}

/* ------------------------------------------------------------------ *
 *  A claim resolves, with or without the dot, in any case, and only
 *  from the LAST component's LAST dot.
 * ------------------------------------------------------------------ */
static void test_a_claim_resolves_a_path(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_register_extension("extA", "extlang_a"),
        "a first claim for '.extA' by a registered language was refused");

    TEST_ASSERT_EQUAL_STRING_MESSAGE("extlang_a",
        jce_script_vm_language_for_path("turret.exta"),
        "the claim did not resolve a plain path");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("extlang_a",
        jce_script_vm_language_for_path("a/b/Turret.EXTA"),
        "extension matching is not case-insensitive — a scene authored with "
        "Turret.EXTA would silently run nothing");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("extlang_a",
        jce_script_vm_language_for_path("dir.v2\\turret.exta"),
        "a Windows separator or a dotted DIRECTORY broke resolution");

    /* The leading dot is optional on the claim side and must produce the
     * SAME claim — proven by the duplicate refusal, not by asserting it
     * twice: a second form that produced a second entry would be accepted. */
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension(".extA", "extlang_a"),
        "'.extA' and 'extA' were treated as two different claims");
}

static void test_a_path_with_no_extension_resolves_to_nothing(void)
{
    /* A bare class name is what a cpp scriptPath looked like before the
     * backend claimed an extension, and it must still resolve to NULL rather
     * than to whatever claimed the last thing that looked like one. */
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("EsGpuTask"),
        "an extensionless path resolved to a language");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("scripts.v2/bob"),
        "a DIRECTORY's dot was read as the file's extension");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("bob."),
        "a trailing dot with no extension resolved to a language");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path(""),
        "the empty path resolved to a language");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path(NULL),
        "a NULL path resolved to a language");
}

/* ------------------------------------------------------------------ *
 *  (4) A registered language that claimed nothing is unreachable by
 *      path — including by a path that spells its own name.
 * ------------------------------------------------------------------ */
static void
test_registered_language_without_a_claim_is_not_selected_by_path(void)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(jce_script_vm_find("extlang_b"),
        "the precondition failed: 'extlang_b' is not registered, so this test "
        "would pass for the wrong reason");
    TEST_ASSERT_NULL_MESSAGE(
        jce_script_vm_language_for_path("thing.extlang_b"),
        "a registered language was selected by a path that merely SPELLS its "
        "name — selection must come from an explicit claim, or a backend that "
        "forgot to claim would appear to work until someone renamed a file");
}

/* ------------------------------------------------------------------ *
 *  (1) A claim for an unregistered language is refused.
 * ------------------------------------------------------------------ */
static void test_claim_for_an_unregistered_language_is_refused(void)
{
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_find("extlang_never"),
        "the precondition failed: 'extlang_never' is registered");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("nvr", "extlang_never"),
        "a claim naming an UNREGISTERED language was accepted — '.nvr' would "
        "then resolve in a build that cannot run it, turning 'backend not "
        "linked' into 'the VM for extlang_never could not be created'");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("x.nvr"),
        "the refused claim was recorded anyway");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("nvr2", NULL),
        "a claim with a NULL language was accepted");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("nvr2", ""),
        "a claim with an empty language was accepted");
}

/* ------------------------------------------------------------------ *
 *  (2) A second claim on the same extension is refused — even by the
 *      language that already holds it.
 * ------------------------------------------------------------------ */
static void test_duplicate_extension_is_refused(void)
{
    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_register_extension("dup1", "extlang_a"),
        "the first claim for '.dup1' was refused");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("dup1", "extlang_b"),
        "a SECOND language took over '.dup1' — live instances issued by the "
        "first claimant's VM would then be dispatched into the second's");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("extlang_a",
        jce_script_vm_language_for_path("x.dup1"),
        "the refused second claim changed the resolution anyway");

    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("lua", "extlang_a"),
        "'.lua' was taken away from the built-in");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(JCE_SCRIPT_VM_DEFAULT_LANGUAGE,
        jce_script_vm_language_for_path("bob.lua"),
        "'.lua' no longer resolves to the built-in");
}

/* ------------------------------------------------------------------ *
 *  (3) Claims that could never match are refused at claim time.
 * ------------------------------------------------------------------ */
static void test_unmatchable_claims_are_refused(void)
{
    char toolong[JCE_SCRIPT_VM_EXTENSION_MAX + 4];
    size_t i;

    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("tar.gz", "extlang_a"),
        "an interior dot was accepted — language_for_path matches only the "
        "text after the LAST dot, so '.tar.gz' could never resolve and the "
        "claim would report success while doing nothing");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("a/b", "extlang_a"),
        "a path separator was accepted as an extension");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("a\\b", "extlang_a"),
        "a backslash was accepted as an extension");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension("", "extlang_a"),
        "the empty extension was accepted");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension(".", "extlang_a"),
        "a bare dot was accepted as an extension");
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension(NULL, "extlang_a"),
        "a NULL extension was accepted");

    for (i = 0; i < sizeof toolong - 1; ++i) toolong[i] = 'x';
    toolong[sizeof toolong - 1] = '\0';
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register_extension(toolong, "extlang_a"),
        "an extension longer than JCE_SCRIPT_VM_EXTENSION_MAX was accepted; "
        "it would be stored truncated and resolve the wrong paths");
}

/* ------------------------------------------------------------------ *
 *  The enumeration a diagnostic prints.  This is the evidence that
 *  separates "unknown language" from "backend not linked", so it has
 *  to be readable and it has to be consistent with the resolver.
 * ------------------------------------------------------------------ */
static void test_enumeration_agrees_with_the_resolver(void)
{
    int n = jce_script_vm_extension_count();
    int i;
    int seen_lua = 0;

    TEST_ASSERT_TRUE_MESSAGE(n > 0,
        "the extension registry enumerates nothing, so the 'claimed "
        "extensions' half of the no-VM-for-this-path diagnostic would print "
        "<none> in a build where .lua plainly works");

    for (i = 0; i < n; ++i) {
        const char *ext  = jce_script_vm_extension_at(i);
        const char *lang = jce_script_vm_extension_language_at(i);
        char        path[128];

        TEST_ASSERT_NOT_NULL_MESSAGE(ext,  "an in-range extension was NULL");
        TEST_ASSERT_NOT_NULL_MESSAGE(lang, "an in-range language was NULL");
        TEST_ASSERT_NOT_NULL_MESSAGE(jce_script_vm_find(lang),
            "an enumerated claim names a language that is not registered — "
            "the language list would then stop being a sound test for 'is "
            "this backend in this executable'");

        snprintf(path, sizeof path, "some/dir/file.%s", ext);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(lang,
            jce_script_vm_language_for_path(path),
            "the enumeration and the resolver disagree about a claim");

        if (strcmp(ext, JCE_SCRIPT_VM_DEFAULT_EXTENSION) == 0) seen_lua = 1;
    }
    TEST_ASSERT_EQUAL_MESSAGE(1, seen_lua,
        "the built-in's own claim is missing from the enumeration");

    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_extension_at(-1),
        "index -1 returned an extension");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_extension_at(n),
        "an out-of-range index returned an extension");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_extension_language_at(n),
        "an out-of-range index returned a language");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_lua_claims_its_own_extension);
    RUN_TEST(test_a_claim_resolves_a_path);
    RUN_TEST(test_a_path_with_no_extension_resolves_to_nothing);
    RUN_TEST(test_registered_language_without_a_claim_is_not_selected_by_path);
    RUN_TEST(test_claim_for_an_unregistered_language_is_refused);
    RUN_TEST(test_duplicate_extension_is_refused);
    RUN_TEST(test_unmatchable_claims_are_refused);
    RUN_TEST(test_enumeration_agrees_with_the_resolver);
    return UNITY_END();
}
