/* test_jce_script_disable.c — THE FAILING-CALLBACK RULE, on the reference VM.
 *
 * jce_script.h promises that a fixed-name lifecycle callback which raises is
 * "caught, logged, and disabled on that instance".  For a long time the header
 * said it and nothing did it: call_method() logged the pcall failure and
 * returned, so the next frame called the same broken handler again, and a
 * script erroring at 60 Hz wrote sixty log lines a second.  This file is the
 * enforcement that was missing, on the Lua VM — which is the implementation
 * the Python, Java and C++ backends are written against, so what is asserted
 * here is what their three lifecycle differentials then compare against.
 *
 * WHAT EACH CASE IS DEFENDING AGAINST.  A disable is a SUPPRESSION, and every
 * cheap way to test a suppression can be passed by suppressing too much:
 *
 *   - "the handler stopped running" is passed by an implementation that
 *     dropped the whole instance.  So every case that asserts silence also
 *     asserts that a DIFFERENT callback on the SAME instance still runs.
 *   - "it stopped running" is also passed by one that never ran it.  So every
 *     case first asserts the handler ran and produced its effect BEFORE the
 *     error, and counts the dispatches that landed.
 *   - "an error was logged" is passed by log-and-continue, which is the defect.
 *     So the notice line is asserted BY ITS TEXT, and the text comes from
 *     JCE_SCRIPT_DISABLED_NOTICE_FMT in the public header rather than being
 *     spelled here: if the header's wording and the VM's output ever diverge,
 *     this file stops compiling into agreement and the case fails by name.
 *
 * Observation is through the host's `log` callback and through Lua globals the
 * handlers write, read back by a follow-up assert-chunk on the same VM (a
 * tripped assert blows that chunk up and yields instance 0, so a non-zero
 * instance proves every assertion in it held) — the idiom the rest of this
 * directory uses.
 */

#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* ── Mock host: records every log line ──────────────────────────────────── */

#define MAX_LOGS 64

typedef struct {
    int  count;
    char line[MAX_LOGS][512];
} LogRecorder;

static LogRecorder g_log;

static void mock_log(void *user, const char *msg)
{
    LogRecorder *r = (LogRecorder *)user;
    if (!r || r->count >= MAX_LOGS) return;
    snprintf(r->line[r->count], sizeof r->line[0], "%s", msg ? msg : "");
    r->count++;
}

static void host_reset(JceScriptHost *h)
{
    memset(&g_log, 0, sizeof g_log);
    memset(h, 0, sizeof *h);
    h->user = &g_log;
    h->log  = mock_log;
}

void setUp(void)    { memset(&g_log, 0, sizeof g_log); }
void tearDown(void) {}

static int log_count_with(const char *needle)
{
    int i, n = 0;
    for (i = 0; i < g_log.count; ++i)
        if (strstr(g_log.line[i], needle)) ++n;
    return n;
}

/* The notice the header REQUIRES, rendered for one handler.  Built from the
 * public macro, so a change to the wording in jce_script.h that the VM did not
 * pick up (or vice versa) fails here instead of drifting. */
static const char *notice_for(const char *handler)
{
    static char buf[512];
    snprintf(buf, sizeof buf, JCE_SCRIPT_DISABLED_NOTICE_FMT, handler);
    return buf;
}

/* ── Scripts ────────────────────────────────────────────────────────────── */

/* on_update raises from its third call onward, and EVERY handler bumps its own
 * global counter BEFORE it can raise — so "it ran" and "it raised" are two
 * separate observations rather than one. */
static const char *SRC =
    "U = 0; C = 0; S = 0; A = 0; D = 0; P = 0\n"
    "local M = {}\n"
    "function M:on_start()  S = S + 1 end\n"
    "function M:on_update(dt) U = U + 1; if U >= 3 then error('boom') end end\n"
    "function M:on_collision(other) C = C + 1; error('crunch') end\n"
    "function M:on_anim_event(id) A = A + 1; error('anim') end\n"
    "function M:on_destroy() D = D + 1 end\n"
    "function M:ping(n, s) P = P + 1; error('ping') end\n"
    "return M\n";

/* The hot-reload target: a DIFFERENT counter (V) proves the new methods are
 * live, and it does not raise, so a re-enabled handler is visibly working
 * rather than merely re-armed. */
static const char *SRC_V2 =
    "V = 0\n"
    "local M = {}\n"
    "function M:on_update(dt) V = V + 1 end\n"
    "function M:on_collision(other) V = V + 100 end\n"
    "return M\n";

/* on_start raises from its SECOND dispatch onward.  jce_script_call_start is
 * a public entry point with no at-most-once contract — this engine's runtime
 * happens to call it once per instantiate, but that is a fact about a caller,
 * not about the API, and the rule participates on the API's terms. */
