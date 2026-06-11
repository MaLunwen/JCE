/*
 * jce_scene.c  ECS scene implementation (flecs backend).
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include "os/core/jce_memory.h"

#include <flecs.h>
#include <string.h>

#define LOG_TAG "scene"

/* Implemented in jce_scene_video.c — installs flecs lifecycle hooks
 * (ctor/dtor/copy/move) on the VideoPlayer component so its decoder handle
 * + GPU texture follow correct ownership across table moves, copies,
 * removal, entity delete, and world fini.  VideoPlayer is the only scene
 * component holding engine-side resources, so it is the only one needing
 * hooks. */
void jce_scene_video_install_hooks(ecs_world_t *world, ecs_entity_t comp_id);

/* Implemented in jce_scene_sequencer.c — same hook contract for the
 * SequencePlayer component, whose runtime blob owns an open JceSequencer
 * (freed on remove / entity delete / scene destroy via the dtor hook). */
void jce_scene_sequencer_install_hooks(ecs_world_t *world, ecs_entity_t comp_id);

/* Defined in jce_scene_particles.c — releases the lazy particle system. */
void jce_scene_particles_shutdown(JceScene *s);

/* ── Component IDs (registered once per world) ─────────────────────── */

/* Internal: per-entity bitmask of DISABLED components (Unity-style enable
 * toggle). One JCE_COMP_FLAG_* bit each. Absent component ⇒ all enabled. */
typedef struct { uint64_t disabled; } JceCompEnableState;

static ECS_COMPONENT_DECLARE(JceCompEnableState);
static ECS_COMPONENT_DECLARE(JceTransform);
static ECS_COMPONENT_DECLARE(JceMeshRenderer);
static ECS_COMPONENT_DECLARE(JceCameraComponent);
static ECS_COMPONENT_DECLARE(JceDirectionalLight);
static ECS_COMPONENT_DECLARE(JcePointLight);
static ECS_COMPONENT_DECLARE(JceSpotLight);
static ECS_COMPONENT_DECLARE(JceTagActive);
static ECS_COMPONENT_DECLARE(JceRigidBodyComponent);
static ECS_COMPONENT_DECLARE(JceRigidBody2DComponent);
static ECS_COMPONENT_DECLARE(JceParticleEmitterComponent);
static ECS_COMPONENT_DECLARE(JceBehaviorTree);
static ECS_COMPONENT_DECLARE(JceSkyboxComponent);
static ECS_COMPONENT_DECLARE(JceSpriteRendererComponent);
static ECS_COMPONENT_DECLARE(JceSpriteAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceSkeletalAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceConstraintComponent);
static ECS_COMPONENT_DECLARE(JceBoxColliderComponent);
static ECS_COMPONENT_DECLARE(JceSphereColliderComponent);
static ECS_COMPONENT_DECLARE(JceCharacterControllerComponent);
static ECS_COMPONENT_DECLARE(JceAudioSourceComponent);
static ECS_COMPONENT_DECLARE(JceVideoPlayerComponent);
static ECS_COMPONENT_DECLARE(JceScriptComponent);
static ECS_COMPONENT_DECLARE(JceEditorMeta);
static ECS_COMPONENT_DECLARE(JceTerrainComponent);
static ECS_COMPONENT_DECLARE(JceLodGroupComponent);
static ECS_COMPONENT_DECLARE(JceVirtualCameraComponent);
static ECS_COMPONENT_DECLARE(JceTriggerVolumeComponent);
static ECS_COMPONENT_DECLARE(JceCapsuleColliderComponent);
static ECS_COMPONENT_DECLARE(JceMeshColliderComponent);
static ECS_COMPONENT_DECLARE(JceCompoundColliderComponent);
static ECS_COMPONENT_DECLARE(JceCollider2DComponent);
static ECS_COMPONENT_DECLARE(JceTrailRendererComponent);
static ECS_COMPONENT_DECLARE(JceLineRendererComponent);
static ECS_COMPONENT_DECLARE(JceReflectionProbeComponent);
static ECS_COMPONENT_DECLARE(JceDecalComponent);
static ECS_COMPONENT_DECLARE(JceLightProbeGroupComponent);
static ECS_COMPONENT_DECLARE(JceAudioListenerComponent);
static ECS_COMPONENT_DECLARE(JceAudioReverbZoneComponent);
static ECS_COMPONENT_DECLARE(JceAudioOcclusionComponent);
static ECS_COMPONENT_DECLARE(JceSpawnManagerComponent);
static ECS_COMPONENT_DECLARE(JceWeaponComponent);
static ECS_COMPONENT_DECLARE(JceSavePointComponent);
static ECS_COMPONENT_DECLARE(JceWheelColliderComponent);
static ECS_COMPONENT_DECLARE(JceConstantForceComponent);
static ECS_COMPONENT_DECLARE(JceConfigurableJointComponent);
static ECS_COMPONENT_DECLARE(JceJoint2DComponent);
static ECS_COMPONENT_DECLARE(JceBillboardRendererComponent);
static ECS_COMPONENT_DECLARE(JceCanvasComponent);
static ECS_COMPONENT_DECLARE(JceCanvasGroupComponent);
static ECS_COMPONENT_DECLARE(JceLayoutGroupComponent);
static ECS_COMPONENT_DECLARE(JceUIImageComponent);
static ECS_COMPONENT_DECLARE(JceUITextComponent);
static ECS_COMPONENT_DECLARE(JceUIButtonComponent);
static ECS_COMPONENT_DECLARE(JceNetworkObjectComponent);
static ECS_COMPONENT_DECLARE(JceClothComponent);
static ECS_COMPONENT_DECLARE(JceNetTransformComponent);
static ECS_COMPONENT_DECLARE(JceNetAnimatorComponent);
static ECS_COMPONENT_DECLARE(JceNetRigidbodyComponent);
static ECS_COMPONENT_DECLARE(JceVfxGraphComponent);
static ECS_COMPONENT_DECLARE(JceTilemapComponent);
static ECS_COMPONENT_DECLARE(JceTilemapCollider2DComponent);
static ECS_COMPONENT_DECLARE(JceAvatarComponent);
static ECS_COMPONENT_DECLARE(JceTagComponent);
static ECS_COMPONENT_DECLARE(JceLayerComponent);
static ECS_COMPONENT_DECLARE(JceVolumeComponent);
static ECS_COMPONENT_DECLARE(JceOcclusionPortalComponent);
static ECS_COMPONENT_DECLARE(JceNavAgentComponent);
static ECS_COMPONENT_DECLARE(JceIkConstraintComponent);
static ECS_COMPONENT_DECLARE(JceSequencePlayerComponent);

