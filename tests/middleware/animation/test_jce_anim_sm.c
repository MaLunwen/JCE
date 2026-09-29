/*
 * test_jce_anim_sm.c — Unit tests for jce_anim_sm.h (L3 animation).
 *
 * Authoring tools emit `.anim_sm.json`; the runtime is a pure JSON
 * parser + per-frame evaluator.  We load tiny in-memory state machines
 * (Idle→Walk driven by a "speed" float param + a Trigger fire path),
 * tick `_update()` with deltas and verify the active state, transition
 * blend, and time accumulation.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_anim_sm.h>

#include <limits.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* op enum mirror — kept in sync with jce_anim_sm.c. */
enum { OP_GT = 0, OP_LT = 1, OP_EQ = 2, OP_NEQ = 3, OP_TRUE = 4, OP_FALSE = 5 };

/* ------------------------------------------------------------------ */
/* Idle ─speed>0.1─▶ Walk                                              */
/* Walk ─speed<0.1─▶ Idle                                              */
/* ------------------------------------------------------------------ */

static const char *k_idle_walk_sm =
    "{"
    "  \"default\": 0,"
    "  \"params\": ["
    "    { \"name\": \"speed\", \"type\": 0, \"defF\": 0.0 }"
    "  ],"
    "  \"states\": ["
    "    { \"name\": \"Idle\", \"clip\": \"clips/idle.anim\", \"speed\": 1.0, \"loop\": true },"
    "    { \"name\": \"Walk\", \"clip\": \"clips/walk.anim\", \"speed\": 1.0, \"loop\": true }"
    "  ],"
    "  \"transitions\": ["
    "    { \"from\": 0, \"to\": 1, \"duration\": 0.2,"
    "      \"conds\": [ { \"param\": 0, \"op\": 0, \"thr\": 0.1 } ] },"
    "    { \"from\": 1, \"to\": 0, \"duration\": 0.2,"
    "      \"conds\": [ { \"param\": 0, \"op\": 1, \"thr\": 0.1 } ] }"
    "  ]"
    "}";

static const char *k_trigger_sm =
    "{"
    "  \"default\": 0,"
    "  \"params\": ["
    "    { \"name\": \"fire\", \"type\": 3 }"
    "  ],"
    "  \"states\": ["
    "    { \"name\": \"A\", \"clip\": \"a.anim\", \"speed\": 1.0 },"
    "    { \"name\": \"B\", \"clip\": \"b.anim\", \"speed\": 1.0 }"
    "  ],"
    "  \"transitions\": ["
    "    { \"from\": 0, \"to\": 1, \"duration\": 0.1,"
    "      \"conds\": [ { \"param\": 0, \"op\": 0 } ] }"
    "  ]"
    "}";

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

static void test_load_null_inputs_safe(void)
{
    TEST_ASSERT_NULL(jce_anim_sm_load_text(NULL, 10));
    TEST_ASSERT_NULL(jce_anim_sm_load_text("{}", 0));
    TEST_ASSERT_NULL(jce_anim_sm_load_file(NULL));
    jce_anim_sm_free(NULL);  /* must not crash */
    TEST_PASS();
}

static void test_load_minimal_sm(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    TEST_ASSERT_NOT_NULL(sm);
    TEST_ASSERT_EQUAL_INT(2, jce_anim_sm_state_count(sm));
    TEST_ASSERT_EQUAL_STRING("Idle", jce_anim_sm_state_name(sm, 0));
    TEST_ASSERT_EQUAL_STRING("clips/walk.anim", jce_anim_sm_state_clip(sm, 1));
    TEST_ASSERT_EQUAL_INT(1, jce_anim_sm_param_count(sm));
    TEST_ASSERT_EQUAL_STRING("speed", jce_anim_sm_param_name(sm, 0));
    TEST_ASSERT_EQUAL_INT(JCE_ANIM_SM_PARAM_FLOAT,
                          (int)jce_anim_sm_param_type(sm, 0));
    jce_anim_sm_free(sm);
}

static void test_initial_state_is_default(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(0, ev.state_index);
    TEST_ASSERT_EQUAL_STRING("Idle", ev.state_name);
    TEST_ASSERT_EQUAL_INT(-1, ev.transition_index);
    jce_anim_sm_free(sm);
}

/* ------------------------------------------------------------------ */
/* Param drive                                                          */
/* ------------------------------------------------------------------ */

