/*
 * test_jce_script_host_abi.c — a short JceScriptHost must not be read past
 * its end (ABI: jce_script_create_sized).
 *
 * JceScriptHost is allocated by the CALLER and is a table of function
 * pointers that grows over releases.  jce_script_create used to do
 * `s->host = *host` — a struct copy at the ENGINE's sizeof.  A game built
 * against an older SDK passes a smaller object, so that copy read whatever
 * followed it in the caller's memory and filed it under the newest member.
 *
 * That is worse than the JceAppDesc case this mirrors.  An over-read there
 * produced a wrong bool; here it produces a FUNCTION POINTER, and the
 * binding calls it the moment a script touches that API.  Every binding
 * guards with `s->host.<member> != NULL`, which poison passes.
 *
 * The tests build exactly that consumer: a host truncated to the offset of
 * the struct's CURRENT last member, with 0xFF after it.  Truncating at the
 * last member rather than a hardcoded one keeps this honest as the struct
 * grows — whatever gets appended next is what the probe withholds.
 *
 * Without the fix the first test does not fail, it CRASHES (0xFF..FF called
 * as a function; verified — exit 139).  That is the intended signal: a
 * segfault in this file means the size-aware copy was removed.
 */

#include "unity.h"

#include <jce/middleware/script/jce_script.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ---- a host whose callbacks record that they ran ---------------------- */

static int  g_log_calls;
/* The last member of JceScriptHost — whichever it currently is.  The
   truncation point below is its offset, so this file keeps testing the
   real tail as the struct grows. */
static bool g_tail_called;

static void h_log(void *user, const char *msg)
{
    (void)user; (void)msg;
    ++g_log_calls;
}

static void h_play_sound_spatial(void *user, const char *path,
				 const float pos[3], float volume,
				 float min_distance, float max_distance,
				 float rolloff)
{
    (void)user;
    (void)path;
    (void)pos;
    (void)volume;
    (void)min_distance;
    (void)max_distance;
    (void)rolloff;
    g_tail_called = true;
}

void setUp(void)    { g_log_calls = 0; g_tail_called = false; }
void tearDown(void) {}

/* Run `body` in a fresh instance on `vm`.  Returns the instance (0 = the
   chunk failed to compile or run). */
static JceScriptInstance run(JceScript *vm, const char *body)
{
    char src[512];
    snprintf(src, sizeof src,
             "local M = {}\n%s\nfunction M:on_start() end\nreturn M\n", body);
    return jce_script_instantiate_source(vm, "host_abi_probe", src, 0);
}

/* ------------------------------------------------------------------ *
 *  The whole point: a legacy-sized host is safe to use.
 * ------------------------------------------------------------------ */

static void test_short_host_is_not_read_past_its_end(void)
{
    /* Where the struct ends just before its current last member. */
    const size_t legacy_size = offsetof(JceScriptHost, play_sound_spatial);
    TEST_ASSERT_TRUE_MESSAGE(legacy_size < sizeof(JceScriptHost),
        "play_sound_spatial is no longer past the truncation point — pick the "
        "current last member of JceScriptHost");

    /* Poisoned arena: legacy-sized host, then 0xFF.  An over-read lands a
       non-NULL garbage pointer in that member, which the binding calls. */
    unsigned char arena[sizeof(JceScriptHost) * 2];
    memset(arena, 0xFF, sizeof(arena));

    JceScriptHost staging;
    memset(&staging, 0, sizeof staging);
    staging.log = h_log;
    staging.play_sound_spatial = h_play_sound_spatial; /* NOT copied over */
    memcpy(arena, &staging, legacy_size);

    JceScript *vm = jce_script_create_sized((const JceScriptHost *)arena,
                                            legacy_size);
    TEST_ASSERT_NOT_NULL(vm);

    /* The host we were handed does not reach that member, so the binding
       must treat it as absent rather than call anything. */
    JceScriptInstance inst = run(
        vm, "jce.play_sound('thunder.wav', 1, 2, 3, 1, 8, 1200, 0.3)");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, inst,
        "probe chunk failed to run — the assertion below would be vacuous");
    TEST_ASSERT_FALSE_MESSAGE(g_tail_called,
        "a callback past the end of the caller's host was invoked");

    jce_script_destroy(vm);
}