/* ── Internal world-matrix cache (side table) ──────────────────────────
 *
 * Per-entity memo of the composed world matrix (world = parent_world *
 * local) plus the frame epoch it was computed for. This is deliberately a
 * scene-owned open-addressing hash keyed by the flecs entity id, NOT a
 * flecs component: jce_scene_get_world_matrix is called from inside live
 * flecs query iterations (e.g. the runtime's rt_pick_primary_cam via
 * jce_scene_each_entity), and adding a component to an entity mid-iteration
 * would move it to a new table and corrupt the iterator. A side table has
 * zero interaction with flecs table layout, so it is safe everywhere.
 *
 * The cache is never serialized, flagged, or otherwise observable — it is
 * pure caching state. jce_scene_get_world_matrix recomputes an entity (and
 * memoizes each ancestor it visits) only when the cached epoch differs from
 * the scene's current world_epoch; jce_scene_invalidate_world_cache (called
 * by jce_scene_update each tick, by the scene renderer / pick pass at the
 * top of each render, and on reparent) bumps world_epoch and empties the
 * table to start a fresh generation. Within one frame each entity's world
 * matrix is therefore composed exactly once — a parent shared by K children
 * is no longer recomposed K times — turning the old O(N*depth) per-frame
 * rebuild into O(N). */
typedef struct {
    uint64_t key;     /* entity id; 0 = empty slot */
    uint64_t epoch;   /* world_epoch this `world` was computed for */
    jce_mat4 world;
} JceWorldCacheSlot;

typedef struct {
    JceWorldCacheSlot *slots;
    uint32_t           cap;       /* power of two; 0 until first use */
    uint32_t           live;      /* occupied slots (current+stale entries) */
} JceWorldCache;

/* ── Scene struct ──────────────────────────────────────────────────── */

struct JceScene {
    ecs_world_t *world;
    JceSceneRenderingSettings rendering_settings;
    bool has_rendering_settings;
    /* Lazily heap-allocated (~70 KB with the full chunk table); NULL until
       authored.  Presence of the pointer == "has streaming settings". */
    JceSceneStreamingSettings *streaming_settings;
    ecs_query_t *cloth_query;   /* cached; created lazily in jce_scene_update */
    ecs_query_t *each_query;    /* cached; created lazily in jce_scene_each_entity
                                 * (leak fix: was ecs_query()+ecs_query_fini() on
                                 * EVERY call, 4-5x/frame in Play → unbounded) */
    uint64_t      world_epoch;  /* bumped per frame to invalidate the world-matrix cache */
    JceWorldCache world_cache;  /* per-entity world matrix memo (side table) */
    void         *particles;    /* JceParticleSystem* (lazy; owned by jce_scene_particles.c) */
};

/* Internal storage hooks for jce_scene_particles.c (same translation unit
 * cannot see `struct JceScene`).  Kept off the public API surface. */
void  *jce_scene_internal_particles_get(const JceScene *s)
{
    return s ? s->particles : NULL;
}

void jce_scene_internal_particles_set(JceScene *s, void *sys)
{
    if (s) s->particles = sys;
}

static void scene_rendering_settings_sanitize(JceSceneRenderingSettings *r)
{
    if (!r) return;

    r->version = 1u;

    if (r->ambient_intensity < 0.0f)
        r->ambient_intensity = 0.0f;

    if (!r->fog_enabled || r->fog_mode == JCE_SCENE_FOG_NONE) {
        r->fog_enabled = false;
        r->fog_mode = JCE_SCENE_FOG_NONE;
    } else if (r->fog_mode < JCE_SCENE_FOG_LINEAR ||
               r->fog_mode > JCE_SCENE_FOG_EXP2) {
        r->fog_mode = JCE_SCENE_FOG_EXP;
    }
    if (r->fog_density < 0.0f)
        r->fog_density = 0.0f;
    if (r->fog_end < r->fog_start)
        r->fog_end = r->fog_start;
    if (r->fog_height_falloff < 0.0f)
        r->fog_height_falloff = 0.0f;

    if (r->shadow_distance < 0.0f)
        r->shadow_distance = 0.0f;
    if (r->cascade_count < 1)
        r->cascade_count = 1;
    if (r->cascade_count > 4)
        r->cascade_count = 4;
    if (r->split_lambda < 0.0f)
        r->split_lambda = 0.0f;
    if (r->split_lambda > 1.0f)
        r->split_lambda = 1.0f;
    if (r->shadow_resolution < 512)
        r->shadow_resolution = 512;
    if (r->shadow_resolution > 4096)
        r->shadow_resolution = 4096;
    if (r->soft_shadow_mode < JCE_SCENE_SOFT_SHADOW_OFF ||
        r->soft_shadow_mode > JCE_SCENE_SOFT_SHADOW_VSM)
        r->soft_shadow_mode = JCE_SCENE_SOFT_SHADOW_PCF;

    if (r->exposure < 0.0f)
        r->exposure = 0.0f;
    if (r->gamma <= 0.0f)
        r->gamma = 2.2f;

    /* Time-of-day clamps (wrap hour into [0,24) without pulling in math.h;
     * authored values are small so a bounded loop is fine). */
    while (r->tod_hour >= 24.0f) r->tod_hour -= 24.0f;
    while (r->tod_hour < 0.0f)   r->tod_hour += 24.0f;
    if (r->tod_speed < 0.0f)
        r->tod_speed = 0.0f;
    if (r->tod_dawn_hour <= 0.0f && r->tod_dusk_hour <= 0.0f) {
        r->tod_dawn_hour = 6.0f;
        r->tod_dusk_hour = 18.0f;
    }

    /* Weather clamps. */
    if (r->weather_type < 0 || r->weather_type > 2)
        r->weather_type = 0;
    if (r->weather_intensity < 0.0f)
        r->weather_intensity = 0.0f;
    if (r->weather_intensity > 1.0f)
        r->weather_intensity = 1.0f;
}

JceSceneRenderingSettings jce_scene_rendering_settings_default(void)
{
    JceSceneRenderingSettings r;
    memset(&r, 0, sizeof(r));

    r.version = 1u;
    r.ambient_color[0] = 0.1f;
    r.ambient_color[1] = 0.1f;
    r.ambient_color[2] = 0.12f;
    r.ambient_intensity = 1.0f;

    r.fog_enabled = false;
    r.fog_mode = JCE_SCENE_FOG_NONE;
    r.fog_color[0] = 0.7f;
    r.fog_color[1] = 0.75f;
    r.fog_color[2] = 0.85f;
    r.fog_density = 0.02f;
    r.fog_start = 10.0f;
    r.fog_end = 200.0f;
    r.fog_height_falloff = 0.05f;
    r.fog_height_origin = 0.0f;

    r.shadow_distance = 100.0f;
    r.cascade_count = 4;
    r.split_lambda = 0.7f;
    r.shadow_resolution = 2048;
    r.soft_shadow_mode = JCE_SCENE_SOFT_SHADOW_PCF;

    r.exposure = 1.0f;
    r.gamma = 2.2f;
    r.bloom_threshold = 1.0f;
    r.bloom_intensity = 0.5f;
    r.fxaa_span_max = 8.0f;
    r.vignette_intensity = 0.3f;
    r.vignette_smoothness = 2.0f;
    r.chromatic_strength = 0.005f;

    /* Time-of-day (off by default; renderer keeps its hardcoded sky). */
    r.tod_enabled    = false;
    r.tod_hour       = 12.0f;
    r.tod_speed      = 0.0f;
    r.tod_latitude   = 35.0f;
    r.tod_dawn_hour  = 6.0f;
    r.tod_dusk_hour  = 18.0f;

    /* Weather (clear by default → overlay is a no-op). */
    r.weather_type      = 0;     /* JCE_WEATHER_CLEAR */
    r.weather_intensity = 0.0f;
    return r;
}

