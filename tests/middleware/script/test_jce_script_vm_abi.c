/*
 * test_jce_script_vm_abi.c — a short JceScriptVM must not be read past its
 * end, and a handle that does not begin with a JceScriptVMHeader must not be
 * dispatched through.
 *
 * Modelled on test_jce_script_host_abi.c, in the OPPOSITE DIRECTION and with
 * a worse payload.  There, the CALLER allocates JceScriptHost and the engine
 * copies it; the fix is a size-clamped copy and an over-read produces a host
 * callback the binding invokes.  Here the PLUGIN allocates JceScriptVM and
 * the engine reads it — and every member past `language` is a function
 * pointer the ENGINE ITSELF calls, on the engine's own thread, in the frame
 * loop.  An over-read here is not a wrong value; it is control flow.
 *
 * The probes below build exactly that plugin: a vtable truncated at the
 * offset of the struct's CURRENT last member, with 0xFF after it.
 * Truncating at the last member rather than a hardcoded one keeps this
 * honest as the struct grows — whatever gets appended next is what the probe
 * withholds.
 *
 * WHAT THIS FILE FORBIDS, precisely:
 *
 *   1. Copying a registered vtable at the ENGINE's sizeof.  A plugin built
 *      against an older header is shorter; copying at our size files the
 *      bytes that follow it under a slot we will later call.  The clamp turns
 *      that into a NULL slot, and (2) turns a NULL slot into a refusal.
 *
 *   2. Accepting a registration with ANY slot NULL.  call_named_num and
 *      call_named_str are how UISlider / UIToggle / UIDropdown /
 *      UIInputField handlers dispatch, and both return false for "no such
 *      global" — which is exactly what a correctly absent handler returns.
 *      A VM that omitted them would break every UI callback in the game and
 *      report nothing, anywhere, ever.  Refusing the whole registration is
 *      the only signal that cannot be mistaken for normal operation.
 *
 *   3. Dispatching through a handle whose first bytes are not a
 *      JceScriptVMHeader naming the implementation we called.  Every
 *      forwarder reads the vtable from offset 0 of the handle.
 *
 *   4. Using a handle from a thread other than the one that created it.
 *
 * Note on failure modes: with the clamp REMOVED, probe (1) does not fail, it
 * SUCCEEDS at registration and then the test's own call crashes on
 * 0xFF..FF-as-a-function.  So the first assertion is on the registration
 * result, which is reachable and clean; the call comes after it and is
 * guarded.  A segfault in this file means the size-aware copy was removed.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script_vm.h>
#include <jce/os/core/jce_thread.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- a probe implementation whose slots record that they ran ---------- */

/* The handle every probe VM returns.  JceScriptVMHeader FIRST — that is the
 * rule this file also tests the enforcement of, in
 * test_handle_without_the_header_is_refused. */
typedef struct ProbeScript {
    JceScriptVMHeader hdr;
    int               marker;
} ProbeScript;

static ProbeScript g_probe_handle;
static ProbeScript g_headerless_handle;   /* hdr.vm deliberately left wrong */

static int  g_created;
static int  g_destroyed;
static int  g_start_calls;
static int  g_named_num_calls;
static int  g_named_str_calls;
/* The slot at the struct's CURRENT last member — whichever it is.  The
   truncation point below is its offset, so this file keeps testing the real
   tail as JceScriptVM grows. */
static int  g_tail_calls;

static const JceScriptVM *g_probe_vm_self;   /* set per test before create */

static JceScript *probe_create_sized(const JceScriptHost *host,
                                     size_t host_size)
{
    (void)host; (void)host_size;
    ++g_created;
    memset(&g_probe_handle, 0, sizeof g_probe_handle);
    g_probe_handle.hdr.vm = g_probe_vm_self;
    g_probe_handle.marker = 0x5A;
    return (JceScript *)&g_probe_handle;
}

