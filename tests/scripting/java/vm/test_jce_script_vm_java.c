/*
 * test_jce_script_vm_java.c — everything about the Java backend that the
 * cross-language differential structurally CANNOT see.
 *
 * The differential proves the Java lifecycle matches Lua's.  It cannot prove
 * anything about the JVM, because Lua has no JVM to disagree with: one VM per
 * process, which thread may touch a handle, what a pending exception does to
 * the NEXT call, and what a short JceScriptHost must not be read past.  Those
 * are here.
 *
 * ORDERING IS LOAD-BEARING IN THIS FILE, and it is stated rather than left to
 * be discovered.  jce_script_vm_java_configure() is REFUSED once the JVM is
 * running — a JVM's options are fixed at creation, so accepting a later change
 * would be a setting that reads back and does nothing.  Every test that needs
 * a different configuration therefore has to run BEFORE the first successful
 * create, and main() runs them in that order.  Unity runs tests in the order
 * main() names them, which is what makes that possible.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>
#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_alloc.h>   /* read_file's buffer is freed with jce_free */
#include <jce/os/core/jce_thread.h>
#include <jce/os/platform/jce_library.h>
#include <jce/script_vm/jce_script_vm_java.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  define TEST_JVM_LIBRARY JCE_JAVA_VM_TEST_JVM_WIN
#elif defined(__APPLE__)
#  define TEST_JVM_LIBRARY JCE_JAVA_VM_TEST_JVM_MAC
#else
#  define TEST_JVM_LIBRARY JCE_JAVA_VM_TEST_JVM_NIX
#endif

/* ── A recording host ───────────────────────────────────────────────────── */

#define REC_MAX_LINES 64
#define REC_LINE_MAX  512

typedef struct Rec {
    char  lines[REC_MAX_LINES][REC_LINE_MAX];
    int   count;
    int   get_position_calls;
    int   set_position_calls;
    JceScriptEntity last_set_entity;
    float last_set_xyz[3];
} Rec;

static Rec g_rec;

static void rec_log(void *user, const char *msg)
{
    Rec *r = (Rec *)user;
    if (!r || r->count >= REC_MAX_LINES) return;
    snprintf(r->lines[r->count], REC_LINE_MAX, "%s", msg ? msg : "(null)");
    ++r->count;
}

static bool rec_get_position(void *user, JceScriptEntity e, float out[3])
{
    Rec *r = (Rec *)user;
    if (r) ++r->get_position_calls;
    out[0] = (float)e;
    out[1] = 0.0f;
    out[2] = 0.0f;
    return true;
}

static void rec_set_position(void *user, JceScriptEntity e,
                             float x, float y, float z)
{
    Rec *r = (Rec *)user;
    if (!r) return;
    ++r->set_position_calls;
    r->last_set_entity = e;
    r->last_set_xyz[0] = x;
    r->last_set_xyz[1] = y;
    r->last_set_xyz[2] = z;
}

static void rec_reset(void) { memset(&g_rec, 0, sizeof(g_rec)); }

/* Reads `path` from the pre-compiled fixture directory.
 *
 * Every other test in this file goes through instantiate_SOURCE, which needs
 * no host file access at all.  The PATH slot — the one the runtime actually
 * calls for a Script component — cannot be reached without this, which is why
 * it did not exist here before: the whole bytecode branch was unreachable from
 * this suite.  Allocation is jce_malloc because the VM frees with jce_free
 * (JceScriptHost::read_file's contract). */
static void *rec_read_file(void *user, const char *path, uint64_t *out_size)
{
    char   full[1024];
    FILE  *f;
    long   len;
    void  *buf;

    (void)user;
    if (out_size) *out_size = 0;
    if (!path) return NULL;

    snprintf(full, sizeof(full), "%s/%s", JCE_JAVA_VM_TEST_PRECOMPILED_DIR,
             path);
    f = fopen(full, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    rewind(f);
    buf = jce_malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len) {
        fclose(f);
        jce_free(buf);
        return NULL;
    }
    fclose(f);
    if (out_size) *out_size = (uint64_t)len;
    return buf;
}

static void host_fill(JceScriptHost *h)
{
    memset(h, 0, sizeof(*h));
    h->user = &g_rec;
    h->log = rec_log;
    h->get_position = rec_get_position;
    h->set_position = rec_set_position;
    h->read_file = rec_read_file;
}

/* Did any recorded line contain `needle`? */
static bool rec_saw(const char *needle)
{
    int i;
    for (i = 0; i < g_rec.count; ++i)
        if (strstr(g_rec.lines[i], needle)) return true;
    return false;
}

/* ── Scripts, as source text ────────────────────────────────────────────── */

static const char *const k_logger =
    "import com.jce.script.vm.JceEntityScript;\n"
    "class Logger extends JceEntityScript {\n"
    "  public void onStart() { log(\"started \" + entity()); }\n"
    "  public void onUpdate(float dt) { log(\"updated\"); }\n"
    "}\n";