bool jce_scene_has_rendering_settings(const JceScene *s)
{
    return s && s->has_rendering_settings;
}

void jce_scene_set_rendering_settings(JceScene *s,
                                      const JceSceneRenderingSettings *settings)
{
    if (!s || !settings) return;
    s->rendering_settings = *settings;
    scene_rendering_settings_sanitize(&s->rendering_settings);
    s->has_rendering_settings = true;
}

const JceSceneRenderingSettings *jce_scene_get_rendering_settings(
    const JceScene *s)
{
    if (!s || !s->has_rendering_settings) return NULL;
    return &s->rendering_settings;
}

JceSceneRenderingSettings *jce_scene_get_rendering_settings_mut(JceScene *s)
{
    if (!s) return NULL;
    if (!s->has_rendering_settings) {
        s->rendering_settings = jce_scene_rendering_settings_default();
        s->has_rendering_settings = true;
    }
    return &s->rendering_settings;
}

void jce_scene_clear_rendering_settings(JceScene *s)
{
    if (!s) return;
    s->rendering_settings = jce_scene_rendering_settings_default();
    s->has_rendering_settings = false;
}

/* ── Scene-level world-streaming settings ──────────────────────────── */

static void scene_streaming_settings_sanitize(JceSceneStreamingSettings *st)
{
    if (!st) return;

    st->version = 1u;

    if (st->mode < 0 || st->mode > 1)
        st->mode = 0;                       /* radial */

    if (st->load_radius < 1.0f)
        st->load_radius = 1.0f;
    if (st->unload_radius < st->load_radius)
        st->unload_radius = st->load_radius;

    if (st->max_pending == 0)
        st->max_pending = 1;
    if (st->frame_budget_ms <= 0.0f)
        st->frame_budget_ms = 2.0f;

    if (st->chunk_count > JCE_SCENE_MAX_STREAM_CHUNKS)
        st->chunk_count = JCE_SCENE_MAX_STREAM_CHUNKS;
    for (uint32_t i = 0; i < st->chunk_count; ++i) {
        JceSceneStreamChunk *c = &st->chunks[i];
        if (c->radius < 0.0f)
            c->radius = 0.0f;
        c->path[sizeof(c->path) - 1] = '\0';
    }
}

JceSceneStreamingSettings jce_scene_streaming_settings_default(void)
{
    JceSceneStreamingSettings st;
    memset(&st, 0, sizeof(st));

    st.version         = 1u;
    st.enabled         = false;
    st.mode            = 0;        /* radial */
    /* Mirror jce_world_stream_config_default() so an authored-then-enabled
       scene behaves like the streamer's own GTA-VC-scale defaults. */
    st.load_radius     = 150.0f;
    st.unload_radius   = 200.0f;
    st.max_pending     = 4;
    st.budget_mb       = 256;
    st.frame_budget_ms = 2.0f;
    st.chunk_count     = 0;
    return st;
}

bool jce_scene_has_streaming_settings(const JceScene *s)
{
    return s && s->streaming_settings != NULL;
}

static JceSceneStreamingSettings *scene_streaming_settings_ensure(JceScene *s)
{
    if (!s) return NULL;
    if (!s->streaming_settings) {
        s->streaming_settings = (JceSceneStreamingSettings *)
            JCE_MALLOC(sizeof(*s->streaming_settings));
        if (!s->streaming_settings) return NULL;
        *s->streaming_settings = jce_scene_streaming_settings_default();
    }
    return s->streaming_settings;
}

void jce_scene_set_streaming_settings(JceScene *s,
                                      const JceSceneStreamingSettings *settings)
{
    if (!s || !settings) return;
    JceSceneStreamingSettings *dst = scene_streaming_settings_ensure(s);
    if (!dst) return;
    *dst = *settings;
    scene_streaming_settings_sanitize(dst);
}

const JceSceneStreamingSettings *jce_scene_get_streaming_settings(
    const JceScene *s)
{
    return s ? s->streaming_settings : NULL;
}

JceSceneStreamingSettings *jce_scene_get_streaming_settings_mut(JceScene *s)
{
    return scene_streaming_settings_ensure(s);
}

void jce_scene_clear_streaming_settings(JceScene *s)
{
    if (!s || !s->streaming_settings) return;
    JCE_FREE(s->streaming_settings);
    s->streaming_settings = NULL;
}

/* ── Create / destroy ──────────────────────────────────────────────── */

JceScene *jce_scene_create(void)
{
    JceScene *s = (JceScene *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;

    s->world = ecs_init();
    if (!s->world) {
        JCE_FREE(s);
        return NULL;
    }
    s->rendering_settings = jce_scene_rendering_settings_default();
    s->has_rendering_settings = false;
    /* Start at 1 so a freshly zeroed cache slot (epoch 0) is always seen as
       stale on first access. world_cache stays zero-init until first use. */
    s->world_epoch = 1;

    /* Register components. */
    ECS_COMPONENT_DEFINE(s->world, JceCompEnableState);
    ECS_COMPONENT_DEFINE(s->world, JceTransform);
    ECS_COMPONENT_DEFINE(s->world, JceMeshRenderer);
    ECS_COMPONENT_DEFINE(s->world, JceCameraComponent);
    ECS_COMPONENT_DEFINE(s->world, JceDirectionalLight);
    ECS_COMPONENT_DEFINE(s->world, JcePointLight);
    ECS_COMPONENT_DEFINE(s->world, JceSpotLight);
    ECS_COMPONENT_DEFINE(s->world, JceTagActive);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBodyComponent);
    ECS_COMPONENT_DEFINE(s->world, JceRigidBody2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceParticleEmitterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBehaviorTree);
    ECS_COMPONENT_DEFINE(s->world, JceSkyboxComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpriteRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpriteAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSkeletalAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConstraintComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBoxColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSphereColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCharacterControllerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioSourceComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVideoPlayerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceScriptComponent);
    ECS_COMPONENT_DEFINE(s->world, JceEditorMeta);
    ECS_COMPONENT_DEFINE(s->world, JceTerrainComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLodGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVirtualCameraComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTriggerVolumeComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCapsuleColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceMeshColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCompoundColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCollider2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTrailRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLineRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceReflectionProbeComponent);
    ECS_COMPONENT_DEFINE(s->world, JceDecalComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLightProbeGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioListenerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioReverbZoneComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAudioOcclusionComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSpawnManagerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceWeaponComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSavePointComponent);
    ECS_COMPONENT_DEFINE(s->world, JceWheelColliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConstantForceComponent);
    ECS_COMPONENT_DEFINE(s->world, JceConfigurableJointComponent);
    ECS_COMPONENT_DEFINE(s->world, JceJoint2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBillboardRendererComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCanvasComponent);
    ECS_COMPONENT_DEFINE(s->world, JceCanvasGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLayoutGroupComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIImageComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUITextComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIButtonComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNetworkObjectComponent);
    ECS_COMPONENT_DEFINE(s->world, JceClothComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNetTransformComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNetAnimatorComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNetRigidbodyComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVfxGraphComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTilemapComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTilemapCollider2DComponent);
    ECS_COMPONENT_DEFINE(s->world, JceAvatarComponent);
    ECS_COMPONENT_DEFINE(s->world, JceTagComponent);
    ECS_COMPONENT_DEFINE(s->world, JceLayerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVolumeComponent);
    ECS_COMPONENT_DEFINE(s->world, JceOcclusionPortalComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNavAgentComponent);
    ECS_COMPONENT_DEFINE(s->world, JceIkConstraintComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSequencePlayerComponent);

    /* VideoPlayer owns a live decoder handle + a GPU texture; install
     * lifecycle hooks so those resources follow correct ownership across
     * table moves / copies / removal / entity delete / world fini. */
    jce_scene_video_install_hooks(s->world, ecs_id(JceVideoPlayerComponent));
    /* SequencePlayer owns an open JceSequencer (runtime blob); same hook
     * contract as VideoPlayer so the handle is freed on component remove,
     * entity delete, and scene destroy (world fini). */
    jce_scene_sequencer_install_hooks(s->world,
                                      ecs_id(JceSequencePlayerComponent));

    LOG_SUCCESS(LOG_TAG, "scene created");
    return s;
}

