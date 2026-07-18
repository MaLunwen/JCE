/* Isolated deterministic scene build, activation, health gate, and rollback. */

#include <jce/middleware/scene/jce_scene_transaction.h>

#include <jce/os/core/jce_alloc.h>

#include <string.h>

typedef struct {
    uint64_t stable_entity_id;
    uint64_t parent_stable_entity_id;
    char stable_role[JCE_SCENE_STABLE_ROLE_MAX];
    JceEntity entity;
} RoleBinding;

struct JceSceneTransaction {
    JceSceneTransactionDesc desc;
    JceSceneTransactionState state;
    JceSceneTransactionError error;

    JceScene *active_scene;
    bool active_owned;
    uint64_t active_generation;
    uint64_t active_plan_hash;
    RoleBinding active_bindings[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint32_t active_binding_count;

    JceScene *staging_scene;
    JceSceneFrozenPlan pending_plan;
    uint64_t pending_generation;
    uint32_t next_operation;
    bool pending_hierarchy_applied;
    RoleBinding pending_bindings[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint32_t pending_binding_count;

    JceScene *previous_scene;
    bool previous_owned;
    uint64_t previous_generation;
    uint64_t previous_plan_hash;
    RoleBinding previous_bindings[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint32_t previous_binding_count;

    uint64_t generation_counter;
};

static void release_scene(JceSceneTransaction *transaction, JceScene *scene,
                          bool owned)
{
    if (!scene || !owned)
        return;
    if (transaction->desc.destroy_scene)
        transaction->desc.destroy_scene(transaction->desc.user, scene);
    else
        jce_scene_destroy(scene);
}

static void clear_pending(JceSceneTransaction *transaction)
{
    release_scene(transaction, transaction->staging_scene, true);
    transaction->staging_scene = NULL;
    transaction->pending_generation = 0u;
    transaction->next_operation = 0u;
    transaction->pending_hierarchy_applied = false;
    transaction->pending_binding_count = 0u;
    memset(&transaction->pending_plan, 0, sizeof(transaction->pending_plan));
}

static JceSceneTransactionState fail_transaction(
    JceSceneTransaction *transaction, JceSceneTransactionError error)
{
    clear_pending(transaction);
    transaction->state = JCE_SCENE_TRANSACTION_FAILED;
    transaction->error = error;
    return transaction->state;
}

static bool binding_entity_unique(const RoleBinding *bindings, uint32_t count,
                                  JceEntity entity)
{
    uint32_t i;
    for (i = 0u; i < count; ++i) {
        if (bindings[i].entity == entity)
            return false;
    }
    return true;
}

static const RoleBinding *binding_find(const RoleBinding *bindings,
                                       uint32_t count,
                                       uint64_t stable_entity_id)
{
    uint32_t i;

    for (i = 0u; i < count; ++i) {
        if (bindings[i].stable_entity_id == stable_entity_id)
            return &bindings[i];
    }
    return NULL;
}

static bool apply_pending_hierarchy(JceSceneTransaction *transaction)
{
    uint32_t i;

    if (!jce_scene_frozen_plan_graph_validate(&transaction->pending_plan))
        return false;
    for (i = 0u; i < transaction->pending_binding_count; ++i) {
        const RoleBinding *binding = &transaction->pending_bindings[i];
        JceEntity expected_parent = JCE_ENTITY_INVALID;

        if (binding->parent_stable_entity_id != 0u) {
            const RoleBinding *parent = binding_find(
                transaction->pending_bindings,
                transaction->pending_binding_count,
                binding->parent_stable_entity_id);
            if (!parent || parent->entity == JCE_ENTITY_INVALID)
                return false;
            expected_parent = parent->entity;
        }
        jce_scene_set_parent(transaction->staging_scene, binding->entity,
                             expected_parent);
        if (jce_scene_get_parent(transaction->staging_scene,
                                 binding->entity) != expected_parent)
            return false;
    }
    transaction->pending_hierarchy_applied = true;
    return true;
}

static bool pending_contract_valid(const JceSceneTransaction *transaction)
{
    uint32_t i;

    if (!transaction->pending_hierarchy_applied ||
        !jce_scene_frozen_plan_graph_validate(&transaction->pending_plan) ||
        transaction->pending_plan.plan_hash == 0u ||
        transaction->pending_plan.plan_hash !=
            jce_scene_frozen_plan_hash(&transaction->pending_plan) ||
        transaction->pending_plan.operation_count !=
            transaction->pending_binding_count ||
        transaction->pending_binding_count > transaction->desc.entity_budget)
        return false;
    for (i = 0u; i < transaction->pending_binding_count; ++i) {
        const RoleBinding *binding = &transaction->pending_bindings[i];
        const JceScenePlanOperation *operation =
            &transaction->pending_plan.operations[i];
        if (binding->entity == JCE_ENTITY_INVALID ||
            binding->stable_entity_id != operation->stable_entity_id ||
            binding->parent_stable_entity_id !=
                operation->parent_stable_entity_id ||
            strcmp(binding->stable_role, operation->stable_role) != 0)
            return false;
        if (operation->parent_stable_entity_id == 0u) {
            if (jce_scene_get_parent(transaction->staging_scene,
                                     binding->entity) != JCE_ENTITY_INVALID)
                return false;
        } else {
            const RoleBinding *parent = binding_find(
                transaction->pending_bindings,
                transaction->pending_binding_count,
                operation->parent_stable_entity_id);
            if (!parent ||
                jce_scene_get_parent(transaction->staging_scene,
                                     binding->entity) != parent->entity)
                return false;
        }
    }
    return true;
}

JCE_API void JCE_CALL
jce_scene_transaction_desc_default(JceSceneTransactionDesc *desc)
{
    if (!desc)
        return;
    memset(desc, 0, sizeof(*desc));
    desc->entity_budget = JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS;
    desc->retain_previous_until_healthy = true;
}

JCE_API JceSceneTransaction *JCE_CALL
jce_scene_transaction_create(const JceSceneTransactionDesc *desc)
{
    JceSceneTransactionDesc effective;
    JceSceneTransaction *transaction;

    jce_scene_transaction_desc_default(&effective);
    if (desc)
        effective = *desc;
    if (!effective.apply || effective.entity_budget == 0u ||
        effective.entity_budget > JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS)
        return NULL;
    transaction = (JceSceneTransaction *)jce_malloc(sizeof(*transaction));
    if (!transaction)
        return NULL;
    memset(transaction, 0, sizeof(*transaction));
    transaction->desc = effective;
    transaction->active_scene = effective.initial_scene;
    transaction->active_owned = effective.own_initial_scene;
    transaction->state = effective.initial_scene
        ? JCE_SCENE_TRANSACTION_ACTIVE : JCE_SCENE_TRANSACTION_IDLE;
    return transaction;
}

JCE_API void JCE_CALL
jce_scene_transaction_destroy(JceSceneTransaction *transaction)
{
    if (!transaction)
        return;
    release_scene(transaction, transaction->staging_scene,
                  transaction->staging_scene != NULL);
    if (transaction->previous_scene != transaction->active_scene)
        release_scene(transaction, transaction->previous_scene,
                      transaction->previous_owned);
    release_scene(transaction, transaction->active_scene,
                  transaction->active_owned);
    jce_free(transaction);
}

JCE_API bool JCE_CALL
jce_scene_transaction_begin(JceSceneTransaction *transaction,
                            const JceSceneFrozenPlan *plan)
{
    if (!transaction || !plan)
        return false;
    if (transaction->state == JCE_SCENE_TRANSACTION_BUILDING ||
        transaction->state == JCE_SCENE_TRANSACTION_VALIDATING ||
        transaction->state == JCE_SCENE_TRANSACTION_PREWARMING ||
        transaction->state == JCE_SCENE_TRANSACTION_READY ||
        transaction->state == JCE_SCENE_TRANSACTION_HEALTH_CHECK) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_INVALID_ARGUMENT;
        return false;
    }
    if (plan->format_version != JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION ||
        plan->compiler_version != JCE_SCENE_COMPILER_VERSION ||
        plan->operation_count > JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_STALE_PLAN;
        transaction->state = JCE_SCENE_TRANSACTION_FAILED;
        return false;
    }
    if (!jce_scene_frozen_plan_graph_validate(plan)) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_HIERARCHY;
        transaction->state = JCE_SCENE_TRANSACTION_FAILED;
        return false;
    }
    if (plan->plan_hash == 0u ||
        plan->plan_hash != jce_scene_frozen_plan_hash(plan)) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_STALE_PLAN;
        transaction->state = JCE_SCENE_TRANSACTION_FAILED;
        return false;
    }
    if (plan->operation_count > transaction->desc.entity_budget) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_BUDGET;
        transaction->state = JCE_SCENE_TRANSACTION_FAILED;
        return false;
    }
    clear_pending(transaction);
    transaction->staging_scene = jce_scene_create();
    if (!transaction->staging_scene) {
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_CREATE_SCENE;
        transaction->state = JCE_SCENE_TRANSACTION_FAILED;
        return false;
    }
    ++transaction->generation_counter;
    if (transaction->generation_counter == 0u)
        ++transaction->generation_counter;
    transaction->pending_generation = transaction->generation_counter;
    transaction->pending_plan = *plan;
    transaction->next_operation = 0u;
    transaction->pending_hierarchy_applied = false;
    transaction->pending_binding_count = 0u;
    transaction->error = JCE_SCENE_TRANSACTION_ERROR_NONE;
    transaction->state = JCE_SCENE_TRANSACTION_BUILDING;
    return true;
}

