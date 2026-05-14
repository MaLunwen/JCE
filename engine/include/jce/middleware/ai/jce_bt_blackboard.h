/*
 * jce_bt_blackboard.h  Shared key/value store for Behavior Tree actions.
 *
 * Behavior trees in JCE expose action callbacks via name; this module
 * gives those callbacks a place to share state without globals: a
 * tagged-union map keyed by short string names ("hp", "target_pos",
 * "alert_timer").  Mirrors Unity NodeCanvas / Behavior Designer
 * Blackboard at the data layer.
 *
 * Plus four ready-to-register decorator-style action callbacks that
 * read the blackboard:
 *   - `bt.cooldown <ms> <key>`     SUCCESS at most once per `ms`, FAILURE otherwise
 *   - `bt.random  <chance>`         SUCCESS with given probability
 *   - `bt.wait_seconds <s>`         RUNNING until `s` seconds elapsed
 *   - `bt.set_flag <key> <value>`   write a boolean to the blackboard, SUCCESS
 *
 * The "argument" syntax is the action name with space-separated
 * tokens — caller's bt loader splits them.  This keeps the action
 * registration surface flat (one name per behaviour) while letting
 * authors parameterise.
 *
 * Layer: middleware / ai (Layer 3) — public.
 */

#ifndef JCE_BT_BLACKBOARD_H
#define JCE_BT_BLACKBOARD_H

#include <jce/middleware/ai/jce_bt.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceBtBlackboard JceBtBlackboard;

/* Lifecycle.  Capacity caps at 64 entries — adequate for typical AI
 * agents.  Returns NULL on OOM. */
JCE_API JceBtBlackboard *jce_bt_blackboard_create(void);
JCE_API void             jce_bt_blackboard_destroy(JceBtBlackboard *bb);

/* Typed accessors.  Missing keys default to 0 / false / "" / NULL. */
JCE_API void   jce_bt_blackboard_set_float (JceBtBlackboard *bb, const char *key, float v);
JCE_API float  jce_bt_blackboard_get_float (const JceBtBlackboard *bb, const char *key, float def);

JCE_API void   jce_bt_blackboard_set_int   (JceBtBlackboard *bb, const char *key, int v);
JCE_API int    jce_bt_blackboard_get_int   (const JceBtBlackboard *bb, const char *key, int def);

JCE_API void   jce_bt_blackboard_set_bool  (JceBtBlackboard *bb, const char *key, bool v);
JCE_API bool   jce_bt_blackboard_get_bool  (const JceBtBlackboard *bb, const char *key, bool def);

JCE_API void   jce_bt_blackboard_set_vec3  (JceBtBlackboard *bb, const char *key,
                                             float x, float y, float z);
JCE_API bool   jce_bt_blackboard_get_vec3  (const JceBtBlackboard *bb, const char *key,
                                             float *out_x, float *out_y, float *out_z);

JCE_API bool   jce_bt_blackboard_has       (const JceBtBlackboard *bb, const char *key);
JCE_API bool   jce_bt_blackboard_remove    (JceBtBlackboard *bb, const char *key);
JCE_API uint32_t jce_bt_blackboard_count   (const JceBtBlackboard *bb);

/* Tick — advances internal timers (used by `wait_seconds` and
 * `cooldown` action variants).  Call once per game tick. */
JCE_API void   jce_bt_blackboard_tick(JceBtBlackboard *bb, float dt);

/* ── Built-in decorator actions ──────────────────────────────── *
 *
 * Each fn implements jce_bt_action_fn signature.  Register via
 * jce_bt_register_action with names like "bt.cooldown",
 * "bt.random", "bt.wait_seconds", "bt.set_flag".  Pass the
 * blackboard pointer as the userdata. */

JCE_API JceBtStatus jce_bt_action_cooldown    (const char *name, void *user);
JCE_API JceBtStatus jce_bt_action_random      (const char *name, void *user);
JCE_API JceBtStatus jce_bt_action_wait_seconds(const char *name, void *user);
JCE_API JceBtStatus jce_bt_action_set_flag    (const char *name, void *user);

/* Convenience: register all four under the standard "bt.*" names. */
JCE_API void jce_bt_register_builtin_decorators(JceBtContext *ctx,
                                                 JceBtBlackboard *bb);

JCE_EXTERN_C_END

#endif /* JCE_BT_BLACKBOARD_H */