static const char *SRC_START =
    "SS = 0\n"
    "local M = {}\n"
    "function M:on_start() SS = SS + 1; if SS >= 2 then error('start') end end\n"
    "function M:on_update(dt) SS = SS + 100 end\n"
    "return M\n";

/* Run `chunk` as an assert-chunk on `s`; non-zero == every assert held. */
static JceScriptInstance check(JceScript *s, const char *chunk)
{
    return jce_script_instantiate_source(s, "@check", chunk, 99);
}

/* ── 1. on_update stops after it raises; the error and the notice are both
 *      logged, once each, to the host ─────────────────────────────────── */
static void test_a_raising_on_update_is_disabled_and_says_so(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 10; ++i) jce_script_call_update(s, inst, 0.016f);

    /* Ten dispatches, three that reached the handler: two clean, one that
     * raised.  A VM that never ran on_update at all would also log nothing,
     * which is why the count is asserted and not just the silence. */
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(U == 3, 'on_update ran ' .. tostring(U) .. ' times, not 3: "
        "either the disable did not take (log spam is back) or the handler "
        "never ran before it')\n"
        "return {}\n"), "see the assert message");

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count_with("on_update error:"),
        "the raise must be reported through host.log exactly once — more means "
        "the disable did not take, zero means the error was swallowed");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count_with(notice_for("on_update")),
        "the disable must announce itself in the words jce_script.h publishes: "
        "a callback that goes quiet with no notice is indistinguishable from "
        "one the script never declared");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_log.count,
        "exactly two host lines for one broken handler over ten frames");

    jce_script_destroy(s);
}

/* ── 2. it is THAT callback on THAT instance — not the instance, not the
 *      callback everywhere ────────────────────────────────────────────── */
static void test_the_disable_is_per_callback_and_per_instance(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance a, b;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    a = jce_script_instantiate_source(s, "@a", SRC, 1);
    b = jce_script_instantiate_source(s, "@b", SRC, 2);
    TEST_ASSERT_NOT_EQUAL(0, a);
    TEST_ASSERT_NOT_EQUAL(0, b);

    /* Break on_update on A only.  (The two instances run separate chunks, so
     * they have separate globals only if the VM shares _G — it does, which is
     * why B's own counters are read from a per-instance field instead.) */
    for (i = 0; i < 5; ++i) jce_script_call_update(s, a, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_update")));

    /* Same instance, DIFFERENT callback: on_start must still dispatch.  An
     * implementation that dropped the instance, or that used one flag instead
     * of one per callback, fails here and nowhere else. */
    jce_script_call_start(s, a);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(S == 1, 'on_start did not run on an instance whose on_update "
        "was disabled: the disable is too coarse — it took the instance, or a "
        "single flag stands for every callback')\n"
        "return {}\n"), "see the assert message");

    /* Different instance, SAME callback: B's on_update must still dispatch.
     * U is shared through _G, so B's dispatch shows up as U advancing past
     * the 3 that A left it at — and as a SECOND error+notice pair. */
    jce_script_call_update(s, b, 0.016f);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(U == 4, 'the second instance did not dispatch on_update (U == "
        "' .. tostring(U) .. '): the disable is stored per CALLBACK NAME "
        "instead of per instance')\n"
        "return {}\n"), "see the assert message");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, log_count_with(notice_for("on_update")),
        "the second instance must be disabled on its own account");

    jce_script_destroy(s);
}

/* ── 3. on_collision and on_anim_event participate too ──────────────────── */
static void test_collision_and_anim_event_participate(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    /* A body resting against a wall re-fires on_collision as readily as the
     * frame clock re-fires on_update; that is why these two are in the rule. */
    for (i = 0; i < 6; ++i) jce_script_call_collision(s, inst, 7);
    for (i = 0; i < 6; ++i)
        jce_script_call_anim_event(s, inst, 1u, "step", 0.0f, 0.0f, 0);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(C == 1, 'on_collision ran ' .. tostring(C) .. ' times, not 1')\n"
        "assert(A == 1, 'on_anim_event ran ' .. tostring(A) .. ' times, not 1')\n"
        "return {}\n"), "see the assert message");
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_collision")));
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_anim_event")));

    jce_script_destroy(s);
}

