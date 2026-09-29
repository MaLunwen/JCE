/* test_jce_script_fixed_update.c — the callback that makes physics the same
 * on a fast machine and a slow one.
 *
 * Scripts had on_start, on_update, on_collision, on_destroy and
 * on_anim_event, and NOTHING on the fixed clock.  So a script applying a
 * force ran on a render frame whose dt varies with the frame rate: a jump
 * that clears a gap at 144 Hz does not at 30 Hz, and no amount of care inside
 * on_update fixes it.  Unity's FixedUpdate, Unreal's substepped tick and
 * Godot's _physics_process all exist for exactly this.
 *
 * THIS FILE TESTS THE VM, NOT THE RUNTIME.  Whether the runtime dispatches it
 * once per physics step, before the step, is a different claim with a
 * different measurement; standing up a runtime here would test the accumulator
 * and call it evidence about the callback.  What is asserted here is what the
 * Lua VM — the reference implementation the other six backends are written
 * against — does when the engine calls it.
 *
 * WHAT EACH CASE DEFENDS AGAINST, because "it works" is cheap to fake:
 *
 *   1. A handler that never ran passes "no error was logged".  So the first
 *      case asserts the handler produced an OBSERVABLE EFFECT and that the
 *      effect carries the dt it was given, not a plausible-looking number.
 *   2. "The new callback fires" is passed by an implementation that wired it
 *      to on_update's method name.  So the script defines BOTH, and each
 *      asserts it saw only its own dispatches.
 *   3. A shared disable bit passes every test above and is the defect this
 *      feature would ship with: a handler that throws every physics step
 *      would take the render-frame callback down with it, and the symptom
 *      ("my on_update stopped") would point at a different function.  So the
 *      third case breaks on_fixed_update and asserts on_update SURVIVES.
 *
 * Observation is through Lua globals read back by a follow-up assert-chunk on
 * the same VM: a tripped assert blows that chunk up and yields instance 0, so
 * a NON-ZERO instance is the proof that every assertion inside it held.  That
 * is the idiom test_jce_script_disable.c established.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/middleware/script/jce_script.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

/* ── Mock host: records log lines so a disable NOTICE can be asserted ──── */
typedef struct { int count; char last[512]; } LogRecorder;
static LogRecorder g_log;

static void mock_log(void *user, const char *msg)
{
    (void)user;
    if (!msg) return;
    g_log.count++;
    snprintf(g_log.last, sizeof g_log.last, "%s", msg);
}

static void host_reset(JceScriptHost *h)
{
    memset(&g_log, 0, sizeof g_log);
    memset(h, 0, sizeof *h);
    h->log = mock_log;
}

void setUp(void) {}
void tearDown(void) {}

/* Run `chunk` on the same VM.  Non-zero means every assert in it held. */
static JceScriptInstance check(JceScript *s, const char *chunk)
{
    return jce_script_instantiate_source(s, "@check", chunk, 99);
}

/* ---------------------------------------------------------------------- */