/* A create that forgets rule 1: the handle's first word is not our table. */
static JceScript *probe_create_headerless(const JceScriptHost *host,
                                          size_t host_size)
{
    (void)host; (void)host_size;
    ++g_created;
    memset(&g_headerless_handle, 0, sizeof g_headerless_handle);
    /* Something plausible and wrong — e.g. the struct really starts with the
       runtime's own module pointer. */
    g_headerless_handle.hdr.vm = (const JceScriptVM *)(void *)&g_created;
    return (JceScript *)&g_headerless_handle;
}

static void probe_destroy(JceScript *s) { (void)s; ++g_destroyed; }

static JceScriptInstance probe_instantiate(JceScript *s, const char *p,
                                           JceScriptEntity o)
{ (void)s; (void)p; (void)o; return 1u; }

static JceScriptInstance probe_instantiate_source(JceScript *s, const char *n,
                                                  const char *src,
                                                  JceScriptEntity o)
{ (void)s; (void)n; (void)src; (void)o; return 1u; }

static void probe_call_start(JceScript *s, JceScriptInstance i)
{ (void)s; (void)i; ++g_start_calls; }

static void probe_call_update(JceScript *s, JceScriptInstance i, float dt)
{ (void)s; (void)i; (void)dt; }

static void probe_release(JceScript *s, JceScriptInstance i)
{ (void)s; (void)i; }

static void probe_call_collision(JceScript *s, JceScriptInstance i,
                                 JceScriptEntity o)
{ (void)s; (void)i; (void)o; }

static void probe_call_message(JceScript *s, JceScriptInstance i,
                               const char *m, double n, const char *str)
{ (void)s; (void)i; (void)m; (void)n; (void)str; }

static void probe_call_anim_event(JceScript *s, JceScriptInstance i,
                                  uint32_t id, const char *n,
                                  float f0, float f1, int i0)
{ (void)s; (void)i; (void)id; (void)n; (void)f0; (void)f1; (void)i0; }

static bool probe_call_named(JceScript *s, const char *f, JceScriptEntity e)
{ (void)s; (void)f; (void)e; return true; }

static bool probe_call_named_num(JceScript *s, const char *f,
                                 JceScriptEntity e, double v)
{ (void)s; (void)f; (void)e; (void)v; ++g_named_num_calls; return true; }

static bool probe_call_named_str(JceScript *s, const char *f,
                                 JceScriptEntity e, const char *str)
{ (void)s; (void)f; (void)e; (void)str; ++g_named_str_calls; return true; }

static int probe_instance_count(const JceScript *s) { (void)s; return 7; }

static void probe_update_coroutines(JceScript *s, float dt)
{ (void)s; (void)dt; }

static JceScriptModule probe_compile_module(JceScript *s, const char *n,
                                            const char *src, size_t len)
{ (void)s; (void)n; (void)src; (void)len; return 3u; }

static void probe_rebind_instance(JceScript *s, JceScriptInstance i,
                                  JceScriptModule m)
{ (void)s; (void)i; (void)m; }

static void probe_release_module(JceScript *s, JceScriptModule m)
{ (void)s; (void)m; }

/* THE CURRENT LAST MEMBER of JceScriptVM.  If a slot is appended after
   call_fixed_update, this function and the truncation point below both move
   to the new tail — the guard in the first test says so out loud.  It moved
   here from release_module on 2026-09-20, when on_fixed_update was appended,
   which is the migration that comment was written to make obvious. */
static void probe_call_fixed_update(JceScript *s, JceScriptInstance i, float dt)
{ (void)s; (void)i; (void)dt; ++g_tail_calls; }

/* A fully populated probe table.  By name, not positionally: this file has
   to keep compiling when a slot is APPENDED, so that the truncation probe
   above still has a real tail to withhold. */
