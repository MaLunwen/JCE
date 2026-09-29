/*
 * test_jce_script_vm_c.c — C is a driver language, and this is what that
 * sentence has to mean before it is true.
 *
 * WHAT WOULD BE GREEN WITHOUT scripting/c, AND MUST NOT BE
 * ------------------------------------------------------------------------
 * C was mechanically reachable before it was a language: JceCppScriptClass is
 * a plain C ABI and a C translation unit always compiled against it.  What did
 * not exist was a language a USER could reach — so every case below is written
 * against the things a user meets, and every one of them fails if "c" stops
 * being registered, stops claiming ".jcec", or stops resolving:
 *
 *   * jce_script_vm_language_for_path("X.jcec") answers "c"      (routing)
 *   * a .jcec scriptPath instantiates a class from a C module    (resolution)
 *   * the class's on_start fires once and on_update per step     (dispatch)
 *   * the handle names ITSELF "c", not "cpp"                     (diagnostics)
 *   * one shared native registry, reached from both languages    (the design)
 *
 * THE SUBJECT IS COMPILED AS C.  tests/scripting/c/vm_module/
 * jce_c_testmodule.c is built twice from one source — once into this
 * executable, once as a MODULE library loaded by absolute path — and the
 * CMakeLists asserts LINKER_LANGUAGE C on both.  A suite that proved this with
 * a .cpp module would prove the thing that was never in doubt.
 *
 * WHY THE HOST IS A RECORDER.  A script class has no return channel; the
 * engine calls it and keeps only the status.  So the module writes a line per
 * event through JceScriptHost::log and these cases assert on the trace.  That
 * makes "on_update was delivered" a counted fact rather than an absence of
 * errors — silence compares equal to silence.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/script_vm/jce_script_vm_c.h>

#include <jce/os/core/jce_log.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The modules compiled INTO this executable (see JCE_C_MODULE_END).  TWO of
 * them, because "two native modules in one binary" is itself under test --
 * see test_two_native_modules_live_in_one_binary and the header comment of
 * vm_module/jce_c_testmodule_two.c. */
const JceCModuleDesc *jce_c_testmodule(void);
const JceCModuleDesc *jce_c_testmodule_two(void);

/* ---- the recording host ---------------------------------------------- */

#define LOG_MAX 64
#define LINE_MAX 160

static char s_log[LOG_MAX][LINE_MAX];
static int  s_log_n;

static void rec_log(void *user, const char *msg)
{
    (void)user;
    if (s_log_n >= LOG_MAX || !msg) return;
    snprintf(s_log[s_log_n], LINE_MAX, "%s", msg);
    ++s_log_n;
}

static void log_reset(void) { s_log_n = 0; }

static int log_count(const char *needle)
{
    int i, n = 0;
    for (i = 0; i < s_log_n; ++i)
        if (strstr(s_log[i], needle) != NULL) ++n;
    return n;
}

/* Read a whole file, or NULL.  Used only by the diagnostics case at the foot
 * of this file, which has to read the ENGINE log rather than the host trace. */
static char *slurp(const char *path)
{
    FILE  *f = fopen(path, "rb");
    long   n;
    char  *buf;
    size_t got;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { fclose(f); return NULL; }
    got      = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static JceScriptHost make_host(void)
{
    JceScriptHost h;
    memset(&h, 0, sizeof h);
    h.user = (void *)0x1234;
    h.log  = rec_log;
    return h;
}

/* ---- one-time process state ------------------------------------------ */

static bool s_ready;
static bool s_dll_loaded;

/* jce_script_vm_register() is a startup-time operation and refuses a duplicate
 * language for good (live handles hold pointers into the registry), so this
 * runs ONCE for the whole executable rather than per case. */
static void ensure_registered(void)
{
    if (s_ready) return;
    s_ready = true;
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_cpp_register(),
        "the cpp backend must register: the c one is a second language over "
        "its registry and half these cases compare the two");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_c_register(),
        "jce_script_vm_c_register() refused -- there is no 'c' language and "
        "nothing below can be true");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        jce_script_vm_cpp_add_module(jce_c_testmodule()),
        "the C module compiled into this executable was refused by the "
        "registry (its own reason is on the line above)");
    TEST_ASSERT_NOT_NULL_MESSAGE(
        jce_script_vm_cpp_add_module(jce_c_testmodule_two()),
        "the SECOND static C module was refused -- two add_module() calls in "
        "one process is the shape jce_script_vm_c.h prescribes for a project "
        "that writes both native languages");
}