/* ── 4. call_message does NOT participate, and the reason is not "we forgot" */
static void test_a_message_handler_is_never_disabled(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 4; ++i) jce_script_call_message(s, inst, "ping", 1.0, NULL);

    /* PINNED, not an oversight: `msg_name` is the caller's string and the C++
     * backend routes every name through one on_message thunk, so the four
     * backends have no shared identity for "the offending callback" here.
     * jce_script.h says so at jce_script_call_message. */
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(P == 4, 'a message handler was disabled (P == ' .. tostring(P) "
        ".. ' of 4). PINNED: call_message does not participate in the "
        "failing-callback rule; see jce_script.h')\n"
        "return {}\n"), "see the assert message");
    TEST_ASSERT_EQUAL_INT(4, log_count_with("ping error:"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, log_count_with("disabled for this script"),
        "no disable notice may be written for a dispatcher that does not "
        "participate");

    jce_script_destroy(s);
}

/* ── 5. a message named "on_update" cannot disable the real on_update ───── */
static void test_a_message_cannot_impersonate_a_lifecycle_slot(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    /* Three sends drive on_update to its raise THROUGH the message path.  If
     * the slot were derived from the method NAME rather than passed in by the
     * dispatcher, this would disable the real on_update — and a game could
     * silence another entity's frame handler with jce.send_message. */
    jce_script_call_message(s, inst, "on_update", 0.0, NULL);
    jce_script_call_message(s, inst, "on_update", 0.0, NULL);
    jce_script_call_message(s, inst, "on_update", 0.0, NULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, log_count_with("disabled for this script"),
        "a caller-named message disabled a lifecycle slot: the slot is being "
        "derived from the method name instead of stated by the dispatcher");

    /* And the real dispatcher is untouched: it runs, raises on its own count
     * (U is already 3, so the very first one raises) and disables itself. */
    jce_script_call_update(s, inst, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_update")));

    jce_script_destroy(s);
}

/* ── 6. named globals never participate — the UI would go silently dead ── */
static void test_a_named_global_is_never_disabled(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@g",
        "G = 0\n"
        "function on_slider(e, v) G = G + 1; error('slider') end\n"
        "return {}\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 4; ++i) {
        /* The return value is the whole reason: `false` means "no such
         * global", which every UI widget treats as a correctly-absent
         * handler.  A disabled global answering false would make a UISlider
         * go dead with nothing reporting anything wrong. */
        TEST_ASSERT_TRUE_MESSAGE(
            jce_script_call_named_num(s, "on_slider", 5, 0.25),
            "a throwing global stopped reporting 'a handler of that name "
            "existed and was invoked' — that answer is indistinguishable from "
            "'no such global', and the widget goes silently dead");
    }
    TEST_ASSERT_TRUE_MESSAGE(jce_script_call_named(s, "on_slider", 5),
        "call_named must agree with call_named_num about the same global");
    TEST_ASSERT_TRUE_MESSAGE(jce_script_call_named_str(s, "on_slider", 5, "x"),
        "call_named_str must agree with call_named_num about the same global");

    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(G == 6, 'a named global was disabled (G == ' .. tostring(G) .. "
        "' of 6). PINNED: a global has no instance to be disabled on, and "
        "false already means \"no such handler\"; see jce_script.h')\n"
        "return {}\n"), "see the assert message");
    TEST_ASSERT_EQUAL_INT(0, log_count_with("disabled for this script"));

    jce_script_destroy(s);
}

/* ── 7. hot reload re-enables — otherwise the fix does not take ─────────── */
static void test_rebind_re_enables_every_disabled_callback(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    JceScriptModule   mod;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 4; ++i) jce_script_call_update(s, inst, 0.016f);
    jce_script_call_collision(s, inst, 7);
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_update")));
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_collision")));

    mod = jce_script_compile_module(s, "@v2", SRC_V2, strlen(SRC_V2));
    TEST_ASSERT_NOT_EQUAL(0, mod);
    jce_script_rebind_instance(s, inst, mod);
    jce_script_release_module(s, mod);

    /* BOTH disabled callbacks come back, because a rebind is the engine saying
     * the code may have changed.  Without this the script you fixed and saved
     * stays dead until the process restarts — a worse defect than the spam. */
    jce_script_call_update(s, inst, 0.016f);
    jce_script_call_update(s, inst, 0.016f);
    jce_script_call_collision(s, inst, 7);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(V == 102, 'after a rebind V == ' .. tostring(V) .. ', not 102 "
        "(2 updates + 1 collision): a hot reload did not re-enable the "
        "callbacks the old module disabled, so fixing the script does not "
        "revive it')\n"
        "return {}\n"), "see the assert message");

    jce_script_destroy(s);
}

/* ── 8. a re-instantiated script starts clean, and a recycled registry ref
 *      does not inherit the dead instance's disables ──────────────────── */