void jce_scene_destroy(JceScene *s)
{
    if (!s) return;
    jce_scene_particles_shutdown(s);                    /* free lazy particle sys */
    if (s->cloth_query) ecs_query_fini(s->cloth_query); /* before world fini */
    if (s->each_query)  ecs_query_fini(s->each_query);  /* before world fini */
    if (s->world) ecs_fini(s->world);
    if (s->world_cache.slots) JCE_FREE(s->world_cache.slots);
    jce_scene_clear_streaming_settings(s);   /* frees the lazy heap block */
    JCE_FREE(s);
    LOG_INFO(LOG_TAG, "scene destroyed");
}

int jce_scene_clear(JceScene *s)
{
    JCE_PROFILE_ZONE_N("Scene::Clear");
    if (!s || !s->world) { JCE_PROFILE_ZONE_END; return -1; }

    /*
     * Collect all user-entity ids first, then delete them after the query
     * is finalized. This avoids the undefined behaviour of mutating world
     * entity tables while a query iterator is live.
     *
     * We treat "presence of JceTransform" as the canonical "user entity"
     * marker, mirroring jce_scene_each_entity() and the implicit contract
     * established by jce_scene_create_entity() (which always installs one).
     */
    enum { CHUNK = 256 };
    ecs_entity_t  stack_buf[CHUNK];
    ecs_entity_t *ids    = stack_buf;
    int           count  = 0;
    int           cap    = CHUNK;

    ecs_query_t *q = ecs_query(s->world, {
        .terms = {{ .id = ecs_id(JceTransform) }},
    });
    if (!q) { JCE_PROFILE_ZONE_END; return -1; }

    ecs_iter_t it = ecs_query_iter(s->world, q);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            if (count == cap) {
                int new_cap = cap * 2;
                ecs_entity_t *grown = (ecs_entity_t *)JCE_CALLOC(
                    (size_t)new_cap, sizeof(ecs_entity_t));
                if (!grown) {
                    if (ids != stack_buf) JCE_FREE(ids);
                    ecs_query_fini(q);
                    JCE_PROFILE_ZONE_END;
                    return -1;
                }
                memcpy(grown, ids, (size_t)count * sizeof(ecs_entity_t));
                if (ids != stack_buf) JCE_FREE(ids);
                ids = grown;
                cap = new_cap;
            }
            ids[count++] = it.entities[i];
        }
    }
    ecs_query_fini(q);

    /*
     * Delete inside a deferred block so child-of relationships and observer
     * callbacks see a consistent view of the world. flecs cascades deletes
     * across (ChildOf, ...) automatically, so deleting parents implicitly
     * removes their children; the duplicate ecs_delete on an already-dead
     * child is a no-op.
     */
    ecs_defer_begin(s->world);
    for (int i = 0; i < count; i++) {
        if (ecs_is_alive(s->world, ids[i])) {
            ecs_delete(s->world, ids[i]);
        }
    }
    ecs_defer_end(s->world);

    if (ids != stack_buf) JCE_FREE(ids);

    jce_scene_clear_rendering_settings(s);
    jce_scene_clear_streaming_settings(s);
    /* Drop any cached world matrices for the now-deleted entities. */
    jce_scene_invalidate_world_cache(s);

    LOG_INFO(LOG_TAG, "scene cleared (%d entities)", count);
    JCE_PROFILE_ZONE_END;
    return count;
}

/* ── Entity management ─────────────────────────────────────────────── */

JceEntity jce_scene_create_entity(JceScene *s, const char *name)
{
    if (!s) return JCE_ENTITY_INVALID;

    ecs_entity_t e = ecs_new(s->world);
    if (name && name[0])
        ecs_set_name(s->world, e, name);

    /* Default transform. */
    JceTransform t;
    t.position = jce_v3(0, 0, 0);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1, 1, 1);
    ecs_set_ptr(s->world, e, JceTransform, &t);

    /* Active by default. */
    ecs_add(s->world, e, JceTagActive);

    return (JceEntity)e;
}

void jce_scene_destroy_entity(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_delete(s->world, (ecs_entity_t)e);
}

const char *jce_scene_entity_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return "(none)";
    const char *name = ecs_get_name(s->world, (ecs_entity_t)e);
    return name ? name : "(unnamed)";
}

const char *jce_scene_entity_registered_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return NULL;
    return ecs_get_name(s->world, (ecs_entity_t)e);
}

void jce_scene_set_entity_name(JceScene *s, JceEntity e, const char *name)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_set_name(s->world, (ecs_entity_t)e, name);
}

/* ── Parent / child hierarchy ──────────────────────────────────────── */

void jce_scene_set_parent(JceScene *s, JceEntity child, JceEntity parent)
{
    if (!s || child == JCE_ENTITY_INVALID) return;
    if (parent == JCE_ENTITY_INVALID) {
        /* Remove parent (make root entity). */
        ecs_entity_t cur = ecs_get_parent(s->world, (ecs_entity_t)child);
        if (cur) ecs_remove_pair(s->world, (ecs_entity_t)child, EcsChildOf, cur);
    } else {
        ecs_add_pair(s->world, (ecs_entity_t)child,
                     EcsChildOf, (ecs_entity_t)parent);
    }
    /* Reparent changes the world matrix of `child` and its whole subtree.
       Drop the cache so a same-frame re-read (e.g. the editor's reparent
       back-solve that captures child world, reparents, then reads the new
       parent's world) never returns a pre-reparent matrix. */
    jce_scene_invalidate_world_cache(s);
}

JceEntity jce_scene_get_parent(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return JCE_ENTITY_INVALID;
    ecs_entity_t p = ecs_get_parent(s->world, (ecs_entity_t)e);
    return (JceEntity)p;
}

/* Local TRS matrix for one entity (scale-0 components default to 1). */
static jce_mat4 scene_local_matrix(JceScene *s, JceEntity e)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    if (!t) return jce_m4_identity();
    return jce_m4_from_trs(t->position, t->rotation,
                           jce_v3_safe_scale(t->scale));
}

