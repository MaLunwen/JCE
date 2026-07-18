/*
 * jce_scene_compiler.h -- Deterministic SceneRecipe to FrozenPlan compiler.
 */

#ifndef JCE_SCENE_COMPILER_H
#define JCE_SCENE_COMPILER_H

#include <jce/middleware/scene/jce_scene_recipe.h>

#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION 2u
#define JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS \
    JCE_SCENE_RECIPE_MAX_TOTAL_INSTANCES
#define JCE_SCENE_FROZEN_PLAN_MAX_BYTES 65536u

typedef struct {
    uint64_t stable_entity_id;
    uint64_t parent_stable_entity_id;
    uint64_t asset_content_hash;
    char stable_role[JCE_SCENE_STABLE_ROLE_MAX];
    char asset_id[JCE_SCENE_ASSET_ID_MAX];
    int32_t position_mm[3];
    int32_t rotation_mdeg[3];
    int32_t scale_milli[3];
} JceScenePlanOperation;

typedef struct {
    uint32_t format_version;
    uint32_t compiler_version;
    uint64_t request_id;
    uint64_t seed;
    uint64_t catalog_hash;
    uint64_t plan_hash;
    uint32_t operation_count;
    JceScenePlanOperation operations[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
} JceSceneFrozenPlan;

typedef struct {
    uint32_t max_placement_attempts;
} JceSceneCompileOptions;

typedef enum {
    JCE_SCENE_COMPILE_OK = 0,
    JCE_SCENE_COMPILE_INVALID_ARGUMENT,
    JCE_SCENE_COMPILE_INVALID_RECIPE,
    JCE_SCENE_COMPILE_INVALID_CATALOG,
    JCE_SCENE_COMPILE_UNKNOWN_CAPABILITY,
    JCE_SCENE_COMPILE_OPERATION_CAPACITY,
    JCE_SCENE_COMPILE_PLACEMENT_FAILED,
    JCE_SCENE_COMPILE_INVALID_HIERARCHY
} JceSceneCompileStatus;

typedef struct {
    JceSceneCompileStatus status;
    uint32_t role_index;
    uint32_t attempt_count;
    JceSceneRecipeError contract_error;
} JceSceneCompileError;

JCE_API void JCE_CALL
jce_scene_compile_options_default(JceSceneCompileOptions *options);

JCE_API JceSceneCompileStatus JCE_CALL
jce_scene_compile(const JceSceneRecipe *recipe,
                  const JceSceneCatalog *catalog,
                  const JceSceneCompileOptions *options,
                  JceSceneFrozenPlan *out_plan,
                  JceSceneCompileError *error);

/* Stable identity over canonical fields, independent of struct padding and
 * host byte order. */
JCE_API uint64_t JCE_CALL
jce_scene_frozen_plan_hash(const JceSceneFrozenPlan *plan);

/* Validates unique stable IDs, resolvable parent IDs, and acyclic topology. */
JCE_API bool JCE_CALL
jce_scene_frozen_plan_graph_validate(const JceSceneFrozenPlan *plan);

/* Writes operation indices in deterministic parent-before-child order. The
 * return value equals operation_count, or zero for an invalid graph / too-small
 * output. An empty valid plan also returns zero. */
JCE_API uint32_t JCE_CALL
jce_scene_frozen_plan_topological_order(const JceSceneFrozenPlan *plan,
                                        uint32_t *out_indices,
                                        uint32_t capacity);

/* Canonical little-endian wire format. Read verifies both structure and hash. */
JCE_API bool JCE_CALL
jce_scene_frozen_plan_write(const JceSceneFrozenPlan *plan,
                            void *buffer, size_t capacity,
                            size_t *out_size);

JCE_API bool JCE_CALL
jce_scene_frozen_plan_read(const void *buffer, size_t size,
                           JceSceneFrozenPlan *out_plan);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_COMPILER_H */