static const char *const k_thrower =
    "import com.jce.script.vm.JceEntityScript;\n"
    "class Thrower extends JceEntityScript {\n"
    "  int starts = 0;\n"
    /* onStart logs and then throws from its SECOND dispatch onward.  The first
     * dispatch is the "a different hook still runs" probe after on_update is
     * disabled; the second and third are the only thing in this suite that can
     * see call_start passing a non-participating hook name. */
    "  public void onStart() {\n"
    "    log(\"alive\");\n"
    "    starts = starts + 1;\n"
    "    if (starts >= 2) throw new IllegalStateException(\"start nope\");\n"
    "  }\n"
    "  public void onUpdate(float dt) { throw new IllegalStateException(\"nope\"); }\n"
    "}\n";

/* A static counter, to ask whether two handles share class storage. */
static const char *const k_counter =
    "import com.jce.script.vm.JceEntityScript;\n"
    "class Counter extends JceEntityScript {\n"
    "  static int seen = 0;\n"
    "  public void onStart() { seen = seen + 1; log(\"seen=\" + seen); }\n"
    "}\n";

static const char *const k_probe_position =
    "import com.jce.script.vm.JceEntityScript;\n"
    "class Probe extends JceEntityScript {\n"
    "  public void onStart() {\n"
    "    float[] p = new float[3];\n"
    "    log(getPosition(7L, p) ? \"HAVE\" : \"ABSENT\");\n"
    "  }\n"
    "}\n";

static const char *const k_not_a_script =
    "class Bystander {\n"
    "  public int value() { return 3; }\n"
    "}\n";

void setUp(void) { rec_reset(); }
void tearDown(void) {}

/* Register once; the registry refuses a second registration by design. */
static void ensure_registered(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    TEST_ASSERT_TRUE_MESSAGE(jce_script_vm_java_register(),
        "jce_script_vm_java_register() was refused — a NULL slot in "
        "k_java_vm is the only way that happens, and the log names it");
}

static void configure_for_real(void)
{
    JceScriptVmJavaConfig cfg;
    static char           lib_opt[1024];
    const char           *opts[1];

    /* -Djce.script.library is set HERE and not later, because a JVM's options
     * are fixed at creation and configure() is refused once it is running.
     * com.jce.script.JceScript reads that property in its static initialiser
     * to find the JNI shim; without it, System.loadLibrary would search the
     * platform library path, which this test process has not arranged. */
    snprintf(lib_opt, sizeof(lib_opt), "-Djce.script.library=%s",
             JCE_JAVA_VM_TEST_SCRIPT_LIB);
    opts[0] = lib_opt;

    memset(&cfg, 0, sizeof(cfg));
    cfg.jvm_library = TEST_JVM_LIBRARY;
    cfg.class_path = JCE_JAVA_VM_TEST_CLASSES;
    cfg.options = opts;
    cfg.option_count = 1;
    (void)jce_script_vm_java_configure(&cfg, sizeof(cfg));
}

/* Registration alone does not make a .java RUN.  The runtime picks a language
 * per script from the PATH, through the extension claims this backend makes
 * for itself in jce_script_vm_java_register().  BOTH are required: instantiate
 * dispatches on the class-file magic and accepts either form, so claiming only
 * .java would leave every pre-compiled deployment unreachable -- and
 * unreachable SILENTLY, because an unclaimed extension is not an error at the
 * backend, it is an entity whose script never loads. */
static void test_the_backend_claims_java_and_class(void)
{
    ensure_registered();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("java",
        jce_script_vm_language_for_path("assets/scripts/Spinner.java"),
        "no VM claims '.java' although the java backend registered -- a "
        "Spinner.java in a scene would resolve to nothing and never run");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("java",
        jce_script_vm_language_for_path("assets/scripts/Spinner.class"),
        "no VM claims '.class' -- a pre-compiled deployment (the shape a "
        "shipped game uses, since it needs no JDK) would never run");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("java",
        jce_script_vm_language_for_path("A/B/Spinner.CLASS"),
        "extension matching is not case-insensitive");
}