/* ── World-matrix cache side table (open addressing, linear probe) ──── */

/* Splitmix64 finalizer — disperses flecs entity ids (which carry a
   generation in the high bits) well for power-of-two masking. */
static uint64_t scene_world_cache_hash(uint64_t x)
{
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Look up `key` without mutating the table. Returns the slot if present
   (key already stored), else NULL. Never grows, so the returned pointer is
   only valid until the next scene_world_cache_put(). */
static JceWorldCacheSlot *scene_world_cache_find(JceWorldCache *wc, uint64_t key)
{
    if (!wc->slots) return NULL;
    uint32_t mask = wc->cap - 1;
    uint32_t i = (uint32_t)(scene_world_cache_hash(key)) & mask;
    while (wc->slots[i].key) {
        if (wc->slots[i].key == key) return &wc->slots[i];
        i = (i + 1) & mask;
    }
    return NULL;
}

/* Insert/overwrite the cached world matrix for `key` at the current epoch.
   Grows + rehashes as needed (which invalidates any slot pointer obtained
   before this call). Silently no-ops on allocation failure. */
static void scene_world_cache_put(JceWorldCache *wc, uint64_t key,
                                  const jce_mat4 *world, uint64_t epoch)
{
    /* Grow when load factor would exceed ~0.75 (or on first use). */
    if (wc->cap == 0 || (wc->live + 1) * 4 >= wc->cap * 3) {
        uint32_t new_cap = wc->cap ? wc->cap * 2 : 256;
        JceWorldCacheSlot *ns =
            (JceWorldCacheSlot *)JCE_CALLOC(new_cap, sizeof(*ns));
        if (!ns) return;          /* OOM → skip caching this entity */
        if (wc->slots) {
            uint32_t nmask = new_cap - 1;
            for (uint32_t i = 0; i < wc->cap; i++) {
                uint64_t k = wc->slots[i].key;
                if (!k) continue;
                uint32_t j = (uint32_t)(scene_world_cache_hash(k)) & nmask;
                while (ns[j].key) j = (j + 1) & nmask;
                ns[j] = wc->slots[i];
            }
            JCE_FREE(wc->slots);
        }
        wc->slots = ns;
        wc->cap   = new_cap;
    }

    uint32_t mask = wc->cap - 1;
    uint32_t i = (uint32_t)(scene_world_cache_hash(key)) & mask;
    while (wc->slots[i].key && wc->slots[i].key != key)
        i = (i + 1) & mask;
    if (!wc->slots[i].key) {
        wc->slots[i].key = key;
        wc->live++;
    }
    wc->slots[i].world = *world;
    wc->slots[i].epoch = epoch;
}

/* Begin a new cache generation. Keeps the allocated table (avoids realloc
   churn on static scenes) but empties it, which both invalidates last
   frame's matrices and reclaims slots of since-destroyed entities so the
   table cannot grow without bound. Cheap: one memset of `cap` slots, far
   less than the O(N) world-matrix composition it guards. */
void jce_scene_invalidate_world_cache(JceScene *s)
{
    if (!s) return;
    s->world_epoch++;
    if (s->world_cache.slots && s->world_cache.live) {
        memset(s->world_cache.slots, 0,
               (size_t)s->world_cache.cap * sizeof(JceWorldCacheSlot));
        s->world_cache.live = 0;
    }
}

/* Memoized world matrix for one entity within the current frame.
 *
 * world = parent_world * local. The parent's world matrix comes from the
 * same memo (computed once, then cached on the parent), so a parent shared
 * by K children is composed once per frame instead of K times — that is the
 * O(N*depth) → O(N) win this cache exists for. A root (no parent) returns
 * its local matrix unchanged, so flat scenes are bit-identical to the
 * pre-cache behaviour. `depth` bounds accidental cycles (matches the prior
 * 32-deep ancestor walk).
 *
 * Entries are tagged with ms->world_epoch; a slot whose epoch != the current
 * epoch is treated as stale and recomputed, so the per-frame epoch bump in
 * jce_scene_update invalidates the whole scene in O(1). The find/put split
 * keeps the early-out lookup free of table growth; the put after the
 * recursion is the only mutation, so no slot pointer is held across a
 * potential rehash. */
static jce_mat4 scene_world_matrix_memo(JceScene *ms, JceEntity e, int depth)
{
    jce_mat4 local = scene_local_matrix(ms, e);

    JceEntity p = jce_scene_get_parent((const JceScene *)ms, e);
    if (p == JCE_ENTITY_INVALID || p == e || depth >= 32)
        return local;             /* root (or cycle/limit guard) */

    /* Reuse this frame's cached world matrix if present and current. */
    JceWorldCacheSlot *hit = scene_world_cache_find(&ms->world_cache, (uint64_t)e);
    if (hit && hit->epoch == ms->world_epoch)
        return hit->world;

    jce_mat4 parent_world = scene_world_matrix_memo(ms, p, depth + 1);
    jce_mat4 world = jce_m4_multiply(&parent_world, &local);

    scene_world_cache_put(&ms->world_cache, (uint64_t)e, &world,
                          ms->world_epoch);
    return world;
}

jce_mat4 jce_scene_get_world_matrix(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return jce_m4_identity();

    JceScene *ms = (JceScene *)s; /* getters are non-const but read-only here */
    return scene_world_matrix_memo(ms, e, 0);
}

int jce_scene_get_children(const JceScene *s, JceEntity parent,
                           JceEntity *out, int max_out)
{
    if (!s || parent == JCE_ENTITY_INVALID || !out || max_out <= 0) return 0;

    /* ecs_children() iterates (ChildOf, parent) directly without building and
       destroying a transient query — this runs per visible hierarchy node
       every editor frame. The loop still drains the iterator fully (flecs
       requires it) even after the output buffer fills. */
    int count = 0;
    ecs_iter_t it = ecs_children(s->world, (ecs_entity_t)parent);
    while (ecs_children_next(&it)) {
        for (int i = 0; i < it.count && count < max_out; i++) {
            out[count++] = (JceEntity)it.entities[i];
        }
    }
    return count;
}

int jce_scene_get_child_count(const JceScene *s, JceEntity parent)
{
    if (!s || parent == JCE_ENTITY_INVALID) return 0;
    /* Direct pair count — no per-call query alloc/iterate/fini. Called per
       visible hierarchy node every editor frame. */
    return (int)ecs_count_id(s->world, ecs_pair(EcsChildOf, (ecs_entity_t)parent));
}

/* ── Component setters / getters / has / remove (macro-generated) ─── */

#define JCE_COMP_IMPL(TYPE, NAME)                                       \
void jce_scene_set_##NAME(JceScene *s, JceEntity e, const TYPE *v)      \
{                                                                       \
    if (!s || !v) return;                                               \
    ecs_set_ptr(s->world, (ecs_entity_t)e, TYPE, v);                    \
}                                                                       \
                                                                        \
TYPE *jce_scene_get_##NAME(JceScene *s, JceEntity e)                    \
{                                                                       \
    if (!s) return NULL;                                                \
    return (TYPE *)ecs_get_mut(s->world, (ecs_entity_t)e, TYPE);        \
}                                                                       \
                                                                        \
bool jce_scene_has_##NAME(const JceScene *s, JceEntity e)               \
{                                                                       \
    if (!s) return false;                                               \
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return false;         \
    return ecs_has(s->world, (ecs_entity_t)e, TYPE);                    \
}                                                                       \
                                                                        \
void jce_scene_remove_##NAME(JceScene *s, JceEntity e)                  \
{                                                                       \
    if (!s) return;                                                     \
    ecs_remove(s->world, (ecs_entity_t)e, TYPE);                        \
}

JCE_COMP_IMPL(JceTransform,                   transform)
JCE_COMP_IMPL(JceMeshRenderer,                mesh_renderer)
JCE_COMP_IMPL(JceCameraComponent,             camera)
JCE_COMP_IMPL(JceDirectionalLight,            dir_light)
JCE_COMP_IMPL(JcePointLight,                  point_light)
JCE_COMP_IMPL(JceSpotLight,                   spot_light)
JCE_COMP_IMPL(JceSkyboxComponent,             skybox)
JCE_COMP_IMPL(JceSpriteRendererComponent,     sprite_renderer)
JCE_COMP_IMPL(JceSpriteAnimatorComponent,     sprite_animator)
JCE_COMP_IMPL(JceAnimatorComponent,           animator)
JCE_COMP_IMPL(JceSkeletalAnimatorComponent,   skeletal_animator)
JCE_COMP_IMPL(JceConstraintComponent,         constraint)
JCE_COMP_IMPL(JceRigidBodyComponent,          rigidbody)
JCE_COMP_IMPL(JceRigidBody2DComponent,        rigidbody2d)
JCE_COMP_IMPL(JceBoxColliderComponent,        box_collider)
JCE_COMP_IMPL(JceSphereColliderComponent,     sphere_collider)
JCE_COMP_IMPL(JceCharacterControllerComponent,character_controller)
JCE_COMP_IMPL(JceAudioSourceComponent,        audio_source)
JCE_COMP_IMPL(JceVideoPlayerComponent,        video_player)
JCE_COMP_IMPL(JceScriptComponent,             script)
JCE_COMP_IMPL(JceParticleEmitterComponent,    particle_emitter)
JCE_COMP_IMPL(JceBehaviorTree,                behavior_tree)
JCE_COMP_IMPL(JceEditorMeta,                  editor_meta)
JCE_COMP_IMPL(JceTerrainComponent,            terrain)
JCE_COMP_IMPL(JceLodGroupComponent,           lod_group)
JCE_COMP_IMPL(JceVirtualCameraComponent,      virtual_camera)
JCE_COMP_IMPL(JceTriggerVolumeComponent,      trigger_volume)
JCE_COMP_IMPL(JceCapsuleColliderComponent,    capsule_collider)
JCE_COMP_IMPL(JceMeshColliderComponent,       mesh_collider)
JCE_COMP_IMPL(JceCompoundColliderComponent,   compound_collider)
JCE_COMP_IMPL(JceCollider2DComponent,         collider2d)
JCE_COMP_IMPL(JceTrailRendererComponent,      trail_renderer)
JCE_COMP_IMPL(JceLineRendererComponent,       line_renderer)
JCE_COMP_IMPL(JceReflectionProbeComponent,    reflection_probe)
JCE_COMP_IMPL(JceDecalComponent,              decal)
JCE_COMP_IMPL(JceLightProbeGroupComponent,    light_probe_group)
JCE_COMP_IMPL(JceAudioListenerComponent,      audio_listener)
JCE_COMP_IMPL(JceAudioReverbZoneComponent,    audio_reverb_zone)
JCE_COMP_IMPL(JceAudioOcclusionComponent,     audio_occlusion)
JCE_COMP_IMPL(JceSpawnManagerComponent,       spawn_manager)
JCE_COMP_IMPL(JceWeaponComponent,             weapon)
JCE_COMP_IMPL(JceSavePointComponent,          save_point)
JCE_COMP_IMPL(JceWheelColliderComponent,      wheel_collider)
JCE_COMP_IMPL(JceConstantForceComponent,      constant_force)
JCE_COMP_IMPL(JceConfigurableJointComponent,  configurable_joint)
JCE_COMP_IMPL(JceJoint2DComponent,            joint2d)
JCE_COMP_IMPL(JceBillboardRendererComponent,  billboard_renderer)
JCE_COMP_IMPL(JceCanvasComponent,             canvas)
JCE_COMP_IMPL(JceCanvasGroupComponent,        canvas_group)
JCE_COMP_IMPL(JceLayoutGroupComponent,        layout_group)
JCE_COMP_IMPL(JceUIImageComponent,            ui_image)
JCE_COMP_IMPL(JceUITextComponent,             ui_text)
JCE_COMP_IMPL(JceUIButtonComponent,           ui_button)
JCE_COMP_IMPL(JceNetworkObjectComponent,      network_object)
JCE_COMP_IMPL(JceClothComponent,              cloth)
JCE_COMP_IMPL(JceNetTransformComponent,       net_transform)
JCE_COMP_IMPL(JceNetAnimatorComponent,        net_animator)
JCE_COMP_IMPL(JceNetRigidbodyComponent,       net_rigidbody)
JCE_COMP_IMPL(JceVfxGraphComponent,           vfx_graph)
JCE_COMP_IMPL(JceTilemapComponent,            tilemap)
JCE_COMP_IMPL(JceTilemapCollider2DComponent,  tilemap_collider2d)
JCE_COMP_IMPL(JceAvatarComponent,             avatar)
JCE_COMP_IMPL(JceTagComponent,                tag_component)
JCE_COMP_IMPL(JceLayerComponent,              layer_component)
JCE_COMP_IMPL(JceVolumeComponent,             volume)
JCE_COMP_IMPL(JceOcclusionPortalComponent,    occlusion_portal)
JCE_COMP_IMPL(JceNavAgentComponent,           nav_agent)
JCE_COMP_IMPL(JceIkConstraintComponent,       ik_constraints)
JCE_COMP_IMPL(JceSequencePlayerComponent,     sequence_player)

#undef JCE_COMP_IMPL

/* ── Component enumeration ─────────────────────────────────────────── */

uint64_t jce_scene_get_disabled_components(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return 0;
    const JceCompEnableState *st = ecs_get(s->world, ent, JceCompEnableState);
    return st ? st->disabled : 0;
}

void jce_scene_set_disabled_components(JceScene *s, JceEntity e, uint64_t mask)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return;
    if (mask == 0) {
        if (ecs_has(s->world, ent, JceCompEnableState))
            ecs_remove(s->world, ent, JceCompEnableState);
        return;
    }
    JceCompEnableState st = { mask };
    ecs_set_ptr(s->world, ent, JceCompEnableState, &st);
}