static void probe_fill(JceScriptVM *vm, const char *language)
{
    memset(vm, 0, sizeof *vm);
    vm->struct_size        = sizeof(JceScriptVM);
    vm->language           = language;
    vm->create_sized       = probe_create_sized;
    vm->destroy            = probe_destroy;
    vm->instantiate        = probe_instantiate;
    vm->instantiate_source = probe_instantiate_source;
    vm->call_start         = probe_call_start;
    vm->call_update        = probe_call_update;
    vm->release            = probe_release;
    vm->call_collision     = probe_call_collision;
    vm->call_message       = probe_call_message;
    vm->call_anim_event    = probe_call_anim_event;
    vm->call_named         = probe_call_named;
    vm->call_named_num     = probe_call_named_num;
    vm->call_named_str     = probe_call_named_str;
    vm->instance_count     = probe_instance_count;
    vm->update_coroutines  = probe_update_coroutines;
    vm->compile_module     = probe_compile_module;
    vm->rebind_instance    = probe_rebind_instance;
    vm->release_module     = probe_release_module;
    vm->call_fixed_update  = probe_call_fixed_update;
}

/* Where the struct ends just before its current last member. */
static size_t truncation_point(void)
{
    return offsetof(JceScriptVM, call_fixed_update);
}

void setUp(void)
{
    g_created = g_destroyed = 0;
    g_start_calls = g_named_num_calls = g_named_str_calls = g_tail_calls = 0;
    g_probe_vm_self = NULL;
}
void tearDown(void) {}

/* ------------------------------------------------------------------ *
 *  (1) + (2): a short vtable is not read past its end, and the hole
 *  that leaves is refused rather than called.
 * ------------------------------------------------------------------ */

static void test_short_vm_is_not_read_past_its_end(void)
{
    const size_t legacy_size = truncation_point();
    unsigned char arena[sizeof(JceScriptVM) * 2];
    JceScriptVM   staging;
    bool          ok;

    TEST_ASSERT_TRUE_MESSAGE(legacy_size < sizeof(JceScriptVM),
        "release_module is no longer past the truncation point — pick the "
        "current last member of JceScriptVM (and move probe_release_module "
        "with it)");

    /* Poisoned arena: legacy-sized table, then 0xFF.  An over-read lands a
       non-NULL garbage pointer in release_module, which every
       jce_script_release_module() would then call. */
    memset(arena, 0xFF, sizeof arena);
    probe_fill(&staging, "probe_short");
    staging.struct_size = legacy_size;   /* what an older plugin would claim */
    memcpy(arena, &staging, legacy_size);

    ok = jce_script_vm_register((const JceScriptVM *)arena);

    /* THE assertion.  With the clamp removed this returns TRUE, because the
       0xFF bytes read as a perfectly non-NULL release_module. */
    TEST_ASSERT_FALSE_MESSAGE(ok,
        "a vtable truncated before its last member was ACCEPTED — the engine "
        "read past struct_size and took the bytes that followed as a function "
        "pointer it will call");

    /* And nothing may have been created or called along the way. */
    TEST_ASSERT_EQUAL_MESSAGE(0, g_created,
        "a refused registration still produced a VM");
    TEST_ASSERT_EQUAL_MESSAGE(0, g_tail_calls,
        "the withheld slot was invoked");
}

/* A refusal must not be a blanket "short tables are bad": the members the
   plugin DID supply have to be the reason it was refused, named.  This probe
   withholds the tail with a REAL function pointer sitting in the poisoned
   bytes rather than 0xFF, so a broken clamp copies something callable and
   plausible instead of something obviously wrong — and the registration
   still has to fail. */
