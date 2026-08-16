/*
 * jce_scene_generation.h -- AI planning to transactional scene activation.
 *
 * This L5 coordinator composes the provider-neutral AI director with the L4
 * deterministic scene transaction. It never commits implicitly: the caller
 * chooses the frame boundary and confirms the guarded health frame.
 */

#ifndef JCE_SCENE_GENERATION_H
#define JCE_SCENE_GENERATION_H

#include <jce/middleware/scene/jce_scene_transaction.h>
#include <jce/runtime/jce_ai_scene_director.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_SCENE_GENERATION_IDLE = 0,
    JCE_SCENE_GENERATION_PLANNING,
    JCE_SCENE_GENERATION_BUILDING,
    JCE_SCENE_GENERATION_VALIDATING,
    JCE_SCENE_GENERATION_PREWARMING,
    JCE_SCENE_GENERATION_FROZEN_READY,
    JCE_SCENE_GENERATION_HEALTH_CHECK,
    JCE_SCENE_GENERATION_ACTIVE,
    JCE_SCENE_GENERATION_FAILED,
    JCE_SCENE_GENERATION_CANCELLED,
    JCE_SCENE_GENERATION_ATTESTING
} JceSceneGenerationStatus;

typedef struct {
    JceAiSceneDirectorDesc director;
    JceSceneTransactionDesc transaction;
    JceSceneCatalog catalog;
    uint32_t max_operations_per_update;
} JceSceneGenerationDesc;

typedef struct JceSceneGenerationCoordinator JceSceneGenerationCoordinator;

JCE_API void JCE_CALL
jce_scene_generation_desc_default(JceSceneGenerationDesc *desc);

JCE_API JceSceneGenerationCoordinator *JCE_CALL
jce_scene_generation_create(const JceSceneGenerationDesc *desc);

JCE_API void JCE_CALL
jce_scene_generation_destroy(JceSceneGenerationCoordinator *coordinator);

/* Starts one generation. Busy coordinators reject replacement requests. */
JCE_API bool JCE_CALL
jce_scene_generation_request(JceSceneGenerationCoordinator *coordinator,
                             const JceAiSceneRequest *request);

/* Polls planning and advances one bounded candidate-build slice. */
JCE_API void JCE_CALL
jce_scene_generation_update(JceSceneGenerationCoordinator *coordinator,
                            double delta_seconds);

/* Explicit frame-boundary activation of a FROZEN_READY candidate. */
JCE_API bool JCE_CALL
jce_scene_generation_commit(JceSceneGenerationCoordinator *coordinator);

/* Completes the guarded frame. false rolls back to the previous scene. */
JCE_API bool JCE_CALL
jce_scene_generation_mark_healthy(
    JceSceneGenerationCoordinator *coordinator, bool healthy);

JCE_API void JCE_CALL
jce_scene_generation_cancel(JceSceneGenerationCoordinator *coordinator);

JCE_API JceSceneGenerationStatus JCE_CALL
jce_scene_generation_status(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API JceAiScenePlanSource JCE_CALL
jce_scene_generation_source(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API JceAiSceneError JCE_CALL
jce_scene_generation_director_error(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API JceSceneTransactionError JCE_CALL
jce_scene_generation_transaction_error(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API bool JCE_CALL
jce_scene_generation_frozen_plan(
    const JceSceneGenerationCoordinator *coordinator,
    JceSceneFrozenPlan *out_plan);

JCE_API JceScene *JCE_CALL
jce_scene_generation_active_scene(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API uint64_t JCE_CALL
jce_scene_generation_active_plan_hash(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API uint64_t JCE_CALL
jce_scene_generation_pending_attestation_hash(
    const JceSceneGenerationCoordinator *coordinator);

JCE_API uint64_t JCE_CALL
jce_scene_generation_active_attestation_hash(
    const JceSceneGenerationCoordinator *coordinator);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_GENERATION_H */