/* ── (1b) A PRE-COMPILED .class runs — the shape a shipped game uses ───────
 *
 * WHAT WAS UNTESTED.  Three separate comments in this repo promised this and
 * nothing checked it: jce_script_vm_java_register() ("instantiate dispatches on
 * the class-file magic and accepts either"), jce_asset_ext.c ("`.class` is
 * java's compiled form and is listed because a shipped Java game carries
 * bytecode, not sources"), and test_the_backend_claims_java_and_class above
 * ("a pre-compiled deployment ... would never run").  The claim on the
 * EXTENSION was tested; the LOADING of bytecode was not.  The one test that
 * mentioned .class bytes fed it the ASCII string "CAFEBABE-not-really", whose
 * first four bytes are 'C','A','F','E' — so it took the SOURCE branch, and
 * JceScriptRuntime.isClassFile()/defineFromClassFile() had no coverage at all.
 *
 * WHY THIS IS THE SHAPE THAT MATTERS.  Source form compiles in memory through
 * javax.tools and therefore needs a JDK on the player's machine; bytecode
 * loads on a plain JRE.  A shipped game ships the second one, so the form with
 * no coverage was the form every real deployment uses.
 *
 * WHY THE FILE NAME IS WRONG ON PURPOSE.  The fixture's class is `Precompiled`
 * and the test loads it as "shipped_asset.class" (the build renames it).  The
 * source branch derives the class name from the PATH it is handed, so it could
 * not produce a class called Precompiled from a file called shipped_asset —
 * and it could not have compiled binary input in the first place.  Success here
 * therefore proves the BYTECODE branch ran and that a .class carries its own
 * identity in its bytes, which is the property that lets a shipped game rename,
 * repack and lowercase its assets (jce_pak stores paths lowercased) without
 * breaking Java's class-name-is-file-name convention.
 *
 * MEASURED END TO END (elemental_serenity, packaged PAK-only build,
 * 2026-08-16): with the scene pointing at "scripts/EsCampfire.class" the
 * runtime logged `script: loaded 'scripts/EsCampfire.class' (java)` and the
 * script's on_start ran once with both campfire lights resolved. */
static void test_a_precompiled_class_runs_through_the_path_slot(void)
{
    JceScriptHost      host;
    JceScript         *s;
    JceScriptInstance  i;
    /* Observations are COPIED OUT and every assertion is made after the VM has
     * been destroyed.  Unity's TEST_ASSERT_* longjmps out of the test function
     * on failure, so an assertion placed before jce_script_destroy() skips it —
     * and a leaked Java handle is not a local problem: live_handles is global,
     * so the NEXT test's "Expected 1 Was 2" fails too and one regression is
     * reported as two, in two places, one of them innocent.  Measured: that is
     * exactly what happened when this test was first written with the asserts
     * inline and isClassFile() was mutated to return false. */
    int              starts_seen;
    JceScriptEntity  bound_entity;
    float            xyz[3];
    bool             saw_start_line;
    bool             saw_update_line;

    ensure_registered();
    configure_for_real();
    host_fill(&host);
    rec_reset();

    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);

    /* The path slot, not instantiate_source: this is the call the runtime
     * makes for a Script component, and the only one that reads bytes. */
    i = jce_script_instantiate(s, "shipped_asset.class", 77u);
    if (i == 0) {
        jce_script_destroy(s);
        TEST_FAIL_MESSAGE(
            "a pre-compiled .class did not instantiate through the path slot. "
            "Either the class-file magic was not recognised (it went to the "
            "source compiler and failed), or the fixture was not built into "
            JCE_JAVA_VM_TEST_PRECOMPILED_DIR);
        return;
    }

    jce_script_call_start(s, i);
    saw_start_line  = rec_saw("precompiled onStart 77");
    starts_seen     = g_rec.set_position_calls;
    bound_entity    = g_rec.last_set_entity;
    xyz[0]          = g_rec.last_set_xyz[0];
    xyz[1]          = g_rec.last_set_xyz[1];
    xyz[2]          = g_rec.last_set_xyz[2];

    jce_script_call_update(s, i, 0.016f);
    saw_update_line = rec_saw("precompiled onUpdate");

    jce_script_release(s, i);
    jce_script_destroy(s);

    /* Independent of the log: a typed observation the script itself made.
     * Asserting BOTH matters — a silent VM and a working one both produce no
     * error, so the test has to see output that only a running onStart can
     * produce. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, starts_seen,
        "onStart did not run for the pre-compiled class");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(77u, (uint64_t)bound_entity,
        "the pre-compiled instance was not bound to its owning entity");
    TEST_ASSERT_EQUAL_FLOAT(11.0f, xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(22.0f, xyz[1]);
    TEST_ASSERT_EQUAL_FLOAT(33.0f, xyz[2]);

    /* And the class identity came from the BYTES, not from the file stem. */
    TEST_ASSERT_TRUE_MESSAGE(saw_start_line,
        "the script's own log line is missing, so the class that ran was not "
        "the one compiled into shipped_asset.class");
    TEST_ASSERT_TRUE_MESSAGE(saw_update_line,
        "on_update is not dispatched to a bytecode-loaded instance, so a "
        "shipped game's Java scripts would start and then freeze");
}

/* ── (1) A bare library name is refused, not resolved ───────────────────────
 *
 * MUST RUN FIRST: it needs a configuration the later tests replace, and
 * configure() is refused once the JVM is up.
 *
 * jce_library_open() documents that on Windows it attaches to an
 * already-resident module of the same name rather than loading the one you
 * named — so "jvm.dll" binds to whichever JVM some other component in the
 * process loaded first, which is a JVM with somebody else's options. */