static void test_short_vm_tail_is_absent_even_when_the_bytes_are_valid(void)
{
    const size_t legacy_size = truncation_point();
    unsigned char arena[sizeof(JceScriptVM) * 2];
    JceScriptVM   staging;

    memset(arena, 0, sizeof arena);
    probe_fill(&staging, "probe_short_valid");
    /* Full table in the arena — including a genuine release_module — but
       struct_size says the plugin stops before it. */
    memcpy(arena, &staging, sizeof staging);
    ((JceScriptVM *)(void *)arena)->struct_size = legacy_size;

    TEST_ASSERT_FALSE_MESSAGE(
        jce_script_vm_register((const JceScriptVM *)arena),
        "the engine used a slot beyond the plugin's declared struct_size — "
        "the bytes were readable and valid, which is exactly the case a "
        "sizeof-based copy gets wrong without ever crashing");
}

/* ------------------------------------------------------------------ *
 *  (2): the two slots whose absence is invisible.
 * ------------------------------------------------------------------ */

static void test_vm_missing_a_silent_slot_is_refused(void)
{
    JceScriptVM vm;

    probe_fill(&vm, "probe_no_named_num");
    vm.call_named_num = NULL;
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a VM with no call_named_num was accepted — every UISlider, UIToggle "
        "and UIDropdown handler would return false, which is what a correctly "
        "absent handler returns, so nothing would ever report it");

    probe_fill(&vm, "probe_no_named_str");
    vm.call_named_str = NULL;
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a VM with no call_named_str was accepted — every UIInputField "
        "on_value_changed / on_submit would silently do nothing");

    /* Not special-cased: EVERY slot is required. */
    probe_fill(&vm, "probe_no_release_module");
    vm.release_module = NULL;
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a VM with a NULL slot was accepted");

    probe_fill(&vm, "probe_no_create");
    vm.create_sized = NULL;
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a VM with no create_sized was accepted");
}

/* ------------------------------------------------------------------ *
 *  The mirror case: a plugin NEWER than the engine.
 * ------------------------------------------------------------------ */

static void test_longer_vm_is_accepted_and_truncated(void)
{
    static unsigned char arena[sizeof(JceScriptVM) * 2];
    JceScriptVM          staging;
    const JceScriptVM   *got;

    memset(arena, 0xFF, sizeof arena);      /* the "future" slots are poison */
    probe_fill(&staging, "probe_future");
    staging.struct_size = sizeof(arena);    /* a plugin from a later header */
    memcpy(arena, &staging, sizeof staging);

    TEST_ASSERT_TRUE_MESSAGE(
        jce_script_vm_register((const JceScriptVM *)arena),
        "a vtable LONGER than this engine's was refused — a plugin built "
        "against a newer header must keep working, with its tail ignored");

    got = jce_script_vm_find("probe_future");
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_MESSAGE(sizeof(JceScriptVM), got->struct_size,
        "the registry recorded the plugin's claimed size rather than what it "
        "actually copied — a later reader would believe in members that were "
        "never copied");
}

/* Degenerate sizes must be refused before anything past struct_size is read. */
static void test_absurd_struct_size_is_refused(void)
{
    JceScriptVM vm;

    probe_fill(&vm, "probe_zero_size");
    vm.struct_size = 0;
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a zero-sized vtable was accepted");

    probe_fill(&vm, "probe_tiny_size");
    vm.struct_size = sizeof(size_t);   /* struct_size and nothing else */
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a vtable too short to hold create_sized was accepted — reading "
        "`language` out of it is already past its end");

    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(NULL),
        "a NULL vtable was accepted");
}

static void test_duplicate_language_is_refused(void)
{
    /* static: the registry records this address as the entry's `origin`,
       and a stack table would dangle the moment the test returns. */
    static JceScriptVM vm;

    probe_fill(&vm, "probe_dup");
    TEST_ASSERT_TRUE(jce_script_vm_register(&vm));

    probe_fill(&vm, "probe_dup");
    TEST_ASSERT_FALSE_MESSAGE(jce_script_vm_register(&vm),
        "a second VM registered the same language name — live handles hold a "
        "pointer into the registry, so a replacement repoints them");
}

/* ------------------------------------------------------------------ *
 *  (3): the handle must begin with the header.
 * ------------------------------------------------------------------ */

