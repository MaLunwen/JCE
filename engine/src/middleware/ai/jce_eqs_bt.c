/*
 * jce_eqs_bt.c  EQS <-> Behavior-Tree adapter.
 *
 * Bridges a JceBlackboard to the pure EQS core and back; see jce_eqs_bt.h for
 * the full contract.  Performs NO nav/physics work — any world test stays a
 * CALLBACK test on the caller's template.  Deterministic + headless.
 */

#include <jce/middleware/ai/jce_eqs_bt.h>

#include <string.h>

/* Max tests the action can substitute into without heap allocation.  Queries
 * with more tests than this still run, but substitution is skipped (the
 * template's parameters are used verbatim) so behaviour stays well-defined. */
#define JCE_EQS_BT_MAX_TESTS 16u

static const char *eqs_bt_key(const char *override_key, const char *fallback)
{
    return (override_key && override_key[0] != '\0') ? override_key : fallback;
}

JceBtStatus JCE_CALL jce_eqs_bt_action(const char *name, void *userdata)
{
    JceEqsBtAction *a = (JceEqsBtAction *)userdata;
    JceEqsQueryDesc work;
    JceEqsTestDesc  tests_copy[JCE_EQS_BT_MAX_TESTS];
    jce_vec3        self_pos;
    jce_vec3        target_pos;
    bool            have_target;
    jce_vec3        best;
    float           score = 0.0f;
    JceEqsScoredPoint sink = { 0 }; /* local out sink; pick_best ignores it */

    (void)name;

    if (!a || !a->eqs || !a->bb)
        return JCE_BT_FAILURE;

    /* (1) self_pos is mandatory and must be a vec3 slot. */
    {
        const char *self_key = eqs_bt_key(a->self_pos_key, JCE_EQS_BT_KEY_SELF_POS);
        if (jce_blackboard_kind(a->bb, self_key) != JCE_BB_VEC3)
            return JCE_BT_FAILURE;
        self_pos = jce_blackboard_get_vec3(a->bb, self_key, jce_v3(0.0f, 0.0f, 0.0f));
    }

    /* (2) target_pos is optional. */
    {
        const char *tgt_key = eqs_bt_key(a->target_pos_key, JCE_EQS_BT_KEY_TARGET_POS);
        have_target = (jce_blackboard_kind(a->bb, tgt_key) == JCE_BB_VEC3);
        target_pos  = jce_blackboard_get_vec3(a->bb, tgt_key, jce_v3(0.0f, 0.0f, 0.0f));
    }

    /* Build the working query from the template. */
    work = a->query;
    if (a->recenter_on_self)
        work.generator.center = self_pos;

    /* Substitute the live target into opted-in tests (private copy so the
     * caller's const template is never mutated).  Only when we actually have a
     * target and the test list fits the stack buffer. */
    if (have_target && a->subs && a->query.tests &&
        a->query.test_count > 0u && a->query.test_count <= JCE_EQS_BT_MAX_TESTS) {
        uint32_t i;
        memcpy(tests_copy, a->query.tests,
               (size_t)a->query.test_count * sizeof(JceEqsTestDesc));
        for (i = 0u; i < a->query.test_count; ++i) {
            switch (a->subs[i]) {
            case JCE_EQS_BT_SUB_PARAM_VEC3:
                tests_copy[i].param_vec3 = target_pos;
                break;
            case JCE_EQS_BT_SUB_DOT_ORIGIN:
                tests_copy[i].dot_origin = target_pos;
                break;
            case JCE_EQS_BT_SUB_NONE:
            default:
                break;
            }
        }
        work.tests = tests_copy;
    }

    /* The action picks the single best — point the output at a local sink so a
     * caller-supplied (possibly NULL) out_candidates never matters here. */
    work.out_candidates = &sink;
    work.out_capacity   = 1u;

    /* (3) run the pure EQS. */
    if (!jce_eqs_pick_best(a->eqs, &work, &best, &score))
        return JCE_BT_FAILURE;

    /* (4) publish the winner. */
    {
        const char *res_key = eqs_bt_key(a->result_key, JCE_EQS_BT_KEY_RESULT);
        jce_blackboard_set_vec3(a->bb, res_key, best);
    }

    a->last_result = best;
    a->last_score  = score;
    a->last_ok     = true;
    return JCE_BT_SUCCESS;
}

void JCE_CALL jce_eqs_bt_register(JceBtContext *ctx, const char *name,
                                  JceEqsBtAction *action)
{
    if (!ctx || !action)
        return;
    jce_bt_register_action(ctx, name ? name : "RunEqsQuery",
                           jce_eqs_bt_action, action);
}