static void test_a_bare_jvm_library_name_is_refused(void)
{
    JceScriptVmJavaConfig cfg;
    JceScript            *s;
    JceScriptHost         host;

    ensure_registered();
    host_fill(&host);

    memset(&cfg, 0, sizeof(cfg));
    cfg.jvm_library = "jvm.dll";           /* no directory: the whole point */
    cfg.class_path = JCE_JAVA_VM_TEST_CLASSES;
    TEST_ASSERT_TRUE(jce_script_vm_java_configure(&cfg, sizeof(cfg)));

    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NULL_MESSAGE(s,
        "a bare JVM library name was accepted; it must be refused, because "
        "the loader would attach to whatever module of that name is already "
        "resident");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, jce_script_vm_java_jvm_creations(),
        "the refusal must happen before JNI_CreateJavaVM");
}

/* ── (2) The JVM starts, and exactly once ───────────────────────────────── */

static void test_the_jvm_starts_and_a_handle_is_created(void)
{
    JceScriptHost host;
    JceScript    *s;

    ensure_registered();
    configure_for_real();
    host_fill(&host);

    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL_MESSAGE(s,
        "no Java VM. Check JAVA_HOME and that the scripting classes were "
        "built into " JCE_JAVA_VM_TEST_CLASSES);
    TEST_ASSERT_EQUAL_STRING("java", jce_script_vm_language_of(s));
    TEST_ASSERT_EQUAL_INT(1, jce_script_vm_java_live_handles());
    jce_script_destroy(s);
    TEST_ASSERT_EQUAL_INT(0, jce_script_vm_java_live_handles());
}

static void test_the_jvm_is_created_exactly_once(void)
{
    JceScriptHost host;
    JceScript    *a;
    JceScript    *b;
    int           before;

    host_fill(&host);
    before = jce_script_vm_java_jvm_creations()
           + jce_script_vm_java_jvm_adoptions();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, before,
        "the JVM should already exist exactly once by now");

    a = jce_script_vm_create("java", &host, sizeof(host));
    b = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL_MESSAGE(b,
        "a SECOND handle was refused. JNI_CreateJavaVM runs once per process; "
        "the second handle must reuse the JVM, not fail.");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        1, jce_script_vm_java_jvm_creations()
           + jce_script_vm_java_jvm_adoptions(),
        "the second handle started a second JVM");
    jce_script_destroy(a);
    jce_script_destroy(b);
}

/* ── (3) Destroy must not tear the JVM down ─────────────────────────────── */

static void test_a_handle_created_after_another_is_destroyed_still_works(void)
{
    JceScriptHost     host;
    JceScript        *a;
    JceScript        *b;
    JceScriptInstance i;

    host_fill(&host);
    a = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(a);
    jce_script_destroy(a);

    b = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL_MESSAGE(b,
        "creating a handle AFTER the previous one was destroyed failed — "
        "which is what calling DestroyJavaVM in destroy() looks like, an "
        "arbitrary distance from the destroy that caused it");
    i = jce_script_instantiate_source(b, "Logger", k_logger, 42u);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, i, "the reused JVM cannot run a script");
    jce_script_call_start(b, i);
    TEST_ASSERT_TRUE(rec_saw("started 42"));
    jce_script_destroy(b);
}

/* ── (4) Two handles do not share a script's static storage ─────────────── */

static void test_two_handles_do_not_share_script_statics(void)
{
    JceScriptHost     host;
    JceScript        *a;
    JceScript        *b;
    JceScriptInstance ia;
    JceScriptInstance ib;

    host_fill(&host);
    a = jce_script_vm_create("java", &host, sizeof(host));
    b = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    ia = jce_script_instantiate_source(a, "Counter", k_counter, 1u);
    ib = jce_script_instantiate_source(b, "Counter", k_counter, 2u);
    TEST_ASSERT_NOT_EQUAL(0, ia);
    TEST_ASSERT_NOT_EQUAL(0, ib);
    jce_script_call_start(a, ia);
    jce_script_call_start(b, ib);

    /* Both must say seen=1.  One JVM serves both handles, so "seen=2" is the
     * signature of two handles sharing one Class object — i.e. one entity's
     * script writing another's static state. */
    TEST_ASSERT_EQUAL_INT(2, g_rec.count);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("seen=1", g_rec.lines[0], "first handle");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("seen=1", g_rec.lines[1],
        "the second handle saw the FIRST handle's static counter — the "
        "per-handle JceScriptClassLoader is what keeps them apart");

    jce_script_destroy(a);
    jce_script_destroy(b);
}

/* ── (5) A slot called from another thread is refused ───────────────────── */