JCE_API JceSceneTransactionState JCE_CALL
jce_scene_transaction_step(JceSceneTransaction *transaction,
                           uint32_t max_operations)
{
    if (!transaction)
        return JCE_SCENE_TRANSACTION_FAILED;
    if (transaction->state == JCE_SCENE_TRANSACTION_BUILDING) {
        uint32_t applied = 0u;
        if (max_operations == 0u)
            max_operations = 1u;
        while (transaction->next_operation <
                   transaction->pending_plan.operation_count &&
               applied < max_operations) {
            uint32_t index = transaction->next_operation;
            const JceScenePlanOperation *operation =
                &transaction->pending_plan.operations[index];
            JceEntity entity = JCE_ENTITY_INVALID;
            RoleBinding *binding;

            if (!transaction->desc.apply(transaction->desc.user,
                                         transaction->staging_scene,
                                         operation, &entity) ||
                entity == JCE_ENTITY_INVALID ||
                !binding_entity_unique(transaction->pending_bindings,
                                       transaction->pending_binding_count,
                                       entity))
                return fail_transaction(transaction,
                                        JCE_SCENE_TRANSACTION_ERROR_APPLY);
            binding = &transaction->pending_bindings[
                transaction->pending_binding_count++];
            memset(binding, 0, sizeof(*binding));
            binding->stable_entity_id = operation->stable_entity_id;
            binding->parent_stable_entity_id =
                operation->parent_stable_entity_id;
            memcpy(binding->stable_role, operation->stable_role,
                   sizeof(binding->stable_role));
            binding->stable_role[sizeof(binding->stable_role) - 1u] = '\0';
            binding->entity = entity;
            ++transaction->next_operation;
            ++applied;
        }
        if (transaction->next_operation ==
            transaction->pending_plan.operation_count) {
            if (!apply_pending_hierarchy(transaction))
                return fail_transaction(
                    transaction, JCE_SCENE_TRANSACTION_ERROR_HIERARCHY);
            transaction->state = JCE_SCENE_TRANSACTION_VALIDATING;
        }
        return transaction->state;
    }
    if (transaction->state == JCE_SCENE_TRANSACTION_VALIDATING) {
        if (!pending_contract_valid(transaction) ||
            (transaction->desc.validate &&
             !transaction->desc.validate(transaction->desc.user,
                                         transaction->staging_scene,
                                         &transaction->pending_plan)))
            return fail_transaction(
                transaction, JCE_SCENE_TRANSACTION_ERROR_VALIDATION);
        transaction->state = JCE_SCENE_TRANSACTION_PREWARMING;
        return transaction->state;
    }
    if (transaction->state == JCE_SCENE_TRANSACTION_PREWARMING) {
        JceScenePrewarmResult result = JCE_SCENE_PREWARM_READY;
        if (transaction->desc.prewarm)
            result = transaction->desc.prewarm(
                transaction->desc.user, transaction->staging_scene,
                &transaction->pending_plan);
        if (result == JCE_SCENE_PREWARM_FAILED)
            return fail_transaction(transaction,
                                    JCE_SCENE_TRANSACTION_ERROR_PREWARM);
        if (result == JCE_SCENE_PREWARM_READY)
            transaction->state = JCE_SCENE_TRANSACTION_READY;
        return transaction->state;
    }
    return transaction->state;
}