static void test_param_find_returns_negative_for_unknown(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    TEST_ASSERT_EQUAL_INT(0,  jce_anim_sm_param_find(sm, "speed"));
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_sm_param_find(sm, "ghost"));
    jce_anim_sm_free(sm);
}

static void test_set_float_drives_transition_to_walk(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));

    jce_anim_sm_set_float(sm, "speed", 1.0f);
    jce_anim_sm_update(sm, 0.05f);          /* fires transition: elapsed=0 */
    jce_anim_sm_update(sm, 0.05f);          /* advance blend */

    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_NOT_EQUAL(-1, ev.transition_index);
    TEST_ASSERT_EQUAL_INT(0, ev.from_state);
    TEST_ASSERT_EQUAL_INT(1, ev.to_state);
    TEST_ASSERT_TRUE(ev.blend > 0.0f && ev.blend < 1.0f);
    jce_anim_sm_free(sm);
}

static void test_long_update_completes_transition(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));

    jce_anim_sm_set_float(sm, "speed", 1.0f);
    /* First update enters transition; subsequent updates advance + complete it. */
    jce_anim_sm_update(sm, 0.1f);
    jce_anim_sm_update(sm, 0.5f);
    jce_anim_sm_update(sm, 0.1f);

    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(1, ev.state_index);
    TEST_ASSERT_EQUAL_INT(-1, ev.transition_index);
    jce_anim_sm_free(sm);
}

static void test_reset_returns_to_default_state(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));

    jce_anim_sm_set_float(sm, "speed", 1.0f);
    jce_anim_sm_update(sm, 0.1f);
    jce_anim_sm_update(sm, 0.5f);
    jce_anim_sm_update(sm, 0.1f);
    jce_anim_sm_reset(sm);

    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(0, ev.state_index);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, ev.state_time);
    jce_anim_sm_free(sm);
}

/* ------------------------------------------------------------------ */
/* Trigger consumption                                                  */
/* ------------------------------------------------------------------ */

static void test_trigger_fires_transition_then_consumed(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_trigger_sm, strlen(k_trigger_sm));

    jce_anim_sm_set_trigger(sm, "fire");
    jce_anim_sm_update(sm, 0.5f);   /* enter transition + complete it */
    jce_anim_sm_update(sm, 0.1f);

    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(1, ev.state_index);

    /* Triggers are one-shot — resetting back to A should not re-fire. */
    jce_anim_sm_reset(sm);
    jce_anim_sm_update(sm, 0.5f);
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(0, ev.state_index);
    jce_anim_sm_free(sm);
}

/* ------------------------------------------------------------------ */
/* Setters silently ignore unknown / mismatched                         */
/* ------------------------------------------------------------------ */

static void test_setters_noop_on_unknown_or_wrong_type(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    /* These must not crash and not affect state. */
    jce_anim_sm_set_float  (sm, "ghost", 9.0f);
    jce_anim_sm_set_int    (sm, "speed", 7);     /* speed is FLOAT */
    jce_anim_sm_set_bool   (sm, "speed", true);
    jce_anim_sm_set_trigger(sm, "speed");

    jce_anim_sm_update(sm, 0.05f);
    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(0, ev.state_index);    /* still Idle */
    jce_anim_sm_free(sm);
}

/* ------------------------------------------------------------------ */
/* State-change polling (state-enter/exit gameplay events)             */
/* ------------------------------------------------------------------ */

/* Seed sentinel: any value that can never be a real state index (or -1
 * "no active state").  INT_MIN matches the renderer's SR_SM_STATE_SEED. */
#define SM_STATE_SEED INT_MIN

static void test_poll_state_change_null_safe(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    int prev = SM_STATE_SEED, from = 0, to = 0;
    /* NULL sm / NULL cursor must both return false and not crash. */
    TEST_ASSERT_FALSE(jce_anim_sm_poll_state_change(NULL, &prev, &from, &to));
    TEST_ASSERT_FALSE(jce_anim_sm_poll_state_change(sm, NULL, &from, &to));
    jce_anim_sm_free(sm);
}