typedef struct WorkerArg {
    JceScript        *vm;
    JceScriptInstance inst;
    JceScriptHost    *host;
    bool              own_a_handle_first;
    bool              made_own_handle;
} WorkerArg;

static void worker_calls_slots(void *arg)
{
    WorkerArg         *w = (WorkerArg *)arg;
    const JceScriptVM *vm = jce_script_vm_java();
    JceScript         *mine = NULL;

    if (w->own_a_handle_first) {
        /* THE POINT OF THIS BRANCH.  Without it the worker is not attached to
         * the JVM at all, GetEnv fails, and the slot no-ops for a reason that
         * has nothing to do with ownership — so the test would pass with the
         * owning-thread check deleted.  Measured: it did.  Creating a handle
         * here attaches this thread, so from now on only the ownership
         * comparison can refuse. */
        mine = jce_script_vm_create("java", w->host, sizeof(*w->host));
        w->made_own_handle = mine != NULL;
    }

    /* DIRECTLY through the vtable, not through jce_script_call_*: the engine's
     * own forwarder would refuse this first, and then this test would be
     * proving the engine's check rather than the backend's.  A plugin holding
     * the table can reach these, so the backend must refuse on its own. */
    vm->call_start(w->vm, w->inst);
    vm->call_update(w->vm, w->inst, 0.5f);
    (void)vm->instance_count(w->vm);

    if (mine) jce_script_destroy(mine);
}

static void run_worker(WorkerArg *w)
{
    JceThread *t = jce_thread_create(worker_calls_slots, w, "jce_java_vm_test");
    TEST_ASSERT_NOT_NULL(t);
    jce_thread_join(t);
}

/* A thread the JVM has never seen cannot dispatch — and this passes because
 * GetEnv reports JNI_EDETACHED, NOT because of the ownership rule.  It is here
 * because that path is real and worth pinning, and it is named for what it
 * actually checks so nobody reads it as the ownership test.  The ownership
 * test is the next one. */
static void test_a_thread_the_jvm_never_saw_cannot_dispatch(void)
{
    JceScriptHost host;
    JceScript    *s;
    WorkerArg     w;

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    memset(&w, 0, sizeof(w));
    w.vm = s;
    w.host = &host;
    w.inst = jce_script_instantiate_source(s, "Logger", k_logger, 9u);
    TEST_ASSERT_NOT_EQUAL(0, w.inst);

    rec_reset();
    run_worker(&w);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rec.count,
        "an unattached thread dispatched into the JVM");
    jce_script_destroy(s);
}

/* THE OWNERSHIP TEST.  The worker owns a handle of its own, so it IS attached
 * and GetEnv succeeds for it; the only thing that can refuse its call into
 * ANOTHER handle is the owning-thread comparison.  A JNIEnv is per-thread and
 * JceScriptRuntime is not synchronised, so the answer is refusal — never
 * AttachCurrentThread, which would hand two threads one unsynchronised
 * runtime. */
static void test_a_slot_called_from_another_attached_thread_is_refused(void)
{
    JceScriptHost host;
    JceScript    *s;
    WorkerArg     w;

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    memset(&w, 0, sizeof(w));
    w.vm = s;
    w.host = &host;
    w.own_a_handle_first = true;
    w.inst = jce_script_instantiate_source(s, "Logger", k_logger, 9u);
    TEST_ASSERT_NOT_EQUAL(0, w.inst);

    rec_reset();
    run_worker(&w);

    TEST_ASSERT_TRUE_MESSAGE(w.made_own_handle,
        "the worker could not make its own handle, so it was never attached "
        "to the JVM and this test degenerates into the previous one");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rec.count,
        "a slot ran on a thread that does not own the handle, on a thread the "
        "JVM knows — so GetEnv succeeded and nothing else refused it");

    /* And the owning thread is unaffected: the refusal is not a poisoned VM. */
    jce_script_call_start(s, w.inst);
    TEST_ASSERT_TRUE_MESSAGE(rec_saw("started 9"),
        "the owning thread stopped working after another thread was refused");
    jce_script_destroy(s);
}

/* ── (6) A throw is reported, the hook is disabled, the rest lives on ──────
 *
 * Three facts, and an implementation that got two of them right would pass a
 * test that checked only the third:
 *
 *   - the throw is REPORTED, carrying the exception's own detail.  Without
 *     that line an escaped Java exception is invisible to the game.
 *   - the throw DISABLES that hook on that instance (jce_script.h's
 *     FAILING-CALLBACK RULE).  call_update returns void, so the only way to
 *     see this is the host stream going quiet for the later dispatches.
 *   - a DIFFERENT hook on the SAME instance still dispatches.  That separates
 *     "the hook was disabled" from "the instance was dropped", and it is also
 *     still the check that the pending exception was CLEARED: with one
 *     pending, every later JNI call is undefined. */