void setUp(void)
{
    ensure_registered();
    log_reset();
}

void tearDown(void) {}

/* ====================================================================== */
/*  Routing                                                               */
/* ====================================================================== */

/* THE CLAIM.  Without it a Script component naming a C class resolves to no
 * language and the entity is refused before any VM is asked -- exactly the
 * state the cpp backend was in before it claimed ".jcecpp". */
static void test_c_claims_jcec_and_routes(void)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c",
        jce_script_vm_language_for_path("scripts/CTick.jcec"),
        "a .jcec scriptPath does not route to the c VM");
    TEST_ASSERT_EQUAL_STRING("c",
        jce_script_vm_language_for_path("CTick.JCEC"));      /* case-folded */

    /* A SECOND call must not undo the first, and must not report failure: a
     * host that links two things which both register would otherwise have to
     * agree about which of them calls first. */
    TEST_ASSERT_TRUE(jce_script_vm_c_register());
    TEST_ASSERT_EQUAL_STRING("c",
        jce_script_vm_language_for_path("scripts/CTick.jcec"));
}

/* ".jcec" AND ".jcecpp" ARE TWO CLAIMS, and neither is a prefix of the other
 * for a matcher that reads the whole text after the last dot.  If this ever
 * goes red, a scene full of C++ scripts has started resolving to C. */
static void test_jcecpp_is_untouched_and_source_extensions_stay_unclaimed(void)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cpp",
        jce_script_vm_language_for_path("scripts/EsFlowerSway.jcecpp"),
        "adding the c language redirected .jcecpp -- every existing C++ "
        "script in every project just changed VM");

    /* THE SPELLINGS THAT MUST NEVER BE CLAIMED.  A ".c" or ".h" claim would
     * classify every translation unit and every vendored header in the
     * project as an attachable script, and would hand the cooker and the
     * publication policy a mandate to pack the project's SOURCE into the
     * shipped game.  This is the C half of the rule the cpp row already
     * carries for .cpp/.cc/.hpp. */
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("src/main.c"),
        "a .c became an attachable script -- the Script picker now offers "
        "every translation unit in the project");
    TEST_ASSERT_NULL_MESSAGE(jce_script_vm_language_for_path("src/main.h"),
        "a .h became an attachable script -- that is every vendored "
        "third-party header too");
    /* A bare class name has no extension: routing cannot answer, and the
     * class resolver is what handles it. */
    TEST_ASSERT_NULL(jce_script_vm_language_for_path("CTick"));
}

/* ====================================================================== */
/*  The table                                                             */
/* ====================================================================== */

/* THE "c" TABLE IS THE "cpp" TABLE APART FROM TWO MEMBERS, and this case is
 * what makes that sentence enforced rather than asserted in a comment.
 *
 * scripting/c derives its table by copying the cpp one at register time
 * precisely so that a slot appended to JceScriptVM propagates with no edit.
 * The two members that must NOT be shared are the language (or both entries
 * would be one language) and create_sized (or jce_script_vm_create() would
 * refuse every handle, because the header would name the wrong table). */