static void test_it_runs_and_receives_the_dt_it_was_given(void)
{
    /* Deliberately NOT 1/60: a handler that ignored its argument and wrote a
     * plausible constant would pass against the obvious number. */
    static const char *SRC =
        "FIXED_N = 0 FIXED_SUM = 0\n"
        "return { on_fixed_update = function(self, dt)\n"
        "    FIXED_N = FIXED_N + 1\n"
        "    FIXED_SUM = FIXED_SUM + dt\n"
        "end }\n";

    JceScriptHost h;
    host_reset(&h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    JceScriptInstance inst = jce_script_instantiate_source(s, "@f", SRC, 1);
    TEST_ASSERT_TRUE_MESSAGE(inst != 0, "the fixture script did not load");

    for (int i = 0; i < 4; ++i)
        jce_script_call_fixed_update(s, inst, 0.015625f);

    /* 1/64, not 1/80.  The first version used 0.0125 and asserted the sum
     * was exactly 0.05 -- and 0.0125 is NOT representable in binary, so the
     * float the engine passes is not the double Lua compares against and the
     * equality failed on a CORRECT implementation.  0.015625 is exact in
     * both, so the sum stays an equality rather than being weakened to a
     * tolerance that would also pass on a dt that was merely close. */
    TEST_ASSERT_TRUE_MESSAGE(
        check(s, "assert(FIXED_N == 4, 'dispatch count')\n"
                 "assert(FIXED_SUM == 0.0625, 'dt did not reach the handler')\n"
                 "return {}\n") != 0,
        "on_fixed_update either did not run, ran the wrong number of times, "
        "or received a dt that was not the one the engine passed");

    jce_script_destroy(s);
}

static void test_it_is_not_on_update_under_another_name(void)
{
    /* Both defined, each counting only its own dispatches.  An implementation
     * that wired the new entry point to the "on_update" method name passes
     * every other assertion in this file and fails here. */
    static const char *SRC =
        "U = 0 F = 0\n"
        "return {\n"
        "  on_update       = function(self, dt) U = U + 1 end,\n"
        "  on_fixed_update = function(self, dt) F = F + 1 end,\n"
        "}\n";

    JceScriptHost h;
    host_reset(&h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    JceScriptInstance inst = jce_script_instantiate_source(s, "@b", SRC, 1);
    TEST_ASSERT_TRUE(inst != 0);

    jce_script_call_update(s, inst, 0.016f);
    jce_script_call_update(s, inst, 0.016f);
    jce_script_call_fixed_update(s, inst, 0.015625f);
    jce_script_call_fixed_update(s, inst, 0.015625f);
    jce_script_call_fixed_update(s, inst, 0.015625f);

    TEST_ASSERT_TRUE_MESSAGE(
        check(s, "assert(U == 2, 'on_update saw ' .. U .. ' not 2')\n"
                 "assert(F == 3, 'on_fixed_update saw ' .. F .. ' not 3')\n"
                 "return {}\n") != 0,
        "the two callbacks are not distinct -- one entry point is dispatching "
        "the other's method");

    jce_script_destroy(s);
}

static void test_a_throwing_fixed_update_does_not_disable_on_update(void)
{
    /* THE CASE THIS FEATURE WOULD HAVE SHIPPED WRONG.  The failing-callback
     * rule disables the offending handler on that instance.  If both share a
     * disable bit, a handler that throws every physics step silently takes the
     * render-frame callback with it, and the report reads "my on_update
     * stopped running" while the bug is in a different function. */
    static const char *SRC =
        "U = 0 F = 0\n"
        "return {\n"
        "  on_update       = function(self, dt) U = U + 1 end,\n"
        "  on_fixed_update = function(self, dt)\n"
        "      F = F + 1 error('boom')\n"
        "  end,\n"
        "}\n";

    JceScriptHost h;
    host_reset(&h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    JceScriptInstance inst = jce_script_instantiate_source(s, "@c", SRC, 1);
    TEST_ASSERT_TRUE(inst != 0);

    /* Three dispatches; the handler must run ONCE and then be suppressed --
     * asserting only "it stopped" would also pass an implementation that
     * never ran it. */
    for (int i = 0; i < 3; ++i)
        jce_script_call_fixed_update(s, inst, 0.015625f);

    /* ...and on_update, on the SAME instance, must still work.  Without this
     * the case above is passed by dropping the whole instance. */
    for (int i = 0; i < 5; ++i)
        jce_script_call_update(s, inst, 0.016f);

    TEST_ASSERT_TRUE_MESSAGE(
        check(s, "assert(F == 1, 'on_fixed_update ran ' .. F ..\n"
                 "       ' times; expected 1 before being disabled')\n"
                 "assert(U == 5, 'on_update ran ' .. U .. ' not 5 -- a "
                 "throwing on_fixed_update took it down with it')\n"
                 "return {}\n") != 0,
        "on_fixed_update and on_update share a disable bit");

    TEST_ASSERT_TRUE_MESSAGE(g_log.count > 0,
        "the failure was suppressed with no log line at all, so a designer "
        "gets silence instead of a notice");

    jce_script_destroy(s);
}

static void test_a_script_without_the_handler_is_a_clean_no_op(void)
{
    /* Every scene authored before this callback existed defines no
     * on_fixed_update, and the engine now calls it every physics step.  That
     * must be free and silent, not a logged miss at 60 Hz. */
    static const char *SRC = "return { on_update = function(self, dt) end }\n";

    JceScriptHost h;
    host_reset(&h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);
    JceScriptInstance inst = jce_script_instantiate_source(s, "@n", SRC, 1);
    TEST_ASSERT_TRUE(inst != 0);

    const int before = g_log.count;
    for (int i = 0; i < 100; ++i)
        jce_script_call_fixed_update(s, inst, 0.015625f);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, g_log.count,
        "dispatching on_fixed_update to a script that does not define it "
        "logged something -- at 60 Hz that is a log line per physics step for "
        "every script in the scene");

    jce_script_destroy(s);
}

static void test_an_invalid_handle_is_tolerated(void)
{
    /* The runtime dispatches this for every tracked script every step; a
     * released instance mid-frame must be a no-op, not a crash. */
    JceScriptHost h;
    host_reset(&h);
    JceScript *s = jce_script_create(&h);
    TEST_ASSERT_NOT_NULL(s);

    jce_script_call_fixed_update(s, 0, 0.015625f);
    jce_script_call_fixed_update(s, 4242, 0.015625f);
    jce_script_call_fixed_update(NULL, 1, 0.015625f);

    jce_script_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_it_runs_and_receives_the_dt_it_was_given);
    RUN_TEST(test_it_is_not_on_update_under_another_name);
    RUN_TEST(test_a_throwing_fixed_update_does_not_disable_on_update);
    RUN_TEST(test_a_script_without_the_handler_is_a_clean_no_op);
    RUN_TEST(test_an_invalid_handle_is_tolerated);
    return UNITY_END();
}