JCE_API bool JCE_CALL
jce_scene_transaction_commit(JceSceneTransaction *transaction,
                             uint64_t generation)
{
    if (!transaction || transaction->state != JCE_SCENE_TRANSACTION_READY ||
        generation == 0u || generation != transaction->pending_generation) {
        if (transaction)
            transaction->error = JCE_SCENE_TRANSACTION_ERROR_GENERATION;
        return false;
    }
    if (!pending_contract_valid(transaction)) {
        fail_transaction(transaction,
                         JCE_SCENE_TRANSACTION_ERROR_HIERARCHY);
        return false;
    }

    transaction->previous_scene = transaction->active_scene;
    transaction->previous_owned = transaction->active_owned;
    transaction->previous_generation = transaction->active_generation;
    transaction->previous_plan_hash = transaction->active_plan_hash;
    transaction->previous_binding_count = transaction->active_binding_count;
    memcpy(transaction->previous_bindings, transaction->active_bindings,
           sizeof(transaction->active_bindings));

    transaction->active_scene = transaction->staging_scene;
    transaction->active_owned = true;
    transaction->active_generation = transaction->pending_generation;
    transaction->active_plan_hash = transaction->pending_plan.plan_hash;
    transaction->active_binding_count = transaction->pending_binding_count;
    memcpy(transaction->active_bindings, transaction->pending_bindings,
           sizeof(transaction->active_bindings));

    transaction->staging_scene = NULL;
    transaction->pending_generation = 0u;
    transaction->next_operation = 0u;
    transaction->pending_hierarchy_applied = false;
    transaction->pending_binding_count = 0u;
    memset(&transaction->pending_plan, 0, sizeof(transaction->pending_plan));
    transaction->error = JCE_SCENE_TRANSACTION_ERROR_NONE;

    if (transaction->desc.on_activated)
        transaction->desc.on_activated(
            transaction->desc.user, transaction->active_scene,
            transaction->previous_scene, transaction->active_generation);

    if (transaction->desc.retain_previous_until_healthy) {
        transaction->state = JCE_SCENE_TRANSACTION_HEALTH_CHECK;
    } else {
        release_scene(transaction, transaction->previous_scene,
                      transaction->previous_owned);
        transaction->previous_scene = NULL;
        transaction->previous_owned = false;
        transaction->previous_generation = 0u;
        transaction->previous_plan_hash = 0u;
        transaction->previous_binding_count = 0u;
        transaction->state = JCE_SCENE_TRANSACTION_ACTIVE;
    }
    return true;
}