static void test_c_table_is_derived_from_cpp_apart_from_two_members(void)
{
    const JceScriptVM *c   = jce_script_vm_c();
    const JceScriptVM *cpp = jce_script_vm_cpp();

    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(cpp);

    TEST_ASSERT_EQUAL_STRING("c", c->language);
    TEST_ASSERT_EQUAL_STRING("cpp", cpp->language);
    TEST_ASSERT_TRUE_MESSAGE(c->create_sized != cpp->create_sized,
        "the c VM reuses the cpp create_sized -- every jce_script_vm_create"
        "(\"c\") would be refused, because the handle's header would name a "
        "table the registry did not dispatch through");

    TEST_ASSERT_EQUAL_UINT(cpp->struct_size, c->struct_size);
    TEST_ASSERT_TRUE(c->destroy            == cpp->destroy);
    TEST_ASSERT_TRUE(c->instantiate        == cpp->instantiate);
    TEST_ASSERT_TRUE(c->instantiate_source == cpp->instantiate_source);
    TEST_ASSERT_TRUE(c->call_start         == cpp->call_start);
    TEST_ASSERT_TRUE(c->call_update        == cpp->call_update);
    TEST_ASSERT_TRUE(c->release            == cpp->release);
    TEST_ASSERT_TRUE(c->call_collision     == cpp->call_collision);
    TEST_ASSERT_TRUE(c->call_message       == cpp->call_message);
    TEST_ASSERT_TRUE(c->call_anim_event    == cpp->call_anim_event);
    TEST_ASSERT_TRUE(c->call_named         == cpp->call_named);
    TEST_ASSERT_TRUE(c->call_named_num     == cpp->call_named_num);
    TEST_ASSERT_TRUE(c->call_named_str     == cpp->call_named_str);
    TEST_ASSERT_TRUE(c->instance_count     == cpp->instance_count);
    TEST_ASSERT_TRUE(c->update_coroutines  == cpp->update_coroutines);
    TEST_ASSERT_TRUE(c->compile_module     == cpp->compile_module);
    TEST_ASSERT_TRUE(c->rebind_instance    == cpp->rebind_instance);
    TEST_ASSERT_TRUE(c->release_module     == cpp->release_module);
}

/* ====================================================================== */
/*  Dispatch                                                              */
/* ====================================================================== */

/* THE WHOLE CLAIM, END TO END: a scriptPath a designer could type, resolved
 * to a class a C author wrote, stepped by the engine's ordinary lifecycle. */
static void test_a_c_class_runs_through_a_jcec_path(void)
{
    JceScriptHost     host = make_host();
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    s = jce_script_vm_create("c", &host, sizeof host);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, "jce_script_vm_create(\"c\") returned NULL");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c", jce_script_vm_language_of(s),
        "the handle does not report itself as c -- every diagnostic this "
        "backend prints would name the wrong language");

    /* THE STORED PATH, not a bare class name.  All three candidate forms are
     * tried and this is the one a scene actually holds. */
    inst = jce_script_instantiate(s, "scripts/CTick.jcec", 7u);
    TEST_ASSERT_TRUE_MESSAGE(inst != 0,
        "a .jcec scriptPath did not resolve to the C class -- C is not a "
        "usable language even though the VM exists");
    TEST_ASSERT_EQUAL_INT(1, log_count("CTick:create"));

    jce_script_call_start(s, inst);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count("CTick:on_start"),
        "on_start was not delivered exactly once");

    for (i = 0; i < 3; ++i) jce_script_call_update(s, inst, 0.5f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, log_count("on_update#"),
        "on_update was not delivered once per step");
    TEST_ASSERT_EQUAL_INT(1, log_count("on_update#3"));

    /* release runs on_destroy and then destroy, mirroring the Lua path. */
    jce_script_release(s, inst);
    TEST_ASSERT_EQUAL_INT(1, log_count("CTick:on_destroy"));
    TEST_ASSERT_EQUAL_INT(1, log_count("CTick:destroy"));

    jce_script_destroy(s);
}

/* THE THREE CANDIDATE FORMS still hold for a C path, and candidate 1 -- the
 * whole stored string -- is still tried FIRST.  That order is the
 * compatibility guarantee: a later candidate can only ADD a resolution. */
static void test_all_three_path_forms_reach_the_same_c_class(void)
{
    JceScriptHost host = make_host();
    JceScript    *s    = jce_script_vm_create("c", &host, sizeof host);
    const char   *forms[3];
    int           i;

    forms[0] = "CTick";
    forms[1] = "CTick.jcec";
    forms[2] = "gameplay/nested/CTick.jcec";

    TEST_ASSERT_NOT_NULL(s);
    for (i = 0; i < 3; ++i) {
        JceScriptInstance inst = jce_script_instantiate(s, forms[i], (uint32_t)(i + 1));
        TEST_ASSERT_TRUE_MESSAGE(inst != 0, forms[i]);
        jce_script_release(s, inst);
    }
    TEST_ASSERT_EQUAL_INT(3, log_count("CTick:create"));
    jce_script_destroy(s);
}

