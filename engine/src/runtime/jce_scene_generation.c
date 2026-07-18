/* Provider-neutral planning and isolated scene-activation coordinator. */

#include <jce/runtime/jce_scene_generation.h>

#include <jce/os/core/jce_alloc.h>

#include <string.h>

#define JCE_SCENE_GENERATION_DEFAULT_SLICE 8u

struct JceSceneGenerationCoordinator {
    JceAiSceneDirector *director;
    JceSceneTransaction *transaction;
    JceSceneCatalog catalog;
    JceSceneGenerationStatus status;
    JceAiScenePlanSource source;
    JceSceneFrozenPlan frozen_plan;
    uint32_t max_operations_per_update;
};

static bool status_busy(JceSceneGenerationStatus status)
{
    return status == JCE_SCENE_GENERATION_PLANNING ||
           status == JCE_SCENE_GENERATION_BUILDING ||
           status == JCE_SCENE_GENERATION_VALIDATING ||
           status == JCE_SCENE_GENERATION_PREWARMING ||
           status == JCE_SCENE_GENERATION_FROZEN_READY ||
           status == JCE_SCENE_GENERATION_HEALTH_CHECK;
}

static void sync_transaction_status(
    JceSceneGenerationCoordinator *coordinator,
    JceSceneTransactionState transaction_status)
{
    switch (transaction_status) {
    case JCE_SCENE_TRANSACTION_BUILDING:
        coordinator->status = JCE_SCENE_GENERATION_BUILDING;
        break;
    case JCE_SCENE_TRANSACTION_VALIDATING:
        coordinator->status = JCE_SCENE_GENERATION_VALIDATING;
        break;
    case JCE_SCENE_TRANSACTION_PREWARMING:
        coordinator->status = JCE_SCENE_GENERATION_PREWARMING;
        break;
    case JCE_SCENE_TRANSACTION_READY:
        coordinator->status = JCE_SCENE_GENERATION_FROZEN_READY;
        break;
    case JCE_SCENE_TRANSACTION_HEALTH_CHECK:
        coordinator->status = JCE_SCENE_GENERATION_HEALTH_CHECK;
        break;
    case JCE_SCENE_TRANSACTION_ACTIVE:
        coordinator->status = JCE_SCENE_GENERATION_ACTIVE;
        break;
    case JCE_SCENE_TRANSACTION_CANCELLED:
        coordinator->status = JCE_SCENE_GENERATION_CANCELLED;
        break;
    case JCE_SCENE_TRANSACTION_FAILED:
        coordinator->status = JCE_SCENE_GENERATION_FAILED;
        break;
    case JCE_SCENE_TRANSACTION_IDLE:
    default:
        coordinator->status = JCE_SCENE_GENERATION_IDLE;
        break;
    }
}

JCE_API void JCE_CALL
jce_scene_generation_desc_default(JceSceneGenerationDesc *desc)
{
    if (!desc)
        return;
    memset(desc, 0, sizeof(*desc));
    jce_ai_scene_director_desc_default(&desc->director);
    jce_scene_transaction_desc_default(&desc->transaction);
    jce_scene_catalog_init(&desc->catalog);
    desc->max_operations_per_update =
        JCE_SCENE_GENERATION_DEFAULT_SLICE;
}

JCE_API JceSceneGenerationCoordinator *JCE_CALL
jce_scene_generation_create(const JceSceneGenerationDesc *desc)
{
    JceSceneGenerationCoordinator *coordinator;
    JceSceneRecipeError catalog_error;

    if (!desc || desc->max_operations_per_update == 0u ||
        jce_scene_catalog_validate(&desc->catalog, &catalog_error) !=
            JCE_SCENE_RECIPE_OK)
        return NULL;
    coordinator = (JceSceneGenerationCoordinator *)jce_malloc(
        sizeof(*coordinator));
    if (!coordinator)
        return NULL;
    memset(coordinator, 0, sizeof(*coordinator));
    coordinator->director = jce_ai_scene_director_create(&desc->director);
    coordinator->transaction =
        jce_scene_transaction_create(&desc->transaction);
    if (!coordinator->director || !coordinator->transaction) {
        jce_ai_scene_director_destroy(coordinator->director);
        jce_scene_transaction_destroy(coordinator->transaction);
        jce_free(coordinator);
        return NULL;
    }
    coordinator->catalog = desc->catalog;
    coordinator->max_operations_per_update =
        desc->max_operations_per_update;
    coordinator->status = JCE_SCENE_GENERATION_IDLE;
    return coordinator;
}

JCE_API void JCE_CALL
jce_scene_generation_destroy(JceSceneGenerationCoordinator *coordinator)
{
    if (!coordinator)
        return;
    jce_ai_scene_director_destroy(coordinator->director);
    jce_scene_transaction_destroy(coordinator->transaction);
    jce_free(coordinator);
}

JCE_API bool JCE_CALL
jce_scene_generation_request(JceSceneGenerationCoordinator *coordinator,
                             const JceAiSceneRequest *request)
{
    if (!coordinator || !request || status_busy(coordinator->status))
        return false;
    memset(&coordinator->frozen_plan, 0,
           sizeof(coordinator->frozen_plan));
    coordinator->source = JCE_AI_SCENE_SOURCE_NONE;
    if (!jce_ai_scene_director_request(coordinator->director, request,
                                       &coordinator->catalog)) {
        coordinator->status = JCE_SCENE_GENERATION_FAILED;
        return false;
    }
    coordinator->status = JCE_SCENE_GENERATION_PLANNING;
    return true;
}