static void test_a_throwing_hook_is_reported_disabled_and_leaves_the_rest_alive(void)
{
    JceScriptHost     host;
    JceScript        *s;
    JceScriptInstance i;
    char              notice[512];

    snprintf(notice, sizeof notice, JCE_SCRIPT_DISABLED_NOTICE_FMT,
             "on_update");

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    i = jce_script_instantiate_source(s, "Thrower", k_thrower, 3u);
    TEST_ASSERT_NOT_EQUAL(0, i);

    rec_reset();
    jce_script_call_start(s, i);             /* 1st: logs, does not throw */
    jce_script_call_update(s, i, 0.5f);      /* throws */
    jce_script_call_update(s, i, 0.5f);      /* disabled: records nothing */
    jce_script_call_update(s, i, 0.5f);      /* still nothing */
    jce_script_call_start(s, i);             /* a different hook: unaffected */
    jce_script_call_start(s, i);             /* 3rd: on_start now disabled */

    TEST_ASSERT_EQUAL_INT_MESSAGE(6, g_rec.count,
        "expected alive / on_update error / on_update disabled / alive / "
        "on_start error / on_start disabled — a seventh line means the third "
        "call_start dispatched anyway, i.e. it passes a hook name the rule "
        "does not recognise");
    TEST_ASSERT_EQUAL_STRING("alive", g_rec.lines[0]);
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(g_rec.lines[1], "on_update error:") == g_rec.lines[1],
        "the throw was not reported through the host at all");
    TEST_ASSERT_TRUE_MESSAGE(strstr(g_rec.lines[1], "nope") != NULL,
        "the report carries no detail from the exception");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(notice, g_rec.lines[2],
        "the disable was not announced in the wording jce_script.h publishes; "
        "every backend must write the same sentence or the cross-language "
        "differentials compare two wordings instead of one behaviour");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("alive", g_rec.lines[3],
        "on_start stopped running on an instance whose on_update was "
        "disabled: either the disable took the whole instance, or the "
        "exception was still pending");

    /* And on_start participates on its own account.  jce_script_call_start has
     * no at-most-once contract; this runtime happens to call it once per
     * instantiate, but that is a fact about a caller, not about the API. */
    TEST_ASSERT_TRUE_MESSAGE(
        strstr(g_rec.lines[4], "on_start error:") == g_rec.lines[4],
        "the second onStart threw and was not reported");
    snprintf(notice, sizeof notice, JCE_SCRIPT_DISABLED_NOTICE_FMT,
             "on_start");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(notice, g_rec.lines[5],
        "on_start's disable was not announced: call_start is passing a hook "
        "name the rule does not recognise");
    jce_script_destroy(s);
}

/* ── (7) Compile failures are reported and return 0 ─────────────────────── */

static void test_source_that_does_not_compile_returns_zero(void)
{
    JceScriptHost     host;
    JceScript        *s;
    JceScriptInstance i;

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    rec_reset();
    i = jce_script_instantiate_source(s, "Broken",
                                      "class Broken { this is not java }", 1u);
    TEST_ASSERT_EQUAL_MESSAGE(0, i, "a chunk that does not compile made an "
                                    "instance");
    TEST_ASSERT_TRUE_MESSAGE(rec_saw("compile error"),
        "the compile failure was never reported to the host");
    jce_script_destroy(s);
}

static void test_a_class_that_is_not_a_script_returns_zero(void)
{
    JceScriptHost     host;
    JceScript        *s;
    JceScriptInstance i;

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    rec_reset();
    i = jce_script_instantiate_source(s, "Bystander", k_not_a_script, 1u);
    TEST_ASSERT_EQUAL_MESSAGE(0, i,
        "a class that does not extend JceEntityScript became an instance — "
        "this is Lua's 'the chunk did not return a table'");
    TEST_ASSERT_TRUE(rec_saw("JceEntityScript"));
    jce_script_destroy(s);
}

/* ── (8) compile_module stops at `len` ──────────────────────────────────── */

static void test_compile_module_stops_at_len(void)
{
    JceScriptHost   host;
    JceScript      *s;
    JceScriptModule mod;
    char            buf[1024];
    size_t          good;

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);

    snprintf(buf, sizeof(buf), "%s", k_logger);
    good = strlen(buf);
    /* Bytes the caller did NOT offer.  A backend that ignored `len` and read
     * to the NUL would compile these and fail. */
    snprintf(buf + good, sizeof(buf) - good, "%s",
             "\nthis trailing text is not Java and must never be compiled\n");

    rec_reset();
    mod = jce_script_compile_module(s, "Logger", buf, good);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, mod,
        "compile_module read past `len` — the source buffer may be a slice of "
        "a larger file");
    jce_script_release_module(s, mod);
    jce_script_destroy(s);
}

/* ── (9) A hostless VM works and its bindings are no-ops ────────────────── */