/* ONE REGISTRY, TWO LANGUAGES -- the design decision, made observable.
 *
 * The same class, published by one C module, is reachable from a "cpp" handle
 * as well.  That is not an accident to be tolerated: a compiled class has no
 * language at run time, and two registries would mean two loaders and two
 * unload refcounts over one set of .dll files.  What the extension states is
 * the AUTHOR's language, which is what the editor and the messages need. */
static void test_one_registry_serves_both_languages(void)
{
    JceScriptHost     host = make_host();
    JceScript        *sc   = jce_script_vm_create("cpp", &host, sizeof host);
    JceScriptInstance inst;

    TEST_ASSERT_NOT_NULL(sc);
    TEST_ASSERT_EQUAL_STRING("cpp", jce_script_vm_language_of(sc));

    inst = jce_script_instantiate(sc, "CTick", 11u);
    TEST_ASSERT_TRUE_MESSAGE(inst != 0,
        "the cpp VM could not see a class published by a C module -- the two "
        "languages have grown separate registries, and a project would now "
        "need two loaders for one .dll");
    jce_script_release(sc, inst);
    jce_script_destroy(sc);
}

/* A C CLASS REPORTS FAILURE BY RETURNING A STRING, which is the whole of the
 * error protocol for a language with no exceptions -- and on_message is the
 * dispatcher that deliberately disables NOTHING, because one thunk serves
 * every message name and a rule keyed on the label would silently kill every
 * message the instance receives. */
