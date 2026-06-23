/*
 * jce_eqs_bt.h  EQS <-> Behavior-Tree adapter (AI layer 3).
 *
 * Bridges the headless Environment Query System (jce_eqs.h) into a Behavior
 * Tree as a registerable ACTION node.  The EQS core stays pure (generators /
 * tests / scoring, deterministic, zero physics/nav/BT calls); the BT core
 * stays unchanged.  This file is the ONLY seam between them: it reads the
 * agent context from a JceBlackboard, drives a caller-configured query through
 * jce_eqs_run / jce_eqs_pick_best, and writes the winning location back to the
 * blackboard so the rest of the tree (e.g. MoveToTarget) can act on it.
 *
 * ------------------------------------------------------------------------
 * Why a bundled struct instead of the JceBtTickEnv path?
 * ------------------------------------------------------------------------
 * The legacy jce_bt_action_fn callback only receives (name, userdata) — it
 * has NO access to the per-tick env's blackboard (that channel backs the
 * jce_bt_register_library nodes).  So the game wires this action through
 * jce_bt_register_action with a JceEqsBtAction* userdata that bundles, set up
 * ONCE:
 *   - the agent's JceBlackboard (the action reads self_pos/target_pos and
 *     writes the result there),
 *   - a reusable JceEqs handle (scratch; one per agent/thread — NOT shared),
 *   - a JceEqsQueryDesc TEMPLATE (generator + tests) the game authored,
 *   - the blackboard key names + per-test target-substitution flags.
 * This keeps the action data-driven and the EQS core callback-injected: any
 * navmesh / line-of-sight test stays the game's CALLBACK test on the template,
 * never a dependency of this adapter.
 *
 * Determinism / headlessness: the adapter performs NO nav/physics work itself.
 * It only (1) reads two vec3 keys, (2) recenters the generator + substitutes
 * the target into opted-in tests, (3) runs the pure EQS, (4) writes one vec3.
 * Same blackboard + same template -> same result.
 *
 * Thread-safety: NOT thread-safe (inherits jce_eqs + jce_bt + blackboard).
 * One JceEqsBtAction (and its JceEqs handle) per agent / per thread.
 */

#ifndef JCE_EQS_BT_H
#define JCE_EQS_BT_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/middleware/ai/jce_bt.h>
#include <jce/middleware/ai/jce_eqs.h>
#include <jce/middleware/ai/jce_perception.h> /* JceBlackboard */

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Default blackboard keys the action reads/writes when the caller leaves the
 * corresponding key field NULL. */
#define JCE_EQS_BT_KEY_SELF_POS   "self_pos"   /* vec3 in  (query center)    */
#define JCE_EQS_BT_KEY_TARGET_POS "target_pos" /* vec3 in  (optional target) */
#define JCE_EQS_BT_KEY_RESULT     "eqs_result" /* vec3 out (best candidate)  */

/*
 * Per-test target substitution.  At tick time the action takes a private copy
 * of the template's tests and, for any test whose substitution is not NONE,
 * replaces a vec3 parameter with the live `target_pos` read from the
 * blackboard.  This is how the game keeps a static JceEqsQueryDesc template
 * yet steers it at the agent's current target each tick.
 *
 *   NONE        — use the template's parameter verbatim.
 *   PARAM_VEC3  — overwrite the test's param_vec3 (DISTANCE target /
 *                 DOT direction) with target_pos.
 *   DOT_ORIGIN  — overwrite the test's dot_origin with target_pos.
 *
 * When target_pos is ABSENT from the blackboard, tests marked for
 * substitution fall back to the template value (no substitution that tick),
 * so a query that does not need a target still runs.
 */
typedef enum {
    JCE_EQS_BT_SUB_NONE       = 0,
    JCE_EQS_BT_SUB_PARAM_VEC3 = 1,
    JCE_EQS_BT_SUB_DOT_ORIGIN = 2
} JceEqsBtSub;

/*
 * Action userdata.  The game owns every pointer here and keeps it alive for
 * the lifetime of the registration.  Set it up once and register with
 * jce_bt_register_action(ctx, "RunEqsQuery", jce_eqs_bt_action, &action).
 */
typedef struct {
    JceEqs              *eqs;       /* required: reusable scratch handle */
    JceBlackboard       *bb;       /* required: the agent's blackboard  */
    JceEqsQueryDesc      query;    /* template: generator + tests.  Its
                                    * out_candidates/out_capacity are
                                    * IGNORED (the action uses pick_best). */

    /* Optional key overrides (NULL -> the JCE_EQS_BT_KEY_* defaults). */
    const char *self_pos_key;      /* vec3 in : recenter the generator   */
    const char *target_pos_key;    /* vec3 in : optional target          */
    const char *result_key;        /* vec3 out: best candidate position  */

    /* Optional per-test substitution.  When `subs` is non-NULL it must have
     * `query.test_count` entries; entry i applies to query.tests[i].  NULL
     * means "no substitution for any test" (a pure self-centered query). */
    const JceEqsBtSub *subs;

    /* When true, recenter the generator on self_pos (the common case).  When
     * false the template's generator.center is used verbatim. */
    bool recenter_on_self;

    /* Filled by the action on the last tick (debug / introspection); the
     * blackboard write is the authoritative output.  Untouched on FAILURE. */
    jce_vec3 last_result;
    float    last_score;
    bool     last_ok;
} JceEqsBtAction;

/*
 * The BT action callback.  Register as:
 *   jce_bt_register_action(ctx, "RunEqsQuery", jce_eqs_bt_action, &action);
 *
 * On tick:
 *   1. read self_pos (vec3) from the blackboard; if recenter_on_self, set the
 *      working generator's center to it (Y carried through the generator);
 *   2. read target_pos (vec3, optional) and substitute it into any test
 *      marked via `subs`;
 *   3. run the query (jce_eqs_pick_best);
 *   4. on a survivor, write it to result_key and return JCE_BT_SUCCESS;
 *      otherwise (all filtered / invalid query / NULL inputs) return
 *      JCE_BT_FAILURE.  Never returns JCE_BT_RUNNING (a query is instantaneous).
 *
 * NULL-safe: a NULL `userdata`, NULL eqs/bb, or a self_pos key that is absent
 * /not a vec3 returns JCE_BT_FAILURE without touching the blackboard.
 */
JCE_API JceBtStatus JCE_CALL jce_eqs_bt_action(const char *name, void *userdata);

/*
 * Convenience: register jce_eqs_bt_action under `name` (or "RunEqsQuery" when
 * name is NULL) on `ctx` with `action` as userdata.  No-op on NULL ctx/action.
 */
JCE_API void JCE_CALL jce_eqs_bt_register(JceBtContext *ctx, const char *name,
                                          JceEqsBtAction *action);

JCE_EXTERN_C_END

#endif /* JCE_EQS_BT_H */