static void test_handle_without_the_header_is_refused(void)
{
    /* static: the registry records this address as the entry's `origin`,
       and a stack table would dangle the moment the test returns. */
    static JceScriptVM vm;
    JceScript  *s;

    probe_fill(&vm, "probe_headerless");
    vm.create_sized = probe_create_headerless;
    TEST_ASSERT_TRUE(jce_script_vm_register(&vm));

    s = jce_script_vm_create("probe_headerless", NULL, 0);
    TEST_ASSERT_NULL_MESSAGE(s,
        "a handle whose first word is not this VM's table was accepted — "
        "every forwarder reads the vtable from there, so the next dispatch "
        "is a call through whatever the implementation's struct starts with");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_created, "create_sized was not reached");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_destroyed,
        "the refused handle was leaked instead of being destroyed");
}

/* The good case, and the proof that the forwarders reach the SLOTS — a
   forwarder that quietly kept calling Lua would pass every test above. */
static void test_registered_vm_receives_every_dispatch(void)
{
    /* static: the registry records this address as the entry's `origin`,
       and a stack table would dangle the moment the test returns. */
    static JceScriptVM vm;
    JceScript  *s;

    probe_fill(&vm, "probe_live");
    TEST_ASSERT_TRUE(jce_script_vm_register(&vm));
    g_probe_vm_self = &vm;              /* what create_sized writes to hdr.vm */

    s = jce_script_vm_create("probe_live", NULL, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, "the probe VM produced no handle");
    TEST_ASSERT_EQUAL_STRING("probe_live", jce_script_vm_language_of(s));

    jce_script_call_start(s, 1u);
    TEST_ASSERT_EQUAL_MESSAGE(1, g_start_calls,
        "jce_script_call_start did not reach the registered VM's slot");

    /* The two that fail silently. */
    TEST_ASSERT_TRUE(jce_script_call_named_num(s, "h", 1u, 2.0));
    TEST_ASSERT_EQUAL_MESSAGE(1, g_named_num_calls,
        "jce_script_call_named_num did not reach the registered VM's slot");
    TEST_ASSERT_TRUE(jce_script_call_named_str(s, "h", 1u, "x"));
    TEST_ASSERT_EQUAL_MESSAGE(1, g_named_str_calls,
        "jce_script_call_named_str did not reach the registered VM's slot");

    /* The tail slot, through the public entry point. */
    jce_script_call_fixed_update(s, 1u, 0.0125f);
    TEST_ASSERT_EQUAL_MESSAGE(1, g_tail_calls,
        "jce_script_call_fixed_update did not reach the registered VM's slot");

    TEST_ASSERT_EQUAL(7, jce_script_instance_count(s));

    jce_script_destroy(s);
    TEST_ASSERT_EQUAL(1, g_destroyed);
}

/* ------------------------------------------------------------------ *
 *  Lua is the first implementation, and the built-in registration is
 *  the only one the engine performs for itself.
 * ------------------------------------------------------------------ */

static void test_lua_is_registered_and_is_what_jce_script_create_makes(void)
{
    JceScriptHost host;
    JceScript    *s;

    TEST_ASSERT_NOT_NULL_MESSAGE(jce_script_vm_find("lua"),
        "the built-in Lua VM is not registered — jce_script_create would "
        "return NULL and every existing consumer would silently lose "
        "scripting");

    memset(&host, 0, sizeof host);
    s = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("lua", jce_script_vm_language_of(s),
        "jce_script_create no longer produces a Lua VM");
    jce_script_destroy(s);
}

/* jce_script_create is deliberately NOT a JceScriptVM slot; it is the
   size-less spelling that must always reach create_sized with THIS engine's
   sizeof(JceScriptHost).  If it were a slot, a backend could supply it and
   not create_sized, losing the short-host clamp that
   test_jce_script_host_abi.c exists to guarantee. */
static size_t g_seen_host_size;