static void test_a_c_status_string_is_an_error_and_on_message_disables_nothing(void)
{
    JceScriptHost     host = make_host();
    JceScript        *s    = jce_script_vm_create("c", &host, sizeof host);
    JceScriptInstance inst;

    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate(s, "CTick.jcec", 3u);
    TEST_ASSERT_TRUE(inst != 0);

    jce_script_call_message(s, inst, "boom", 0.0, NULL);
    /* Both sides of the report: the class's own line, and the engine's
     * "<method> error: <text>" through the SAME host log. */
    TEST_ASSERT_EQUAL_INT(1, log_count("CTick:boom"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count("on_message error: C said no"),
        "a returned status string was not reported as an error");

    /* Still delivered afterwards -- nothing was disabled. */
    log_reset();
    jce_script_call_message(s, inst, "ping", 0.0, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count("CTick:ping"),
        "on_message stopped being delivered after one error -- the failing-"
        "callback rule fired where jce_script_vm_cpp.c passes CB_NONE");

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* ====================================================================== */
/*  The shared object                                                     */
/* ====================================================================== */

/* A C MODULE BUILT AS A REAL MODULE LIBRARY, loaded the way editor Play loads
 * a project's module: by absolute path, through the exported entry symbol.
 *
 * A statically linked stand-in cannot fail the way a plugin fails, and the
 * exported symbol is the half a C author could most easily not have: it is
 * emitted by JCE_C_MODULE_END, and it is deliberately the SAME symbol name the
 * cpp macro emits, because one loader looks it up. */
static void test_a_c_module_loads_as_a_shared_object(void)
{
    JceScriptHost     host = make_host();
    JceScript        *s;
    JceScriptInstance inst;

    if (!s_dll_loaded) {
        JceCModule *m = jce_script_vm_cpp_load_library(JCE_C_TESTMODULE_PATH);
        TEST_ASSERT_NOT_NULL_MESSAGE(m,
            "the C module built as a MODULE library did not load from "
            JCE_C_TESTMODULE_PATH " -- either JCE_C_MODULE_END emitted no "
            "exported entry, or the registry refused it (reason above)");
        TEST_ASSERT_EQUAL_STRING("c_testmodule_dll",
                                 jce_script_vm_cpp_module_name(m));
        s_dll_loaded = true;
    }

    TEST_ASSERT_TRUE(jce_script_vm_cpp_has_class("CDllSpin.jcec"));

    s = jce_script_vm_create("c", &host, sizeof host);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate(s, "scripts/CDllSpin.jcec", 21u);
    TEST_ASSERT_TRUE_MESSAGE(inst != 0,
        "a class from a C module loaded as a shared object did not resolve");
    jce_script_call_start(s, inst);
    jce_script_call_update(s, inst, 0.25f);
    TEST_ASSERT_EQUAL_INT(1, log_count("dll:CDllSpin:on_start"));
    TEST_ASSERT_EQUAL_INT(1, log_count("dll:CDllSpin:on_update#1"));

    jce_script_release(s, inst);
    jce_script_destroy(s);
}

/* AN UNRESOLVED NAME REFUSES.  The refusal itself is this case's contract:
 * instantiate returns 0 rather than a handle that dispatches nowhere.  What
 * the refusal SAYS is asserted by the case below it, which had to reach the
 * engine log to do it. */
/* TWO NATIVE MODULES, ONE BINARY -- and the case is mostly about the fact
 * that this executable EXISTS.
 *
 * JCE_C_MODULE_END and JCE_CPP_MODULE_END each emit the shared-object entry
 * point `jce_cpp_script_module`, which is one external symbol, and they
 * emitted it unconditionally: two statically linked native modules produced
 * `LNK2005: jce_cpp_script_module already defined` and the binary could not
 * be built.  elemental_serenity hit it the first time it compiled a C script
 * beside its C++ one, which is the arrangement jce_script_vm_c.h prescribes
 * ("a project that wants both publishes TWO modules").
 *
 * So the build is the first half of this test and it cannot be written as an
 * assertion; the CMakeLists says which define is load-bearing.  What IS
 * assertable is the half that would still be wrong if the two modules were
 * merged or one were dropped: both are registered, their classes are
 * DISTINCT, and each dispatches to its own code. */
static void test_two_native_modules_live_in_one_binary(void)
{
    JceScriptHost     host = make_host();
    JceScript        *s;
    JceScriptInstance a, b;

    s = jce_script_vm_create("c", &host, sizeof host);
    TEST_ASSERT_NOT_NULL(s);

    a = jce_script_instantiate(s, "scripts/CTick.jcec", 41u);
    b = jce_script_instantiate(s, "scripts/CTwo.jcec",  42u);
    TEST_ASSERT_TRUE_MESSAGE(a != 0,
        "the first static module's class did not resolve");
    TEST_ASSERT_TRUE_MESSAGE(b != 0,
        "the SECOND static module's class did not resolve -- one registry "
        "serves every loaded module, so both must be reachable");
    TEST_ASSERT_TRUE_MESSAGE(a != b, "two classes returned one instance");

    /* Each instance must reach ITS OWN code.  The two modules tag their trace
     * differently ("static:" vs "second:"), so a merged or shadowed class
     * would show up as two lines with one tag rather than one line each. */
    jce_script_call_start(s, a);
    jce_script_call_start(s, b);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count("static:CTick:on_start"),
        "the first module's on_start did not fire exactly once");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count("second:CTwo:on_start"),
        "the second module's on_start did not fire exactly once -- ASSERTED "
        "SEPARATELY on purpose: silence compares equal to silence, and a "
        "combined count of 2 would also pass if one class ran twice");

    jce_script_release(s, a);
    jce_script_release(s, b);
    jce_script_destroy(s);
}

static void test_an_unknown_c_class_is_refused(void)
{
    JceScriptHost host = make_host();
    JceScript    *s    = jce_script_vm_create("c", &host, sizeof host);

    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_UINT(0u,
        jce_script_instantiate(s, "scripts/NoSuchThing.jcec", 5u));
    TEST_ASSERT_EQUAL_INT(0, jce_script_instance_count(s));
    jce_script_destroy(s);
}