static void test_a_fresh_instance_inherits_nothing(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance first, second;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    first = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, first);
    for (i = 0; i < 4; ++i) jce_script_call_update(s, first, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_update")));

    /* Releasing luaL_unref's the registry slot, and luaL_ref RECYCLES freed
     * slots — so the next instance is very likely handed the SAME integer.
     * A C-side table keyed by that integer would hand it the dead instance's
     * disables; the mask lives in the instance's own metatable so it cannot. */
    jce_script_release(s, first);
    second = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, second);

    memset(&g_log, 0, sizeof g_log);
    jce_script_call_update(s, second, 0.016f);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(U == 1, 'the fresh instance did not dispatch on_update (U == ' "
        ".. tostring(U) .. ' after its chunk reset it to 0): a released "
        "instance left its disables behind on a recycled registry ref')\n"
        "return {}\n"), "see the assert message");
    TEST_ASSERT_EQUAL_INT(0, g_log.count);

    jce_script_destroy(s);
}

/* ── 9. a script cannot read or forge the flag through its own `self` ──── */
static void test_the_flag_is_not_reachable_from_the_script(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d",
        "SEEN = 'unset'\n"
        "U = 0\n"
        "local M = {}\n"
        "function M:on_update(dt)\n"
        "  U = U + 1\n"
        "  SEEN = tostring(self.__jce_disabled)\n"
        "  if U >= 2 then error('boom') end\n"
        "end\n"
        "return M\n", 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 4; ++i) jce_script_call_update(s, inst, 0.016f);

    /* Field lookup on the instance falls through __index to the MODULE, never
     * to the metatable, so the mask is invisible to ordinary script code.  If
     * it were stored in the instance table instead, `pairs(self)` would show
     * it and a script could clear it. */
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(SEEN == 'nil', 'a script can see the disable mask through "
        "self (' .. tostring(SEEN) .. '): it must live in the metatable, not "
        "in the instance table')\n"
        "assert(U == 2, 'on_update ran ' .. tostring(U) .. ' times, not 2')\n"
        "return {}\n"), "see the assert message");

    jce_script_destroy(s);
}

/* ── 10. no host.log: the disable still takes, and nothing crashes ─────── */
static void test_the_disable_takes_without_a_host_log(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    memset(&h, 0, sizeof h);          /* have_host true, log NULL */
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@d", SRC, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 8; ++i) jce_script_call_update(s, inst, 0.016f);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(U == 3, 'with no host.log the handler ran ' .. tostring(U) .. "
        "' times, not 3: the disable is riding on the reporting path instead "
        "of being the policy')\n"
        "return {}\n"), "see the assert message");

    jce_script_destroy(s);
}

/* ── 11. on_start participates too, on the API's terms ───────────────── */
static void test_on_start_participates(void)
{
    JceScriptHost     h;
    JceScript        *s;
    JceScriptInstance inst;
    int               i;

    host_reset(&h);
    s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    inst = jce_script_instantiate_source(s, "@s", SRC_START, 1);
    TEST_ASSERT_NOT_EQUAL(0, inst);

    for (i = 0; i < 5; ++i) jce_script_call_start(s, inst);

    /* Two dispatches reached the handler: one clean, one that raised.  Then
     * silence.  A backend that passed the non-participating slot at this one
     * call site would log five errors and pass every other case in this file. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, log_count_with("on_start error:"),
        "on_start raised more than once: its dispatcher is passing the "
        "non-participating slot, so the rule skips exactly this one handler");
    TEST_ASSERT_EQUAL_INT(1, log_count_with(notice_for("on_start")));

    /* And only on_start: on_update on the same instance is untouched. */
    jce_script_call_update(s, inst, 0.016f);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, check(s,
        "assert(SS == 102, 'SS == ' .. tostring(SS) .. ', not 102 (2 starts + "
        "1 update): either on_start ran a third time or disabling it took "
        "on_update with it')\n"
        "return {}\n"), "see the assert message");

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_raising_on_update_is_disabled_and_says_so);
    RUN_TEST(test_the_disable_is_per_callback_and_per_instance);
    RUN_TEST(test_collision_and_anim_event_participate);
    RUN_TEST(test_a_message_handler_is_never_disabled);
    RUN_TEST(test_a_message_cannot_impersonate_a_lifecycle_slot);
    RUN_TEST(test_a_named_global_is_never_disabled);
    RUN_TEST(test_rebind_re_enables_every_disabled_callback);
    RUN_TEST(test_a_fresh_instance_inherits_nothing);
    RUN_TEST(test_the_flag_is_not_reachable_from_the_script);
    RUN_TEST(test_the_disable_takes_without_a_host_log);
    RUN_TEST(test_on_start_participates);
    return UNITY_END();
}