JCE_API bool JCE_CALL
jce_scene_transaction_mark_healthy(JceSceneTransaction *transaction,
                                   bool healthy)
{
    if (!transaction ||
        transaction->state != JCE_SCENE_TRANSACTION_HEALTH_CHECK)
        return false;
    if (healthy) {
        release_scene(transaction, transaction->previous_scene,
                      transaction->previous_owned);
        transaction->previous_scene = NULL;
        transaction->previous_owned = false;
        transaction->previous_generation = 0u;
        transaction->previous_plan_hash = 0u;
        transaction->previous_binding_count = 0u;
        transaction->state = JCE_SCENE_TRANSACTION_ACTIVE;
        transaction->error = JCE_SCENE_TRANSACTION_ERROR_NONE;
        return true;
    }

    {
        JceScene *failed_scene = transaction->active_scene;
        bool failed_owned = transaction->active_owned;

        transaction->active_scene = transaction->previous_scene;
        transaction->active_owned = transaction->previous_owned;
        transaction->active_generation = transaction->previous_generation;
        transaction->active_plan_hash = transaction->previous_plan_hash;
        transaction->active_binding_count =
            transaction->previous_binding_count;
        memcpy(transaction->active_bindings, transaction->previous_bindings,
               sizeof(transaction->active_bindings));

        transaction->previous_scene = NULL;
        transaction->previous_owned = false;
        transaction->previous_generation = 0u;
        transaction->previous_plan_hash = 0u;
        transaction->previous_binding_count = 0u;

        if (transaction->desc.on_activated)
            transaction->desc.on_activated(
                transaction->desc.user, transaction->active_scene,
                failed_scene, transaction->active_generation);
        release_scene(transaction, failed_scene, failed_owned);
    }
    transaction->error = JCE_SCENE_TRANSACTION_ERROR_HEALTH_CHECK;
    transaction->state = transaction->active_scene
        ? JCE_SCENE_TRANSACTION_ACTIVE : JCE_SCENE_TRANSACTION_FAILED;
    return true;
}