bool jce_scene_component_enabled(const JceScene *s, JceEntity e, uint64_t flag)
{
    /* Default enabled: only an explicitly-set DISABLED bit turns it off. */
    return (jce_scene_get_disabled_components(s, e) & flag) == 0;
}

void jce_scene_set_component_enabled(JceScene *s, JceEntity e, uint64_t flag, bool enabled)
{
    uint64_t m  = jce_scene_get_disabled_components(s, e);
    uint64_t nm = enabled ? (m & ~flag) : (m | flag);
    if (nm != m) jce_scene_set_disabled_components(s, e, nm);
}

uint64_t jce_scene_get_component_flags(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return 0;
    uint64_t flags = 0;

    if (ecs_has(s->world, ent, JceTransform))                   flags |= JCE_COMP_FLAG_TRANSFORM;
    if (ecs_has(s->world, ent, JceMeshRenderer))                flags |= JCE_COMP_FLAG_MESH_RENDERER;
    if (ecs_has(s->world, ent, JceCameraComponent))             flags |= JCE_COMP_FLAG_CAMERA;
    if (ecs_has(s->world, ent, JceDirectionalLight))            flags |= JCE_COMP_FLAG_DIR_LIGHT;
    if (ecs_has(s->world, ent, JcePointLight))                  flags |= JCE_COMP_FLAG_POINT_LIGHT;
    if (ecs_has(s->world, ent, JceSpotLight))                   flags |= JCE_COMP_FLAG_SPOT_LIGHT;
    if (ecs_has(s->world, ent, JceSkyboxComponent))             flags |= JCE_COMP_FLAG_SKYBOX;
    if (ecs_has(s->world, ent, JceSpriteRendererComponent))     flags |= JCE_COMP_FLAG_SPRITE_RENDERER;
    if (ecs_has(s->world, ent, JceSpriteAnimatorComponent))     flags |= JCE_COMP_FLAG_SPRITE_ANIMATOR;
    if (ecs_has(s->world, ent, JceAnimatorComponent))           flags |= JCE_COMP_FLAG_ANIMATOR;
    if (ecs_has(s->world, ent, JceSkeletalAnimatorComponent))   flags |= JCE_COMP_FLAG_SKELETAL_ANIMATOR;
    if (ecs_has(s->world, ent, JceConstraintComponent))         flags |= JCE_COMP_FLAG_CONSTRAINT;
    if (ecs_has(s->world, ent, JceRigidBodyComponent))          flags |= JCE_COMP_FLAG_RIGIDBODY;
    if (ecs_has(s->world, ent, JceRigidBody2DComponent))        flags |= JCE_COMP_FLAG_RIGIDBODY_2D;
    if (ecs_has(s->world, ent, JceBoxColliderComponent))        flags |= JCE_COMP_FLAG_BOX_COLLIDER;
    if (ecs_has(s->world, ent, JceSphereColliderComponent))     flags |= JCE_COMP_FLAG_SPHERE_COLLIDER;
    if (ecs_has(s->world, ent, JceCharacterControllerComponent))flags |= JCE_COMP_FLAG_CHARACTER_CONTROLLER;
    if (ecs_has(s->world, ent, JceAudioSourceComponent))        flags |= JCE_COMP_FLAG_AUDIO_SOURCE;
    if (ecs_has(s->world, ent, JceScriptComponent))             flags |= JCE_COMP_FLAG_SCRIPT;
    if (ecs_has(s->world, ent, JceParticleEmitterComponent))    flags |= JCE_COMP_FLAG_PARTICLE_EMITTER;
    if (ecs_has(s->world, ent, JceBehaviorTree))                flags |= JCE_COMP_FLAG_BEHAVIOR_TREE;
    if (ecs_has(s->world, ent, JceEditorMeta))                  flags |= JCE_COMP_FLAG_EDITOR_META;
    if (ecs_has(s->world, ent, JceTerrainComponent))            flags |= JCE_COMP_FLAG_TERRAIN;
    if (ecs_has(s->world, ent, JceLodGroupComponent))           flags |= JCE_COMP_FLAG_LOD_GROUP;
    if (ecs_has(s->world, ent, JceVirtualCameraComponent))      flags |= JCE_COMP_FLAG_VIRTUAL_CAMERA;
    if (ecs_has(s->world, ent, JceTriggerVolumeComponent))      flags |= JCE_COMP_FLAG_TRIGGER_VOLUME;
    if (ecs_has(s->world, ent, JceCapsuleColliderComponent))    flags |= JCE_COMP_FLAG_CAPSULE_COLLIDER;
    if (ecs_has(s->world, ent, JceMeshColliderComponent))       flags |= JCE_COMP_FLAG_MESH_COLLIDER;
    if (ecs_has(s->world, ent, JceCollider2DComponent))         flags |= JCE_COMP_FLAG_COLLIDER_2D;
    if (ecs_has(s->world, ent, JceTrailRendererComponent))      flags |= JCE_COMP_FLAG_TRAIL_RENDERER;
    if (ecs_has(s->world, ent, JceLineRendererComponent))       flags |= JCE_COMP_FLAG_LINE_RENDERER;
    if (ecs_has(s->world, ent, JceReflectionProbeComponent))    flags |= JCE_COMP_FLAG_REFLECTION_PROBE;
    if (ecs_has(s->world, ent, JceDecalComponent))              flags |= JCE_COMP_FLAG_DECAL;
    if (ecs_has(s->world, ent, JceLightProbeGroupComponent))    flags |= JCE_COMP_FLAG_LIGHT_PROBE_GROUP;
    if (ecs_has(s->world, ent, JceAudioListenerComponent))      flags |= JCE_COMP_FLAG_AUDIO_LISTENER;
    if (ecs_has(s->world, ent, JceAudioReverbZoneComponent))    flags |= JCE_COMP_FLAG_AUDIO_REVERB_ZONE;
    if (ecs_has(s->world, ent, JceAudioOcclusionComponent))     flags |= JCE_COMP_FLAG_AUDIO_OCCLUSION;
    if (ecs_has(s->world, ent, JceSpawnManagerComponent))       flags |= JCE_COMP_FLAG_SPAWN_MANAGER;
    if (ecs_has(s->world, ent, JceWeaponComponent))             flags |= JCE_COMP_FLAG_WEAPON;
    if (ecs_has(s->world, ent, JceSavePointComponent))          flags |= JCE_COMP_FLAG_SAVE_POINT;
    if (ecs_has(s->world, ent, JceWheelColliderComponent))      flags |= JCE_COMP_FLAG_WHEEL_COLLIDER;
    if (ecs_has(s->world, ent, JceConstantForceComponent))      flags |= JCE_COMP_FLAG_CONSTANT_FORCE;
    if (ecs_has(s->world, ent, JceConfigurableJointComponent))  flags |= JCE_COMP_FLAG_CONFIGURABLE_JOINT;
    if (ecs_has(s->world, ent, JceJoint2DComponent))            flags |= JCE_COMP_FLAG_JOINT_2D;
    if (ecs_has(s->world, ent, JceBillboardRendererComponent))  flags |= JCE_COMP_FLAG_BILLBOARD_RENDERER;
    if (ecs_has(s->world, ent, JceCanvasComponent))             flags |= JCE_COMP_FLAG_CANVAS;
    if (ecs_has(s->world, ent, JceCanvasGroupComponent))        flags |= JCE_COMP_FLAG_CANVAS_GROUP;
    if (ecs_has(s->world, ent, JceLayoutGroupComponent))        flags |= JCE_COMP_FLAG_LAYOUT_GROUP;
    if (ecs_has(s->world, ent, JceUIImageComponent))            flags |= JCE_COMP_FLAG_UI_IMAGE;
    if (ecs_has(s->world, ent, JceUITextComponent))             flags |= JCE_COMP_FLAG_UI_TEXT;
    if (ecs_has(s->world, ent, JceUIButtonComponent))           flags |= JCE_COMP_FLAG_UI_BUTTON;
    if (ecs_has(s->world, ent, JceNetworkObjectComponent))      flags |= JCE_COMP_FLAG_NETWORK_OBJECT;
    if (ecs_has(s->world, ent, JceClothComponent))              flags |= JCE_COMP_FLAG_CLOTH;
    if (ecs_has(s->world, ent, JceNetTransformComponent))       flags |= JCE_COMP_FLAG_NET_TRANSFORM;
    if (ecs_has(s->world, ent, JceNetAnimatorComponent))        flags |= JCE_COMP_FLAG_NET_ANIMATOR;
    if (ecs_has(s->world, ent, JceNetRigidbodyComponent))       flags |= JCE_COMP_FLAG_NET_RIGIDBODY;
    if (ecs_has(s->world, ent, JceVfxGraphComponent))           flags |= JCE_COMP_FLAG_VFX_GRAPH;
    if (ecs_has(s->world, ent, JceTilemapComponent))            flags |= JCE_COMP_FLAG_TILEMAP;
    if (ecs_has(s->world, ent, JceTilemapCollider2DComponent))  flags |= JCE_COMP_FLAG_TILEMAP_COLLIDER_2D;
    if (ecs_has(s->world, ent, JceAvatarComponent))             flags |= JCE_COMP_FLAG_AVATAR;
    if (ecs_has(s->world, ent, JceTagComponent))                flags |= JCE_COMP_FLAG_TAG;
    if (ecs_has(s->world, ent, JceLayerComponent))              flags |= JCE_COMP_FLAG_LAYER;
    if (ecs_has(s->world, ent, JceVolumeComponent))             flags |= JCE_COMP_FLAG_VOLUME;
    if (ecs_has(s->world, ent, JceOcclusionPortalComponent))    flags |= JCE_COMP_FLAG_OCCLUSION_PORTAL;

    return flags;
}