static void test_a_null_host_is_tolerated(void)
{
    JceScript        *s;
    JceScriptInstance i;

    s = jce_script_vm_create("java", NULL, 0u);
    TEST_ASSERT_NOT_NULL_MESSAGE(s,
        "jce_script_create(NULL) is a documented configuration and must make "
        "a VM whose bindings are no-ops");
    rec_reset();
    i = jce_script_instantiate_source(s, "Logger", k_logger, 5u);
    TEST_ASSERT_NOT_EQUAL(0, i);
    jce_script_call_start(s, i);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rec.count,
        "a hostless VM reached a host");
    jce_script_destroy(s);
}

/* ── (10) A SHORT JceScriptHost is not read past its end ───────────────────
 *
 * jce_script_vm.h names this as the backend author's own test, and points at
 * tests/middleware/script/test_jce_script_host_abi.c as the model.  The host
 * is CALLER-allocated and grows; copying it at OUR sizeof reads past the end
 * of a host built against an older header and files the bytes that followed
 * under a callback the VM then invokes.
 *
 * The probe truncates at offsetof(get_position).  What follows is NOT 0xFF,
 * and the reason is a lesson this campaign has now learned five times: with
 * the clamp removed, 0xFF..FF as a function pointer CRASHES THE PROCESS, and a
 * crash reports less than a red test — the named assertion never prints, and
 * neither does anything after it.  The tail is filled instead with a real
 * function that records having been called, so "the VM read past the end of
 * the caller's allocation" is a NAMED failure with a sentence attached.
 * Measured both ways: with 0xFF the mutation kills the run at this test and
 * three later tests never report at all; with the recording poison it is one
 * red line. */
static bool g_poison_ran;

static bool poison_get_position(void *user, JceScriptEntity e, float out[3])
{
    (void)user;
    (void)e;
    (void)out;
    g_poison_ran = true;
    return false;
}

static void test_a_short_host_is_not_read_past_its_end(void)
{
    typedef bool (*GetPositionFn)(void *, JceScriptEntity, float *);
    GetPositionFn     poison = poison_get_position;
    unsigned char     arena[sizeof(JceScriptHost) * 2];
    JceScriptHost     staging;
    JceScript        *s;
    JceScriptInstance i;
    size_t            off;
    const size_t      legacy = offsetof(JceScriptHost, get_position);

    /* Positive control first: with the FULL host the same script says HAVE.
     * Without it, "ABSENT" below would pass for a script that simply cannot
     * call anything. */
    host_fill(&staging);
    s = jce_script_vm_create("java", &staging, sizeof(staging));
    TEST_ASSERT_NOT_NULL(s);
    rec_reset();
    i = jce_script_instantiate_source(s, "Probe", k_probe_position, 7u);
    TEST_ASSERT_NOT_EQUAL(0, i);
    jce_script_call_start(s, i);
    TEST_ASSERT_EQUAL_INT(1, g_rec.count);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("HAVE", g_rec.lines[0],
        "the positive control failed: this script cannot see get_position "
        "even with a full host, so the ABSENT assertion below would be "
        "vacuous");
    TEST_ASSERT_EQUAL_INT(1, g_rec.get_position_calls);
    jce_script_destroy(s);

    /* Now the short host, poisoned past its end with something that TELLS us
     * it ran instead of destroying the process. */
    g_poison_ran = false;
    for (off = 0; off + sizeof(poison) <= sizeof(arena); off += sizeof(poison))
        memcpy(arena + off, &poison, sizeof(poison));
    host_fill(&staging);
    memcpy(arena, &staging, legacy);

    s = jce_script_vm_create("java", (const JceScriptHost *)arena, legacy);
    TEST_ASSERT_NOT_NULL(s);
    rec_reset();
    i = jce_script_instantiate_source(s, "Probe", k_probe_position, 7u);
    TEST_ASSERT_NOT_EQUAL(0, i);
    jce_script_call_start(s, i);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rec.count,
        "the short host's `log` (which IS inside the truncation) did not "
        "work, so this test is measuring the wrong thing");
    TEST_ASSERT_FALSE_MESSAGE(g_poison_ran,
        "the VM called a function pointer that lived PAST the end of the "
        "caller's JceScriptHost. That is not a wrong value, it is control "
        "flow into whatever followed the caller's allocation.");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ABSENT", g_rec.lines[0],
        "get_position was reached through a host that never had it — the "
        "copy read past the end of the caller's allocation");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_rec.get_position_calls,
        "the REAL get_position ran, so this arena is not laid out the way "
        "this test believes and the ABSENT assertion above proves nothing");
    jce_script_destroy(s);
}