/* The truncation must not cost the members the caller DID supply — a fix
   that simply ignored short hosts would also pass the test above. */
static void test_short_host_keeps_the_members_it_did_supply(void)
{
    const size_t legacy_size = offsetof(JceScriptHost, play_sound_spatial);

    unsigned char arena[sizeof(JceScriptHost) * 2];
    memset(arena, 0xFF, sizeof(arena));

    JceScriptHost staging;
    memset(&staging, 0, sizeof staging);
    staging.log = h_log;
    memcpy(arena, &staging, legacy_size);

    JceScript *vm = jce_script_create_sized((const JceScriptHost *)arena,
                                            legacy_size);
    TEST_ASSERT_NOT_NULL(vm);

    JceScriptInstance inst = run(vm, "jce.log('from a legacy host')");
    TEST_ASSERT_NOT_EQUAL(0, inst);
    TEST_ASSERT_TRUE_MESSAGE(g_log_calls > 0,
        "log() was dropped — the short-host path lost a member the caller "
        "actually provided");

    jce_script_destroy(vm);
}

/* A host LONGER than this engine understands (caller newer than the engine)
   is the mirror case: copy what we know, ignore the tail, do not fail. */
static void test_longer_host_is_accepted_and_truncated(void)
{
    unsigned char arena[sizeof(JceScriptHost) * 2];
    memset(arena, 0xFF, sizeof(arena));

    JceScriptHost staging;
    memset(&staging, 0, sizeof staging);
    staging.log = h_log;
    memcpy(arena, &staging, sizeof staging);

    JceScript *vm = jce_script_create_sized((const JceScriptHost *)arena,
                                            sizeof(arena));
    TEST_ASSERT_NOT_NULL(vm);

    JceScriptInstance inst = run(vm, "jce.log('from a newer host')");
    TEST_ASSERT_NOT_EQUAL(0, inst);
    TEST_ASSERT_TRUE(g_log_calls > 0);

    jce_script_destroy(vm);
}

/* The legacy entry point must still work — it is a real exported symbol for
   already-linked binaries, and the header's macro shim must not break the
   ordinary in-tree call. */
static void test_full_host_through_the_legacy_entry_point(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = h_log;
    host.play_sound_spatial = h_play_sound_spatial;

    JceScript *vm = jce_script_create(&host);
    TEST_ASSERT_NOT_NULL(vm);

    JceScriptInstance inst = run(
        vm, "jce.play_sound('thunder.wav', 1, 2, 3, 1, 8, 1200, 0.3)");
    TEST_ASSERT_NOT_EQUAL(0, inst);
    TEST_ASSERT_TRUE_MESSAGE(g_tail_called,
        "a fully-sized host lost its last member — the size clamp is too "
        "eager");

    jce_script_destroy(vm);
}

/* Degenerate sizes must not crash or half-copy. */
static void test_zero_size_host_is_treated_as_absent(void)
{
    JceScriptHost host;
    memset(&host, 0, sizeof host);
    host.log = h_log;

    JceScript *vm = jce_script_create_sized(&host, 0);
    TEST_ASSERT_NOT_NULL(vm);

    JceScriptInstance inst = run(vm, "jce.log('ignored')");
    TEST_ASSERT_NOT_EQUAL(0, inst);
    TEST_ASSERT_EQUAL_MESSAGE(0, g_log_calls,
        "a zero-sized host still had its callbacks used");

    jce_script_destroy(vm);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_short_host_is_not_read_past_its_end);
    RUN_TEST(test_short_host_keeps_the_members_it_did_supply);
    RUN_TEST(test_longer_host_is_accepted_and_truncated);
    RUN_TEST(test_full_host_through_the_legacy_entry_point);
    RUN_TEST(test_zero_size_host_is_treated_as_absent);
    return UNITY_END();
}