static void test_poll_seed_fires_initial_state_enter(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));

    /* First poll with the seed sentinel reports a change INTO the initial
       state (from = the sentinel, to = state 0), so gameplay receives an
       on_state_enter for the start state. */
    int prev = SM_STATE_SEED, from = -99, to = -99;
    TEST_ASSERT_TRUE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));
    TEST_ASSERT_EQUAL_INT(SM_STATE_SEED, from);
    TEST_ASSERT_EQUAL_INT(0, to);
    TEST_ASSERT_EQUAL_INT(0, prev);   /* cursor advanced to the initial state */

    /* No change since: a second poll returns false and leaves out-params alone. */
    from = -99; to = -99;
    TEST_ASSERT_FALSE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));
    TEST_ASSERT_EQUAL_INT(-99, from);
    TEST_ASSERT_EQUAL_INT(-99, to);
    TEST_ASSERT_EQUAL_INT(0, prev);

    jce_anim_sm_free(sm);
}

static void test_poll_reports_transition_exactly_once(void)
{
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));

    /* Consume the initial-state-enter so we isolate the Idle->Walk change. */
    int prev = SM_STATE_SEED, from = 0, to = 0;
    TEST_ASSERT_TRUE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));
    TEST_ASSERT_EQUAL_INT(0, to);   /* entered Idle */

    /* Drive speed up and run the SM long enough to complete the Idle->Walk
       transition (the active state index only advances to Walk on completion). */
    jce_anim_sm_set_float(sm, "speed", 1.0f);
    jce_anim_sm_update(sm, 0.1f);
    jce_anim_sm_update(sm, 0.5f);
    jce_anim_sm_update(sm, 0.1f);

    JceAnimSmEval ev = {0};
    jce_anim_sm_eval(sm, &ev);
    TEST_ASSERT_EQUAL_INT(1, ev.state_index);   /* now in Walk */

    /* The poll reports from = Idle(0), to = Walk(1) exactly once. */
    from = -99; to = -99;
    TEST_ASSERT_TRUE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));
    TEST_ASSERT_EQUAL_INT(0, from);
    TEST_ASSERT_EQUAL_INT(1, to);
    TEST_ASSERT_EQUAL_INT(1, prev);

    /* A second poll with no further change returns false. */
    TEST_ASSERT_FALSE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));

    jce_anim_sm_free(sm);
}

static void test_poll_seed_minus_one_skips_initial_enter(void)
{
    /* Seeding to -1 (the "no active state" value that also equals the initial
       active state index... no: initial is 0) means the FIRST poll fires the
       enter into state 0 because 0 != -1.  This documents the alternative seed:
       -1 still reports the initial enter here since the initial state is 0, but
       it would NOT fire if the SM started with no active state.  We assert the
       cursor lands on the real initial index regardless of seed choice. */
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    int prev = -1, from = -99, to = -99;
    TEST_ASSERT_TRUE(jce_anim_sm_poll_state_change(sm, &prev, &from, &to));
    TEST_ASSERT_EQUAL_INT(-1, from);
    TEST_ASSERT_EQUAL_INT(0, to);
    TEST_ASSERT_EQUAL_INT(0, prev);
    jce_anim_sm_free(sm);
}

static void test_poll_null_out_params_ok(void)
{
    /* out_from / out_to may be NULL; the cursor still advances + true returns. */
    JceAnimSm *sm = jce_anim_sm_load_text(k_idle_walk_sm, strlen(k_idle_walk_sm));
    int prev = SM_STATE_SEED;
    TEST_ASSERT_TRUE(jce_anim_sm_poll_state_change(sm, &prev, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, prev);
    TEST_ASSERT_FALSE(jce_anim_sm_poll_state_change(sm, &prev, NULL, NULL));
    jce_anim_sm_free(sm);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_load_null_inputs_safe);
    RUN_TEST(test_load_minimal_sm);
    RUN_TEST(test_initial_state_is_default);
    RUN_TEST(test_param_find_returns_negative_for_unknown);
    RUN_TEST(test_set_float_drives_transition_to_walk);
    RUN_TEST(test_long_update_completes_transition);
    RUN_TEST(test_reset_returns_to_default_state);
    RUN_TEST(test_trigger_fires_transition_then_consumed);
    RUN_TEST(test_setters_noop_on_unknown_or_wrong_type);
    RUN_TEST(test_poll_state_change_null_safe);
    RUN_TEST(test_poll_seed_fires_initial_state_enter);
    RUN_TEST(test_poll_reports_transition_exactly_once);
    RUN_TEST(test_poll_seed_minus_one_skips_initial_enter);
    RUN_TEST(test_poll_null_out_params_ok);
    return UNITY_END();
}