/* THE REFUSAL A C AUTHOR ACTUALLY READS SAYS "c".
 *
 * WHY THIS CASE EXISTS: IT WAS FOUND MISSING BY MUTATION.  Deleting the two
 * lines of vm_lang() in scripting/cpp/src/jce_script_vm_cpp.c -- so that one
 * set of shared functions hardcodes "cpp" again, as it did before this
 * language existed -- left the ENTIRE tree green: 350/350 unit tests, the
 * catalog gate, and every other case in this file.  Nothing anywhere executed
 * the claim.
 *
 * That claim is not decoration.  It is the ONLY thing separating this design
 * from the alias design that was rejected: "c" is a separate registered
 * language so that a user who writes C is never told they wrote C++, which is
 * the lying-message defect commits 028cc503 and 1a7323cc each closed once.  A
 * contract asserted in four comments, a README table and a header, and
 * enforced by nothing, is exactly the defect class this campaign keeps
 * finding -- so it is enforced here.
 *
 * THE ENGINE LOG IS THE ONLY PLACE THIS IS OBSERVABLE.  The recording host
 * above sees JceScriptHost::log, and cpp_instantiate's refusal never goes
 * there; it goes to LOG_ERROR.  So the file sink is the capture seam, exactly
 * as tests/os/core/test_jce_log_colors.c uses it.
 *
 * BOTH SIDES ARE ASSERTED TO HAVE PRODUCED OUTPUT, independently, before
 * either is trusted: silence compares equal to silence, and a test that only
 * checked "cpp is absent" would pass on an empty file -- including on a build
 * where the refusal never happened at all. */
static void test_the_refusal_a_c_author_reads_says_c(void)
{
    static const char path[] = "test_jce_script_vm_c_diag.log";
    JceScriptHost     host   = make_host();
    JceScript        *c;
    JceScript        *cpp;
    char             *text;

    jce_log_set_file(path);

    c = jce_script_vm_create("c", &host, sizeof host);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_UINT(0u,
        jce_script_instantiate(c, "scripts/NoSuchThing.jcec", 7u));
    jce_script_destroy(c);

    /* The same miss under the OTHER language, in the same process and through
     * the same functions -- which is what makes the two spellings a
     * discrimination rather than a coincidence. */
    cpp = jce_script_vm_create("cpp", &host, sizeof host);
    TEST_ASSERT_NOT_NULL(cpp);
    TEST_ASSERT_EQUAL_UINT(0u,
        jce_script_instantiate(cpp, "scripts/NoSuchThing.jcecpp", 8u));
    jce_script_destroy(cpp);

    jce_log_flush();
    jce_log_set_file(NULL);     /* releases the file; Windows holds it open */

    text = slurp(path);
    TEST_ASSERT_NOT_NULL_MESSAGE(text,
        "the log sink produced no file -- this case proves nothing until it "
        "does, so it fails rather than passing on an absence");

    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "no cpp script class resolves"),
        "the cpp refusal did not reach the log at all -- the capture seam is "
        "broken, not the language, and the c assertion below would have "
        "'passed' on the same empty evidence");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "no c script class resolves"),
        "a C script's refusal named the wrong language. One set of functions "
        "serves both registrations, so the language must be read from the "
        "handle (vm_lang) -- hardcoding 'cpp' tells a C author their script "
        "is C++, which is the whole defect a separate 'c' language exists to "
        "prevent");

    free(text);
    remove(path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_c_claims_jcec_and_routes);
    RUN_TEST(test_jcecpp_is_untouched_and_source_extensions_stay_unclaimed);
    RUN_TEST(test_c_table_is_derived_from_cpp_apart_from_two_members);
    RUN_TEST(test_a_c_class_runs_through_a_jcec_path);
    RUN_TEST(test_all_three_path_forms_reach_the_same_c_class);
    RUN_TEST(test_one_registry_serves_both_languages);
    RUN_TEST(test_a_c_status_string_is_an_error_and_on_message_disables_nothing);
    RUN_TEST(test_a_c_module_loads_as_a_shared_object);
    RUN_TEST(test_two_native_modules_live_in_one_binary);
    RUN_TEST(test_an_unknown_c_class_is_refused);
    RUN_TEST(test_the_refusal_a_c_author_reads_says_c);
    return UNITY_END();
}