/* ── Iteration ─────────────────────────────────────────────────────── */

typedef struct {
    JceScene          *scene;
    JceEntityCallback  cb;
    void              *user_data;
} IterCtx;

static void entity_iter_cb(ecs_iter_t *it)
{
    IterCtx *ctx = (IterCtx *)it->ctx;
    for (int i = 0; i < it->count; i++) {
        ctx->cb(ctx->scene, (JceEntity)it->entities[i], ctx->user_data);
    }
}

void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !cb) return;

    /* Cache the query.  It was previously created with ecs_query() and torn
     * down with ecs_query_fini() on EVERY call — and this runs 4-5x per frame
     * during Play (video update, particles probe + each, viewer scan, audio-3D
     * listener), so the per-query Flecs allocations accumulated unbounded
     * (GB-scale over minutes of idle Play).  Mirrors the cloth_query pattern;
     * freed in jce_scene_destroy. */
    if (!s->each_query) {
        s->each_query = ecs_query(s->world, {
            .terms = {{ .id = ecs_id(JceTransform) }},
        });
        if (!s->each_query) return;
    }

    ecs_iter_t it = ecs_query_iter(s->world, s->each_query);
    while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; i++) {
            cb(s, (JceEntity)it.entities[i], user_data);
        }
    }
}