static JceScript *probe_create_recording_size(const JceScriptHost *host,
                                              size_t host_size)
{
    g_seen_host_size = host_size;
    return probe_create_sized(host, host_size);
}

static void test_host_size_reaches_create_sized_unchanged(void)
{
    /* static: the registry records this address as the entry's `origin`,
       and a stack table would dangle the moment the test returns. */
    static JceScriptVM   vm;
    JceScriptHost host;
    JceScript    *s;

    probe_fill(&vm, "probe_size");
    vm.create_sized = probe_create_recording_size;
    TEST_ASSERT_TRUE(jce_script_vm_register(&vm));
    g_probe_vm_self  = &vm;
    g_seen_host_size = (size_t)-1;

    memset(&host, 0, sizeof host);
    s = jce_script_vm_create("probe_size", &host, sizeof host);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_MESSAGE(sizeof(JceScriptHost), g_seen_host_size,
        "the host size did not reach create_sized unchanged");
    jce_script_destroy(s);
}

/* ------------------------------------------------------------------ *
 *  (4): the owning-thread rule, checked in RELEASE.
 *
 *  Deliberately NOT phrased as "the main thread": jce_thread_is_main()
 *  returns true when jce_thread_mark_main() was never called, and no unit
 *  test process ever calls it, so a main-thread assertion would be
 *  vacuously true exactly here.  jce_thread_current_id() needs no marking.
 * ------------------------------------------------------------------ */

typedef struct {
    JceScript *s;
    int        start_calls_seen;
} WorkerArg;

static void worker_calls_start(void *user)
{
    WorkerArg *a = (WorkerArg *)user;
    jce_script_call_start(a->s, 1u);
    a->start_calls_seen = g_start_calls;
}

static void test_worker_thread_is_refused(void)
{
    /* static: the registry records this address as the entry's `origin`,
       and a stack table would dangle the moment the test returns. */
    static JceScriptVM vm;
    JceScript  *s;
    WorkerArg   arg;
    JceThread  *t;

    probe_fill(&vm, "probe_thread");
    TEST_ASSERT_TRUE(jce_script_vm_register(&vm));
    g_probe_vm_self = &vm;

    s = jce_script_vm_create("probe_thread", NULL, 0);
    TEST_ASSERT_NOT_NULL(s);

    /* Same thread: allowed, and the baseline for the comparison below.  A
       check that refused BOTH threads would otherwise look identical. */
    jce_script_call_start(s, 1u);
    TEST_ASSERT_EQUAL_MESSAGE(1, g_start_calls,
        "the owning thread was refused — the check is not a thread check");

    arg.s = s;
    arg.start_calls_seen = -1;
    t = jce_thread_create(worker_calls_start, &arg, "vm_abi_worker");
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "could not start the worker thread");
    jce_thread_join(t);

    TEST_ASSERT_EQUAL_MESSAGE(1, arg.start_calls_seen,
        "a dispatch from a thread that does not own the handle reached the "
        "VM — the owning-thread check is absent or was compiled out");
    TEST_ASSERT_EQUAL_MESSAGE(1, g_start_calls,
        "the worker's dispatch reached the slot");

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_vm_is_not_read_past_its_end);
    RUN_TEST(test_short_vm_tail_is_absent_even_when_the_bytes_are_valid);
    RUN_TEST(test_vm_missing_a_silent_slot_is_refused);
    RUN_TEST(test_longer_vm_is_accepted_and_truncated);
    RUN_TEST(test_absurd_struct_size_is_refused);
    RUN_TEST(test_duplicate_language_is_refused);
    RUN_TEST(test_handle_without_the_header_is_refused);
    RUN_TEST(test_registered_vm_receives_every_dispatch);
    RUN_TEST(test_lua_is_registered_and_is_what_jce_script_create_makes);
    RUN_TEST(test_host_size_reaches_create_sized_unchanged);
    RUN_TEST(test_worker_thread_is_refused);
    return UNITY_END();
}
