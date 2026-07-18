/*
 * jce_scene_recipe.h -- Bounded semantic input for deterministic scenes.
 *
 * AI providers may propose this data, but they never emit ECS operations,
 * asset paths, or scripts. JCE validates and compiles the recipe locally.
 */

#ifndef JCE_SCENE_RECIPE_H
#define JCE_SCENE_RECIPE_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SCENE_RECIPE_SCHEMA_VERSION 2u
#define JCE_SCENE_CATALOG_SCHEMA_VERSION 1u
#define JCE_SCENE_COMPILER_VERSION 2u

#define JCE_SCENE_RECIPE_MAX_ROLES 32u
#define JCE_SCENE_RECIPE_MAX_INSTANCES_PER_ROLE 64u
#define JCE_SCENE_RECIPE_MAX_TOTAL_INSTANCES 256u
#define JCE_SCENE_CATALOG_MAX_ENTRIES 256u
#define JCE_SCENE_STABLE_ROLE_MAX 48u
#define JCE_SCENE_CAPABILITY_MAX 48u
#define JCE_SCENE_ASSET_ID_MAX 80u

typedef enum {
    JCE_SCENE_PLACEMENT_SINGLE = 0,
    JCE_SCENE_PLACEMENT_GRID,
    JCE_SCENE_PLACEMENT_SCATTER,
    JCE_SCENE_PLACEMENT_RING
} JceScenePlacement;

typedef struct {
    char stable_role[JCE_SCENE_STABLE_ROLE_MAX];
    char capability[JCE_SCENE_CAPABILITY_MAX];
    char parent_role[JCE_SCENE_STABLE_ROLE_MAX];
    uint32_t parent_instance;
    uint32_t min_count;
    uint32_t max_count;
    JceScenePlacement placement;
    bool required;
    double center[3];
    double extent[3];
    double min_separation;
    double yaw_min_degrees;
    double yaw_max_degrees;
    double scale_min;
    double scale_max;
} JceSceneRecipeRole;

typedef struct {
    uint32_t schema_version;
    uint32_t compiler_version;
    uint64_t request_id;
    uint64_t seed;
    uint32_t role_count;
    JceSceneRecipeRole roles[JCE_SCENE_RECIPE_MAX_ROLES];
} JceSceneRecipe;

typedef struct {
    char asset_id[JCE_SCENE_ASSET_ID_MAX];
    char capability[JCE_SCENE_CAPABILITY_MAX];
    uint64_t content_hash;
    uint32_t weight;
    bool enabled;
} JceSceneCatalogEntry;

typedef struct {
    uint32_t schema_version;
    uint64_t content_hash;
    uint32_t entry_count;
    JceSceneCatalogEntry entries[JCE_SCENE_CATALOG_MAX_ENTRIES];
} JceSceneCatalog;

typedef enum {
    JCE_SCENE_RECIPE_OK = 0,
    JCE_SCENE_RECIPE_INVALID_ARGUMENT,
    JCE_SCENE_RECIPE_INVALID_JSON,
    JCE_SCENE_RECIPE_UNKNOWN_FIELD,
    JCE_SCENE_RECIPE_UNSUPPORTED_VERSION,
    JCE_SCENE_RECIPE_CAPACITY_EXCEEDED,
    JCE_SCENE_RECIPE_INVALID_TOKEN,
    JCE_SCENE_RECIPE_DUPLICATE_ROLE,
    JCE_SCENE_RECIPE_INVALID_RANGE,
    JCE_SCENE_RECIPE_INVALID_NUMBER,
    JCE_SCENE_RECIPE_INVALID_HIERARCHY,
    JCE_SCENE_RECIPE_INVALID_CATALOG
} JceSceneRecipeStatus;

typedef struct {
    JceSceneRecipeStatus status;
    uint32_t item_index;
    char field[JCE_SCENE_ASSET_ID_MAX];
} JceSceneRecipeError;

JCE_API void JCE_CALL jce_scene_recipe_init(JceSceneRecipe *recipe);
JCE_API void JCE_CALL jce_scene_catalog_init(JceSceneCatalog *catalog);

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_recipe_validate(const JceSceneRecipe *recipe,
                          JceSceneRecipeError *error);

JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_catalog_validate(const JceSceneCatalog *catalog,
                           JceSceneRecipeError *error);

/* Deterministic half-away-from-zero quantisation. `scale` must be positive.
 * `out` is left unchanged on invalid, non-finite, or overflowing input. */
JCE_API bool JCE_CALL jce_scene_recipe_quantize(double value, int32_t scale,
                                                int32_t *out);

/* Strict parser for the provider-neutral JSON wire contract. Unknown fields
 * are rejected so provider drift cannot silently change runtime behaviour. */
JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_recipe_parse_json(const char *json, size_t length,
                            JceSceneRecipe *out_recipe,
                            JceSceneRecipeError *error);

/* Strict authoring/cook parser for the closed semantic asset catalog. Host
 * paths and unknown metadata are rejected at this provider-facing boundary. */
JCE_API JceSceneRecipeStatus JCE_CALL
jce_scene_catalog_parse_json(const char *json, size_t length,
                             JceSceneCatalog *out_catalog,
                             JceSceneRecipeError *error);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_RECIPE_H */