void *jce_scene_get_world(JceScene *s)
{
    return s ? s->world : NULL;
}

void jce_scene_update(JceScene *s, float dt)
{
    JCE_PROFILE_ZONE_N("Scene::Update");
    if (!s) { JCE_PROFILE_ZONE_END; return; }

    /* New frame → invalidate last frame's world-matrix cache. Done before
       ecs_progress so any system that reads world matrices this frame builds
       a fresh, consistent cache against transforms as they are at read time. */
    jce_scene_invalidate_world_cache(s);

    ecs_progress(s->world, dt);

    /* ── Cloth reconciliation (P3-C.4 follow-up) ─────────────────────
     * For every entity with a JceClothComponent whose runtime handle is
     * absent or whose authoring fields have been edited (`dirty`), destroy
     * the old handle (if any) and create a fresh one from the desc.
     *
     * Wind updates on an already-existing handle are applied in-place to
     * avoid a full rebuild for cheap parameter tweaks. */
    {
        if (!s->cloth_query) {
            s->cloth_query = ecs_query(s->world, {
                .terms = {{ .id = ecs_id(JceClothComponent) }},
            });
        }
        ecs_query_t *q = s->cloth_query;   /* cached; reused every frame */
        if (q) {
            ecs_iter_t it = ecs_query_iter(s->world, q);
            while (ecs_query_next(&it)) {
                JceClothComponent *c = ecs_field(&it, JceClothComponent, 0);
                if (!c) continue;
                for (int i = 0; i < it.count; ++i) {
                    JceClothComponent *cc = &c[i];
                    if (cc->dirty || cc->handle == 0) {
                        if (cc->handle != 0) {
                            jce_cloth_destroy((JceClothHandle)cc->handle);
                            cc->handle = 0;
                        }
                        if (cc->res_u < 2) cc->res_u = 2;
                        if (cc->res_v < 2) cc->res_v = 2;
                        uint32_t pc = cc->pinned_count;
                        if (pc > JCE_CLOTH_MAX_PINNED) pc = JCE_CLOTH_MAX_PINNED;
                        JceClothDesc d;
                        memset(&d, 0, sizeof d);
                        d.corner_00 = cc->corner_00;
                        d.corner_10 = cc->corner_10;
                        d.corner_01 = cc->corner_01;
                        d.corner_11 = cc->corner_11;
                        d.res_u = cc->res_u;
                        d.res_v = cc->res_v;
                        d.mass_total       = cc->mass_total;
                        d.stiffness_linear = cc->stiffness_linear;
                        d.stiffness_angular= cc->stiffness_angular;
                        d.damping          = cc->damping;
                        d.iterations       = cc->iterations ? cc->iterations : 4;
                        d.pinned_indices   = pc ? cc->pinned_indices : NULL;
                        d.pinned_count     = pc;
                        d.self_collision   = cc->self_collision;
                        d.wind_enabled     = cc->wind_enabled;
                        d.wind_velocity    = cc->wind_velocity;
                        cc->handle = (uint32_t)jce_cloth_create(&d);
                        cc->dirty  = false;
                    } else {
                        jce_cloth_set_wind((JceClothHandle)cc->handle,
                                           cc->wind_velocity,
                                           cc->wind_enabled);
                    }
                }
            }
            /* q is cached (s->cloth_query) — freed in jce_scene_destroy. */
        }
    }

#if defined(JCE_PROFILER_ENABLED)
    {
        const ecs_world_info_t *info = ecs_get_world_info(s->world);
        if (info) {
            JCE_PROFILE_PLOT_I("scene.entities", (int64_t)info->entity_count);
        }
    }
#endif

    JCE_PROFILE_ZONE_END;
}