JCE_API void JCE_CALL
jce_scene_generation_update(JceSceneGenerationCoordinator *coordinator,
                            double delta_seconds)
{
    if (!coordinator)
        return;
    if (coordinator->status == JCE_SCENE_GENERATION_PLANNING) {
        JceAiSceneDirectorStatus director_status;

        jce_ai_scene_director_update(coordinator->director, delta_seconds);
        director_status = jce_ai_scene_director_status(coordinator->director);
        if (director_status == JCE_AI_SCENE_DIRECTOR_READY) {
            if (!jce_ai_scene_director_result(coordinator->director,
                                              &coordinator->frozen_plan) ||
                !jce_scene_transaction_begin(coordinator->transaction,
                                              &coordinator->frozen_plan)) {
                coordinator->status = JCE_SCENE_GENERATION_FAILED;
                return;
            }
            coordinator->source =
                jce_ai_scene_director_source(coordinator->director);
            coordinator->status = JCE_SCENE_GENERATION_BUILDING;
        } else if (director_status == JCE_AI_SCENE_DIRECTOR_FAILED) {
            coordinator->status = JCE_SCENE_GENERATION_FAILED;
            return;
        } else if (director_status == JCE_AI_SCENE_DIRECTOR_CANCELLED) {
            coordinator->status = JCE_SCENE_GENERATION_CANCELLED;
            return;
        }
    }

    if (coordinator->status == JCE_SCENE_GENERATION_BUILDING ||
        coordinator->status == JCE_SCENE_GENERATION_VALIDATING ||
        coordinator->status == JCE_SCENE_GENERATION_PREWARMING) {
        JceSceneTransactionState transaction_status =
            jce_scene_transaction_step(
                coordinator->transaction,
                coordinator->max_operations_per_update);
        sync_transaction_status(coordinator, transaction_status);
    }
}

JCE_API bool JCE_CALL
jce_scene_generation_commit(JceSceneGenerationCoordinator *coordinator)
{
    uint64_t generation;

    if (!coordinator ||
        coordinator->status != JCE_SCENE_GENERATION_FROZEN_READY)
        return false;
    generation = jce_scene_transaction_pending_generation(
        coordinator->transaction);
    if (!jce_scene_transaction_commit(coordinator->transaction, generation)) {
        coordinator->status = JCE_SCENE_GENERATION_FAILED;
        return false;
    }
    sync_transaction_status(
        coordinator,
        jce_scene_transaction_state(coordinator->transaction));
    return true;
}

JCE_API bool JCE_CALL
jce_scene_generation_mark_healthy(
    JceSceneGenerationCoordinator *coordinator, bool healthy)
{
    if (!coordinator ||
        coordinator->status != JCE_SCENE_GENERATION_HEALTH_CHECK ||
        !jce_scene_transaction_mark_healthy(coordinator->transaction,
                                            healthy))
        return false;
    sync_transaction_status(
        coordinator,
        jce_scene_transaction_state(coordinator->transaction));
    return true;
}

JCE_API void JCE_CALL
jce_scene_generation_cancel(JceSceneGenerationCoordinator *coordinator)
{
    if (!coordinator || !status_busy(coordinator->status) ||
        coordinator->status == JCE_SCENE_GENERATION_HEALTH_CHECK)
        return;
    if (coordinator->status == JCE_SCENE_GENERATION_PLANNING)
        jce_ai_scene_director_cancel(coordinator->director);
    else
        jce_scene_transaction_cancel(coordinator->transaction);
    memset(&coordinator->frozen_plan, 0,
           sizeof(coordinator->frozen_plan));
    coordinator->source = JCE_AI_SCENE_SOURCE_NONE;
    coordinator->status = JCE_SCENE_GENERATION_CANCELLED;
}

JCE_API JceSceneGenerationStatus JCE_CALL
jce_scene_generation_status(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator ? coordinator->status : JCE_SCENE_GENERATION_FAILED;
}

JCE_API JceAiScenePlanSource JCE_CALL
jce_scene_generation_source(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator ? coordinator->source : JCE_AI_SCENE_SOURCE_NONE;
}

JCE_API JceAiSceneError JCE_CALL
jce_scene_generation_director_error(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator
        ? jce_ai_scene_director_error(coordinator->director)
        : JCE_AI_SCENE_ERROR_INVALID_ARGUMENT;
}

JCE_API JceSceneTransactionError JCE_CALL
jce_scene_generation_transaction_error(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator
        ? jce_scene_transaction_error(coordinator->transaction)
        : JCE_SCENE_TRANSACTION_ERROR_INVALID_ARGUMENT;
}

JCE_API bool JCE_CALL
jce_scene_generation_frozen_plan(
    const JceSceneGenerationCoordinator *coordinator,
    JceSceneFrozenPlan *out_plan)
{
    if (!coordinator || !out_plan || coordinator->frozen_plan.plan_hash == 0u)
        return false;
    *out_plan = coordinator->frozen_plan;
    return true;
}

JCE_API JceScene *JCE_CALL
jce_scene_generation_active_scene(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator
        ? jce_scene_transaction_active_scene(coordinator->transaction)
        : NULL;
}

JCE_API uint64_t JCE_CALL
jce_scene_generation_active_plan_hash(
    const JceSceneGenerationCoordinator *coordinator)
{
    return coordinator
        ? jce_scene_transaction_active_plan_hash(coordinator->transaction)
        : 0u;
}
