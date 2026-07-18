/*
 * jce_ai_scene_director.h -- Provider-neutral AI scene orchestration.
 *
 * The transport may call any REST service, but this layer accepts only a
 * bounded semantic SceneRecipe and always compiles it locally into a
 * deterministic FrozenPlan. No provider SDK, credential, host path, script,
 * or raw ECS mutation crosses this API.
 */

#ifndef JCE_AI_SCENE_DIRECTOR_H
#define JCE_AI_SCENE_DIRECTOR_H

#include <jce/middleware/scene/jce_scene_compiler.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_AI_SCENE_PROMPT_MAX_BYTES 32768u
#define JCE_AI_SCENE_RESPONSE_MAX_BYTES 32768u
#define JCE_AI_SCENE_LOCALE_MAX 16u
#define JCE_AI_SCENE_INTENT_MAX 256u
#define JCE_AI_SCENE_RESPONSE_SCHEMA_VERSION 1u

typedef uint64_t JceAiSceneTransportTicket;

typedef struct {
    uint64_t request_id;
    const char *body;
    size_t body_size;
} JceAiSceneTransportRequest;

typedef enum {
    JCE_AI_SCENE_TRANSPORT_PENDING = 0,
    JCE_AI_SCENE_TRANSPORT_COMPLETE,
    JCE_AI_SCENE_TRANSPORT_FAILED
} JceAiSceneTransportState;

typedef bool (JCE_CALL *JceAiSceneTransportSubmitFn)(
    void *user, const JceAiSceneTransportRequest *request,
    JceAiSceneTransportTicket *out_ticket);

typedef JceAiSceneTransportState (JCE_CALL *JceAiSceneTransportPollFn)(
    void *user, JceAiSceneTransportTicket ticket,
    char *response, size_t capacity, size_t *out_size);

typedef void (JCE_CALL *JceAiSceneTransportCancelFn)(
    void *user, JceAiSceneTransportTicket ticket);

typedef struct {
    void *user;
    JceAiSceneTransportSubmitFn submit;
    JceAiSceneTransportPollFn poll;
    JceAiSceneTransportCancelFn cancel;
} JceAiSceneTransport;

typedef struct {
    uint64_t value;
    uint64_t catalog_hash;
    uint64_t seed;
    uint32_t policy_version;
    uint32_t compiler_version;
} JceAiSceneCacheKey;

typedef bool (JCE_CALL *JceAiSceneCacheLoadFn)(
    void *user, const JceAiSceneCacheKey *key,
    void *buffer, size_t capacity, size_t *out_size);

typedef bool (JCE_CALL *JceAiSceneCacheStoreFn)(
    void *user, const JceAiSceneCacheKey *key,
    const void *buffer, size_t size);

typedef struct {
    void *user;
    JceAiSceneCacheLoadFn load;
    JceAiSceneCacheStoreFn store;
} JceAiSceneCache;

typedef struct {
    JceAiSceneTransport transport;
    JceAiSceneCache cache;
    const JceSceneRecipe *bootstrap_recipes;
    uint32_t bootstrap_recipe_count;
    uint32_t policy_version;
    uint32_t max_retries;
    double request_timeout_seconds;
} JceAiSceneDirectorDesc;

typedef struct {
    uint64_t request_id;
    uint64_t seed;
    char locale[JCE_AI_SCENE_LOCALE_MAX];
    char intent[JCE_AI_SCENE_INTENT_MAX];
    bool offline;
} JceAiSceneRequest;

typedef enum {
    JCE_AI_SCENE_DIRECTOR_IDLE = 0,
    JCE_AI_SCENE_DIRECTOR_WAITING,
    JCE_AI_SCENE_DIRECTOR_READY,
    JCE_AI_SCENE_DIRECTOR_FAILED,
    JCE_AI_SCENE_DIRECTOR_CANCELLED
} JceAiSceneDirectorStatus;

typedef enum {
    JCE_AI_SCENE_SOURCE_NONE = 0,
    JCE_AI_SCENE_SOURCE_NETWORK,
    JCE_AI_SCENE_SOURCE_CACHE,
    JCE_AI_SCENE_SOURCE_BOOTSTRAP
} JceAiScenePlanSource;

typedef enum {
    JCE_AI_SCENE_ERROR_NONE = 0,
    JCE_AI_SCENE_ERROR_INVALID_ARGUMENT,
    JCE_AI_SCENE_ERROR_INVALID_CATALOG,
    JCE_AI_SCENE_ERROR_PROMPT_TOO_LARGE,
    JCE_AI_SCENE_ERROR_TRANSPORT,
    JCE_AI_SCENE_ERROR_TIMEOUT,
    JCE_AI_SCENE_ERROR_INVALID_RESPONSE,
    JCE_AI_SCENE_ERROR_STALE_RESPONSE,
    JCE_AI_SCENE_ERROR_INVALID_RECIPE,
    JCE_AI_SCENE_ERROR_COMPILE,
    JCE_AI_SCENE_ERROR_NO_FALLBACK
} JceAiSceneError;

typedef struct JceAiSceneDirector JceAiSceneDirector;

JCE_API void JCE_CALL
jce_ai_scene_director_desc_default(JceAiSceneDirectorDesc *desc);

JCE_API JceAiSceneDirector *JCE_CALL
jce_ai_scene_director_create(const JceAiSceneDirectorDesc *desc);

JCE_API void JCE_CALL
jce_ai_scene_director_destroy(JceAiSceneDirector *director);

/* Copies the request and catalog. Returns true when accepted, including an
 * immediate cache/bootstrap result. */
JCE_API bool JCE_CALL
jce_ai_scene_director_request(JceAiSceneDirector *director,
                              const JceAiSceneRequest *request,
                              const JceSceneCatalog *catalog);

/* Main-thread cooperative poll. Retries reuse the exact request body, ID,
 * seed, catalog hash, and cache key. */
JCE_API void JCE_CALL
jce_ai_scene_director_update(JceAiSceneDirector *director,
                             double delta_seconds);

JCE_API void JCE_CALL
jce_ai_scene_director_cancel(JceAiSceneDirector *director);

JCE_API JceAiSceneDirectorStatus JCE_CALL
jce_ai_scene_director_status(const JceAiSceneDirector *director);

JCE_API JceAiScenePlanSource JCE_CALL
jce_ai_scene_director_source(const JceAiSceneDirector *director);

JCE_API JceAiSceneError JCE_CALL
jce_ai_scene_director_error(const JceAiSceneDirector *director);

JCE_API bool JCE_CALL
jce_ai_scene_director_result(const JceAiSceneDirector *director,
                             JceSceneFrozenPlan *out_plan);

JCE_API const JceAiSceneCacheKey *JCE_CALL
jce_ai_scene_director_cache_key(const JceAiSceneDirector *director);

JCE_EXTERN_C_END

#endif /* JCE_AI_SCENE_DIRECTOR_H */