JCE_API void JCE_CALL
jce_scene_transaction_cancel(JceSceneTransaction *transaction)
{
    if (!transaction)
        return;
    if (transaction->state == JCE_SCENE_TRANSACTION_HEALTH_CHECK)
        return;
    if (transaction->state == JCE_SCENE_TRANSACTION_BUILDING ||
        transaction->state == JCE_SCENE_TRANSACTION_VALIDATING ||
        transaction->state == JCE_SCENE_TRANSACTION_PREWARMING ||
        transaction->state == JCE_SCENE_TRANSACTION_READY)
        clear_pending(transaction);
    transaction->error = JCE_SCENE_TRANSACTION_ERROR_NONE;
    transaction->state = transaction->active_scene
        ? JCE_SCENE_TRANSACTION_ACTIVE : JCE_SCENE_TRANSACTION_CANCELLED;
}

JCE_API JceSceneTransactionState JCE_CALL
jce_scene_transaction_state(const JceSceneTransaction *transaction)
{
    return transaction ? transaction->state : JCE_SCENE_TRANSACTION_FAILED;
}

JCE_API JceSceneTransactionError JCE_CALL
jce_scene_transaction_error(const JceSceneTransaction *transaction)
{
    return transaction ? transaction->error
                       : JCE_SCENE_TRANSACTION_ERROR_INVALID_ARGUMENT;
}

JCE_API uint64_t JCE_CALL
jce_scene_transaction_pending_generation(
    const JceSceneTransaction *transaction)
{
    return transaction ? transaction->pending_generation : 0u;
}

JCE_API uint64_t JCE_CALL
jce_scene_transaction_active_generation(
    const JceSceneTransaction *transaction)
{
    return transaction ? transaction->active_generation : 0u;
}

JCE_API JceScene *JCE_CALL
jce_scene_transaction_active_scene(const JceSceneTransaction *transaction)
{
    return transaction ? transaction->active_scene : NULL;
}

JCE_API uint64_t JCE_CALL
jce_scene_transaction_active_plan_hash(
    const JceSceneTransaction *transaction)
{
    return transaction ? transaction->active_plan_hash : 0u;
}

JCE_API uint32_t JCE_CALL
jce_scene_transaction_find_role(const JceSceneTransaction *transaction,
                                const char *stable_role,
                                JceEntity *out_entities,
                                uint32_t capacity)
{
    uint32_t found = 0u;
    uint32_t i;

    if (!transaction || !stable_role || stable_role[0] == '\0')
        return 0u;
    for (i = 0u; i < transaction->active_binding_count; ++i) {
        if (strcmp(transaction->active_bindings[i].stable_role,
                   stable_role) != 0)
            continue;
        if (out_entities && found < capacity)
            out_entities[found] = transaction->active_bindings[i].entity;
        ++found;
    }
    return found;
}

JCE_API JceEntity JCE_CALL
jce_scene_transaction_find_stable_id(
    const JceSceneTransaction *transaction, uint64_t stable_entity_id)
{
    uint32_t i;

    if (!transaction || stable_entity_id == 0u)
        return JCE_ENTITY_INVALID;
    for (i = 0u; i < transaction->active_binding_count; ++i) {
        if (transaction->active_bindings[i].stable_entity_id ==
            stable_entity_id)
            return transaction->active_bindings[i].entity;
    }
    return JCE_ENTITY_INVALID;
}