/* ── (11) The two halves of Java scripting actually join ───────────────────
 *
 * scripting/java already carried the DOWN direction: com.jce.script.JceScript,
 * 71 generated entries over the C ABI, opened from a raw JceScriptHost
 * pointer.  This backend is the UP direction.  JceScriptSurface claims the
 * join is one call — and a claim with no test is a defect by this repository's
 * own doctrine, so here is the test.
 *
 * The script reaches the engine through the FULL surface (JNI shim -> C ABI ->
 * the same JceScriptHost this VM was created with), and the assertion is on
 * the host's own recording: same host, same member, the arguments the script
 * passed.
 *
 * WHY THE PRELOAD.  On Windows the JVM loads the JNI shim by absolute path,
 * and the shim imports jce_script_api.dll, whose directory is not on this
 * process's search path.  Opening it here first puts it in the process under
 * that module name, and jce_library_open()'s documented Windows behaviour —
 * attach to an already-resident module of the same name — is then exactly what
 * resolves the import.  That behaviour is a hazard when you pass a bare name
 * (see the first test in this file); used deliberately with an absolute path
 * it is the fix. */
static const char *const k_joined =
    "import com.jce.script.vm.JceEntityScript;\n"
    "import com.jce.script.vm.JceScriptSurface;\n"
    "class Joined extends JceEntityScript {\n"
    "  public void onStart() {\n"
    "    JceScriptSurface.of(this).setPosition(entity(), 1.0f, 2.0f, 3.0f);\n"
    "  }\n"
    "}\n";

static void test_the_scripting_surface_reaches_the_same_host(void)
{
    JceScriptHost     host;
    JceScript        *s;
    JceScriptInstance i;
    JceLibrary        api;

    api = jce_library_open(JCE_JAVA_VM_TEST_SCRIPT_API_LIB);
    TEST_ASSERT_NOT_NULL_MESSAGE(api,
        "could not preload " JCE_JAVA_VM_TEST_SCRIPT_API_LIB " — without it "
        "the JNI shim's import cannot resolve and this test would fail for a "
        "reason that has nothing to do with the surface");

    host_fill(&host);
    s = jce_script_vm_create("java", &host, sizeof(host));
    TEST_ASSERT_NOT_NULL(s);
    rec_reset();
    i = jce_script_instantiate_source(s, "Joined", k_joined, 11u);
    TEST_ASSERT_NOT_EQUAL(0, i);
    jce_script_call_start(s, i);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_rec.set_position_calls,
        "the script called the 71-entry scripting surface and the engine's "
        "host never saw it. Both halves are present in this process; the join "
        "is JceScriptSurface.of(this), and it did not arrive.");
    TEST_ASSERT_EQUAL_UINT64(11u, g_rec.last_set_entity);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_rec.last_set_xyz[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_rec.last_set_xyz[1]);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, g_rec.last_set_xyz[2]);

    jce_script_destroy(s);
    jce_library_close(api);
}

/* ── (12) Configuration after the JVM is running is refused ─────────────── */

static void test_configure_after_the_jvm_is_running_is_refused(void)
{
    JceScriptVmJavaConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.jvm_library = TEST_JVM_LIBRARY;
    cfg.class_path = "somewhere/else";
    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_java_configure(&cfg, sizeof(cfg)),
        "configure() was ACCEPTED while the JVM is running. A JVM's options "
        "are fixed at creation, so this would be a setting that reads back "
        "and does nothing.");
}

int main(void)
{
    UNITY_BEGIN();
    /* Order matters — see the file header. */
    RUN_TEST(test_a_bare_jvm_library_name_is_refused);
    RUN_TEST(test_the_backend_claims_java_and_class);
    /* After the bare-library refusal (which needs its own configuration) and
     * before nothing in particular: it starts the JVM like every test below. */
    RUN_TEST(test_a_precompiled_class_runs_through_the_path_slot);
    RUN_TEST(test_the_jvm_starts_and_a_handle_is_created);
    RUN_TEST(test_the_jvm_is_created_exactly_once);
    RUN_TEST(test_a_handle_created_after_another_is_destroyed_still_works);
    RUN_TEST(test_two_handles_do_not_share_script_statics);
    RUN_TEST(test_a_thread_the_jvm_never_saw_cannot_dispatch);
    RUN_TEST(test_a_slot_called_from_another_attached_thread_is_refused);
    RUN_TEST(test_a_throwing_hook_is_reported_disabled_and_leaves_the_rest_alive);
    RUN_TEST(test_source_that_does_not_compile_returns_zero);
    RUN_TEST(test_a_class_that_is_not_a_script_returns_zero);
    RUN_TEST(test_compile_module_stops_at_len);
    RUN_TEST(test_a_null_host_is_tolerated);
    RUN_TEST(test_a_short_host_is_not_read_past_its_end);
    RUN_TEST(test_the_scripting_surface_reaches_the_same_host);
    RUN_TEST(test_configure_after_the_jvm_is_running_is_refused);
    return UNITY_END();
}
