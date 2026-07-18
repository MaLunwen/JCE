/*
 * jce_scene_transaction.h -- Isolated candidate build and atomic activation.
 */

#ifndef JCE_SCENE_TRANSACTION_H
#define JCE_SCENE_TRANSACTION_H

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_compiler.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_SCENE_PREWARM_PENDING = 0,
    JCE_SCENE_PREWARM_READY,
    JCE_SCENE_PREWARM_FAILED
} JceScenePrewarmResult;

typedef bool (JCE_CALL *JceSceneTransactionApplyFn)(
    void *user, JceScene *staging,
    const JceScenePlanOperation *operation, JceEntity *out_entity);

typedef bool (JCE_CALL *JceSceneTransactionValidateFn)(
    void *user, const JceScene *staging,
    const JceSceneFrozenPlan *plan);

typedef JceScenePrewarmResult (JCE_CALL *JceSceneTransactionPrewarmFn)(
    void *user, JceScene *staging, const JceSceneFrozenPlan *plan);

typedef void (JCE_CALL *JceSceneTransactionDestroySceneFn)(
    void *user, JceScene *scene);

typedef void (JCE_CALL *JceSceneTransactionActivatedFn)(
    void *user, JceScene *current, JceScene *previous,
    uint64_t generation);

typedef struct {
    void *user;
    JceScene *initial_scene;
    bool own_initial_scene;
    uint32_t entity_budget;
    bool retain_previous_until_healthy;
    JceSceneTransactionApplyFn apply;
    JceSceneTransactionValidateFn validate;
    JceSceneTransactionPrewarmFn prewarm;
    JceSceneTransactionDestroySceneFn destroy_scene;
    JceSceneTransactionActivatedFn on_activated;
} JceSceneTransactionDesc;

typedef enum {
    JCE_SCENE_TRANSACTION_IDLE = 0,
    JCE_SCENE_TRANSACTION_BUILDING,
    JCE_SCENE_TRANSACTION_VALIDATING,
    JCE_SCENE_TRANSACTION_PREWARMING,
    JCE_SCENE_TRANSACTION_READY,
    JCE_SCENE_TRANSACTION_HEALTH_CHECK,
    JCE_SCENE_TRANSACTION_ACTIVE,
    JCE_SCENE_TRANSACTION_FAILED,
    JCE_SCENE_TRANSACTION_CANCELLED
} JceSceneTransactionState;

typedef enum {
    JCE_SCENE_TRANSACTION_ERROR_NONE = 0,
    JCE_SCENE_TRANSACTION_ERROR_INVALID_ARGUMENT,
    JCE_SCENE_TRANSACTION_ERROR_STALE_PLAN,
    JCE_SCENE_TRANSACTION_ERROR_CREATE_SCENE,
    JCE_SCENE_TRANSACTION_ERROR_BUDGET,
    JCE_SCENE_TRANSACTION_ERROR_APPLY,
    JCE_SCENE_TRANSACTION_ERROR_HIERARCHY,
    JCE_SCENE_TRANSACTION_ERROR_VALIDATION,
    JCE_SCENE_TRANSACTION_ERROR_PREWARM,
    JCE_SCENE_TRANSACTION_ERROR_GENERATION,
    JCE_SCENE_TRANSACTION_ERROR_HEALTH_CHECK
} JceSceneTransactionError;

typedef struct JceSceneTransaction JceSceneTransaction;

JCE_API void JCE_CALL
jce_scene_transaction_desc_default(JceSceneTransactionDesc *desc);

JCE_API JceSceneTransaction *JCE_CALL
jce_scene_transaction_create(const JceSceneTransactionDesc *desc);

JCE_API void JCE_CALL
jce_scene_transaction_destroy(JceSceneTransaction *transaction);

JCE_API bool JCE_CALL
jce_scene_transaction_begin(JceSceneTransaction *transaction,
                            const JceSceneFrozenPlan *plan);

/* Advances at most `max_operations` build operations, or one validation /
 * prewarm phase. Never commits implicitly. */
JCE_API JceSceneTransactionState JCE_CALL
jce_scene_transaction_step(JceSceneTransaction *transaction,
                           uint32_t max_operations);

/* Explicit frame-boundary activation. The generation guard rejects callbacks
 * from cancelled or superseded candidates. */
JCE_API bool JCE_CALL
jce_scene_transaction_commit(JceSceneTransaction *transaction,
                             uint64_t generation);

/* Completes the post-activation health gate. `healthy=false` restores the
 * previous scene and destroys the failed candidate. */
JCE_API bool JCE_CALL
jce_scene_transaction_mark_healthy(JceSceneTransaction *transaction,
                                   bool healthy);

/* Cancels only a pre-commit candidate. Once commit enters the health gate,
 * cancellation is ignored and the caller must report the health result. */
JCE_API void JCE_CALL
jce_scene_transaction_cancel(JceSceneTransaction *transaction);

JCE_API JceSceneTransactionState JCE_CALL
jce_scene_transaction_state(const JceSceneTransaction *transaction);

JCE_API JceSceneTransactionError JCE_CALL
jce_scene_transaction_error(const JceSceneTransaction *transaction);

/* Returns the current pre-commit guard, or zero when no candidate is pending. */
JCE_API uint64_t JCE_CALL
jce_scene_transaction_pending_generation(
    const JceSceneTransaction *transaction);

JCE_API uint64_t JCE_CALL
jce_scene_transaction_active_generation(
    const JceSceneTransaction *transaction);

JCE_API JceScene *JCE_CALL
jce_scene_transaction_active_scene(const JceSceneTransaction *transaction);

JCE_API uint64_t JCE_CALL
jce_scene_transaction_active_plan_hash(
    const JceSceneTransaction *transaction);

JCE_API uint32_t JCE_CALL
jce_scene_transaction_find_role(const JceSceneTransaction *transaction,
                                const char *stable_role,
                                JceEntity *out_entities,
                                uint32_t capacity);

JCE_API JceEntity JCE_CALL
jce_scene_transaction_find_stable_id(
    const JceSceneTransaction *transaction, uint64_t stable_entity_id);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_TRANSACTION_H */
