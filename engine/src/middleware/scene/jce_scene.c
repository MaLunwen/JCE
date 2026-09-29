/*
 * jce_scene.c  ECS scene implementation (flecs backend).
 */

#include <jce/middleware/scene/jce_str_intern.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_fullscreen_effect.h>
#include <jce/middleware/scene/jce_water_field.h>
#include <jce/middleware/scene/jce_water_ripple.h>
#include <jce/middleware/world/jce_environment.h>
#include "jce_terrain_cache.h"
#include <stdio.h>   /* snprintf (entity-name uniquify) */
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_perf_phase.h>   /* #4 ECS stress: ecs_move phase timing */
#include <jce/os/core/jce_timer.h>
#include "jce_component_registry_internal.h"
#include "jce_scene_internal.h"   /* entity-name scope rule (jce_scene_names.c) */
/* jce_scene_normalise_mesh_renderer, called by the SET macro below.  It was
 * named only in a COMMENT there, so the call went through an implicit
 * declaration -- C99 assumes int(*)() and the real function returns void, which
 * this ABI happens to survive and another need not. */
#include "jce_scene_component_normalise.h"
#include "os/core/jce_memory.h"

#include <flecs.h>
#include <string.h>
#include <stdlib.h>   /* getenv — JCE_DISABLE_XGEN kill-switch */
#include <math.h>     /* sinf/cosf — bounded orbit for the #4 entity-count spin */

#define LOG_TAG "scene"

/* #4 ECS benchmark: set to 1 by the editor Performance Benchmark panel to enable
 * the per-frame JceTransform integrate in jce_scene_update without a relaunch
 * (the JCE_STRESS_SPIN env is the headless equivalent). */
int jce_scene_stress_spin_runtime = 0;
/* Entity-count BENCHMARK root: when non-zero the stress-spin integrate moves
 * ONLY this root's descendants (the benchmark's own spawned entities), so the
 * real authored scene's models are never dragged around by the test.  0 = move
 * everything (the raw env JCE_STRESS_SPIN dev throughput path). */
unsigned int jce_scene_stress_spin_root = 0u;

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
/* Per-entity per-component DISABLED bits, keyed by dense comp_id from
 * the component registry (jce_component_registry.h) — NOT by the legacy
 * 64-bit JCE_COMP_FLAG_* space, which is full.  4×64 words cover
 * JCE_COMP_MAX rows.  Absent component == everything enabled. */
typedef struct { uint64_t disabled[4]; } JceCompEnableState;
/* "A script wrote this component's enable bit during this run."  Purely an
 * authoring affordance: without it the editor cannot tell a component the user
 * switched off from one a script re-asserts every frame, so the Inspector
 * checkbox silently does nothing and reads as broken.  Never serialized, so a
 * Play-stop restore drops it along with the rest of the run's state. */
typedef struct { uint64_t driven[4]; } JceCompScriptDriven;
/* The name the caller actually authored.
 *
 * Scene entities are created in the flecs ROOT scope, which already contains
 * flecs's own built-in entities ("Empty", "Target", "Prefab", "Disabled",
 * "Name", "Component", "World", "Module", "Observer", ...).  flecs requires
 * names to be unique within a scope, so jce_scene_create_entity uniquifies a
 * taken name by appending "_<entity_id>".  That kept flecs's index valid but
 * made the rename observable: asking for "Target" produced "Target_470", so
 * every lookup by the authored name failed and the wrong name reached the
 * saved scene.  Duplicate authored names (many "Ped" from a SpawnManager) hit
 * the same path.
 *
 * The authored name is therefore stored separately and is the entity's
 * identity for the public API and for serialization; the flecs name remains
 * an internal, possibly-uniquified index.  The pointer is interned in the
 * scene string pool, so it is stable across flecs table moves and is owned by
 * the pool rather than by the component. */
typedef struct { const char *v; } JceCompAuthoredName;

static ECS_COMPONENT_DECLARE(JceCompEnableState);
static ECS_COMPONENT_DECLARE(JceCompScriptDriven);
static ECS_COMPONENT_DECLARE(JceCompAuthoredName);
static ECS_COMPONENT_DECLARE(JceTransform);
static ECS_COMPONENT_DECLARE(JcePivotComponent);
static ECS_COMPONENT_DECLARE(JceMeshRenderer);
static ECS_COMPONENT_DECLARE(JceCameraComponent);
static ECS_COMPONENT_DECLARE(JceDirectionalLight);
static ECS_COMPONENT_DECLARE(JcePointLight);
static ECS_COMPONENT_DECLARE(JceSpotLight);
static ECS_COMPONENT_DECLARE(JceAreaLight);
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
static ECS_COMPONENT_DECLARE(JceMusicTrackComponent);
static ECS_COMPONENT_DECLARE(JceVideoPlayerComponent);
static ECS_COMPONENT_DECLARE(JceScriptComponent);
static ECS_COMPONENT_DECLARE(JceEditorMeta);
static ECS_COMPONENT_DECLARE(JceTerrainComponent);
static ECS_COMPONENT_DECLARE(JceVegetationScatterComponent);
static ECS_COMPONENT_DECLARE(JceGrassFieldComponent);
static ECS_COMPONENT_DECLARE(JceFoliageClusterComponent);
static ECS_COMPONENT_DECLARE(JceWaterComponent);
static ECS_COMPONENT_DECLARE(JceBuoyancyComponent);
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
static ECS_COMPONENT_DECLARE(JceLayoutElementComponent);
static ECS_COMPONENT_DECLARE(JceUIImageComponent);
static ECS_COMPONENT_DECLARE(JceUITextComponent);
static ECS_COMPONENT_DECLARE(JceUIButtonComponent);
static ECS_COMPONENT_DECLARE(JceUISliderComponent);
static ECS_COMPONENT_DECLARE(JceUIToggleComponent);
static ECS_COMPONENT_DECLARE(JceUIInputFieldComponent);
static ECS_COMPONENT_DECLARE(JceUIScrollViewComponent);
static ECS_COMPONENT_DECLARE(JceUIProgressBarComponent);
static ECS_COMPONENT_DECLARE(JceUIDropdownComponent);
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
static ECS_COMPONENT_DECLARE(JceFootIkComponent);
static ECS_COMPONENT_DECLARE(JceContentSizeFitterComponent);
static ECS_COMPONENT_DECLARE(JceBoneAttachmentComponent);
static ECS_COMPONENT_DECLARE(JceFullBodyIkComponent);
static ECS_COMPONENT_DECLARE(JceSequencePlayerComponent);
static ECS_COMPONENT_DECLARE(JceMorphWeightsComponent);
static ECS_COMPONENT_DECLARE(JceNetworkVariableComponent);
static ECS_COMPONENT_DECLARE(JceGameplayAbilitySystemComponent);
static ECS_COMPONENT_DECLARE(JceRagdollComponent);
static ECS_COMPONENT_DECLARE(JceRagdollPoseRelay);
static ECS_COMPONENT_DECLARE(JceAnimCmdRelay);
static ECS_COMPONENT_DECLARE(JceFractureComponent);
static ECS_COMPONENT_DECLARE(JceVehicleComponent);
static ECS_COMPONENT_DECLARE(JceSoftBodyComponent);
static ECS_COMPONENT_DECLARE(JceSimLodComponent);
static ECS_COMPONENT_DECLARE(JceSceneFullscreenEffect);

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
    /* Every water body in this scene, on ONE clock.  Lazily created because
     * most scenes have no water at all.  Owned here rather than by the renderer
     * or the runtime precisely so neither of them can own a private copy --
     * that is the bug jce_water_field.h exists to make unrepresentable. */
    JceWaterFieldSet *water_fields;
    /* The disturbance layer, beside the ambient one it adds to. */
    JceWaterRipple   *water_ripple;
    /* THE environment state for this scene: time of day, weather, wind,
     * humidity, cloud cover.
     *
     * Here for exactly the reason water_fields is here. It lived on the scene
     * renderer, which is where it is advanced -- and so the runtime, which
     * needs the wind to build the same ocean the renderer draws, could not
     * reach it. The renderer therefore multiplied the wave spectrum's wind by
     * the weather and the runtime did not, and the two handed the SAME water
     * field two different descriptors. Rendering and physics disagreeing about
     * the world is the failure this whole layer exists to prevent, and it does
     * not stop being that failure when the disagreement is about wind. */
    JceEnvironmentState env;
    bool                env_init;
    /* One loaded terrain per path, shared by renderer / pick / physics.  Same
     * reason as water_fields above: five independent loaders of one asset are
     * five copies that can disagree, and an editor sculpt reached only one of
     * them.  Lazily created; most scenes have no terrain. */
    JceTerrainCache *terrain_cache;
    ecs_query_t *cloth_query;   /* cached; created lazily in jce_scene_update */
    ecs_query_t *each_query;    /* cached; created lazily in jce_scene_each_entity
                                 * (leak fix: was ecs_query()+ecs_query_fini() on
                                 * EVERY call, 4-5x/frame in Play → unbounded) */
    ecs_query_t *spin_query;    /* stress-spin (entity-count benchmark) integrate:
                                 * JceTransform WITHOUT Camera / CharacterController
                                 * so the view-driving entities are never moved by
                                 * the benchmark (else the game-view follow drifts). */
    uint64_t      world_epoch;  /* bumped per frame to invalidate the world-matrix cache */
    JceWorldCache world_cache;  /* per-entity world matrix memo (side table) */
    /* Structural epoch: bumped ONLY on real structural edits (a set_transform /
     * set_pivot / reparent / component add-remove that changes a world matrix),
     * NOT on the per-frame world-cache drops in jce_scene_update / the renderer /
     * the pick pass.  Lets a cross-frame consumer (the scene renderer's persistent
     * static world-matrix + AABB cache, large-world-opt M1 #2) keep its cached
     * value for a STATIC entity across frames and recompute only when this changes.
     * In-place transform mutation that bypasses jce_scene_set_transform (the
     * runtime physics write-back rt_sync_transforms) does NOT bump this — those
     * entities are excluded from the persistent cache by a per-entity dynamic
     * predicate instead, so the epoch staying put for them is intentional. */
    uint64_t      structural_epoch;
    /* Per-entity transform generation (dynamic-scene opt): set_transform /
     * set_pivot bump ONLY the moved entity's gen + its subtree instead of the
     * global structural_epoch, so a moving camera / script-animated entity no
     * longer invalidates the renderer's persistent static world-cache for the
     * WHOLE scene (measured ~+27% cpu/frame at 10k entities when it did).  Open-
     * addressing hash (entity→gen), grown on demand.  xgen_active gates the
     * renderer's per-entity check, so a scene that never set_transforms an entity
     * stays on the cheap epoch-only fast path (zero per-entity lookup cost). */
    struct JceXGenSlot { uint64_t entity; uint64_t gen; uint64_t mat_gen; } *xgen; /* entity==0: empty */
    uint32_t      xgen_cap, xgen_count;
    bool          xgen_active;
    /* Frame-invariance counters (DOTS-floor slice 1) — see jce_scene.h docs.
     * roster_epoch: create/destroy; enable_gen: EditorMeta.enabled flips;
     * xform_counter: every per-entity world invalidation + physics write-back. */
    uint64_t      roster_epoch;
    uint64_t      enable_gen;
    /* Number of entities carrying a JceCompEnableState row.  Maintained at
     * the only two write sites (jce_scene_set_comp_enabled) so the read
     * side is a plain load.  jce_scene_comp_enabled used to ask flecs
     * (ecs_count_id) on EVERY call for the same all-clear answer; that is
     * documented as O(1) but iterates the id's table cache and costs ~220
     * cycles even when it returns 0 - measured as 27% of the scene
     * renderer's submit loop, 4x the component fetch it guards.
     *
     * Deleting an entity drops its row without passing through the setter,
     * so this can drift HIGH.  That direction is safe: it only falls back
     * to the per-entity ecs_get, which is always correct.  It must never
     * drift low, hence the increment sits on the branch that actually adds
     * a row (cur == NULL) rather than on every write. */
    /* Owns every interned component asset path (see jce_str_intern.h).
     * Scene-scoped: the pointers live inside components and must not outlive
     * the world that holds them. */
    JceStrPool   *str_pool;
    int32_t       comp_enable_rows;
    /* The entities that actually carry a disable row.  Kept because the
     * all-or-nothing short-circuit below it was defeated by a single row: at
     * 200k entities exactly ONE carried one, and that made every visible
     * entity pay a full component lookup every frame (70 cycles x 27295 =
     * ~0.56 ms).  A scene has a handful of these at most -- a linear scan of
     * this list answers in a couple of cycles and is exact.
     *
     * Above JCE_COMP_DISABLE_SET_MAX the list stops being maintained and the
     * query falls back to the component lookup: a scene with hundreds of
     * disabled components is not the case this optimises, and a list that
     * silently truncated would answer WRONG rather than slow. */
    JceEntity    *comp_enable_ents;
    int32_t       comp_enable_ents_count;
    int32_t       comp_enable_ents_cap;
    bool          comp_enable_ents_valid;
    uint64_t      xform_counter;
    /* cull_data_gen: a component whose FIELDS the entity-cull table memoizes
     * was written.  enable_gen covers only the enabled bit and xform_counter
     * only transforms, so toggling e.g. casts_shadow on a live entity had no
     * signal and the cull table served the stale value forever.
     *
     * Scoped deliberately.  It was first bumped from the accessor macros
     * themselves -- every one of the 92 components -- on the theory that a
     * blanket signal cannot be forgotten.  That made it useless as a cache
     * key: any script writing ANY component every frame (a UI fill, a line
     * colour, a material tint) invalidated it, so the cull freeze AND its
     * incremental-repair path both died every frame and the whole entity
     * cull rebuilt from scratch.  Only the components jce_sr_cull.c actually
     * reads may bump it; tools/lint/check_cull_gen_consumers.py fails the
     * build if that file grows a read whose component does not. */
    uint64_t      cull_data_gen;
    /* Dirty ring (DOTS-floor L2): every entity whose world matrix was
     * invalidated since the last take — appended by the
     * invalidate_entity_world subtree walk (the only per-entity xform bump
     * source).  The physics write-back mutates an UNKNOWN entity set in
     * place, so it sets dirty_overflow instead; overflow (or a ring past
     * capacity) tells the consumer to fall back to a full rebuild. */
    JceEntity     dirty_ring[1024];
    uint32_t      dirty_count;
    bool          dirty_overflow;
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
        r->soft_shadow_mode > JCE_SCENE_SOFT_SHADOW_PCSS)
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

    /* Sky clamps.  The upper bound is the LAST enumerator, written as the
     * symbol rather than a literal: this was `> 3` and silently reset any
     * newly-added mode to GRADIENT on load, so a new sky would have looked
     * implemented everywhere and never once rendered. */
    if (r->sky_mode < JCE_SCENE_SKY_GRADIENT ||
        r->sky_mode > JCE_SCENE_SKY_PHYSICAL)
        r->sky_mode = JCE_SCENE_SKY_GRADIENT;
    /* Clouds: clamp coverage, and keep the layer non-degenerate.  A top at or
     * below the bottom would make the march divide by a zero-thickness slab. */
    if (!(r->cloud_coverage > 0.0f)) r->cloud_coverage = 0.0f;
    if (r->cloud_coverage > 1.0f)    r->cloud_coverage = 1.0f;
    if (r->cloud_top_km <= r->cloud_bottom_km) {
        r->cloud_bottom_km = 0.0f;   /* 0 selects the defaults downstream */
        r->cloud_top_km    = 0.0f;
    }

    if (r->sky_turbidity < 1.0f)
        r->sky_turbidity = 1.0f;
    if (r->sky_turbidity > 10.0f)
        r->sky_turbidity = 10.0f;

    /* Look Profile clamps. */
    if (r->tonemap_op < 0 || r->tonemap_op > 2)
        r->tonemap_op = JCE_TONEMAP_ACES;
    if (r->wrap_factor   < 0.0f) r->wrap_factor   = 0.0f;
    if (r->wrap_factor   > 1.0f) r->wrap_factor   = 1.0f;
    if (r->rim_intensity < 0.0f) r->rim_intensity = 0.0f;
    if (r->rim_power      < 0.1f) r->rim_power     = 0.1f;
    if (r->lut_strength  < 0.0f) r->lut_strength  = 0.0f;
    if (r->lut_strength  > 1.0f) r->lut_strength  = 1.0f;
    if (r->bloom_knee    < 0.0f) r->bloom_knee    = 0.0f;
    r->lut_path[sizeof(r->lut_path) - 1] = '\0';
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

    /* Sky (gradient by default → existing sky path unchanged). */
    r.sky_mode      = JCE_SCENE_SKY_GRADIENT;
    r.sky_turbidity = 2.5f;      /* clear temperate day (Preetham default) */

    /* Stylized sky dome (golden-hour preset).  Unused unless sky_mode==3
     * AND the "stylized_sky" gate is on; absent in old scenes → these
     * values seed but never render → baseline byte-identical. */
    r.sky_dome_zenith[0]  = 0.16f; r.sky_dome_zenith[1]  = 0.33f; r.sky_dome_zenith[2]  = 0.62f;
    r.sky_dome_mid[0]     = 0.55f; r.sky_dome_mid[1]     = 0.55f; r.sky_dome_mid[2]     = 0.68f;
    r.sky_dome_mid_pos    = 0.45f;
    r.sky_dome_horizon[0] = 0.95f; r.sky_dome_horizon[1] = 0.78f; r.sky_dome_horizon[2] = 0.55f;
    r.sky_dome_ground[0]  = 0.30f; r.sky_dome_ground[1]  = 0.26f; r.sky_dome_ground[2]  = 0.22f;
    r.sky_dome_glow[0]    = 0.90f; r.sky_dome_glow[1]    = 0.55f; r.sky_dome_glow[2]    = 0.28f;
    r.sky_dome_glow_falloff   = 7.0f;
    r.sky_dome_sun_color[0]   = 1.0f; r.sky_dome_sun_color[1] = 0.92f; r.sky_dome_sun_color[2] = 0.70f;
    r.sky_dome_sun_size       = 0.9985f;  /* cos threshold: small bright core */
    r.sky_dome_sun_softness   = 0.0010f;
    r.sky_dome_halo_power     = 48.0f;
    r.sky_dome_halo_strength  = 0.35f;
    /* Sun rays OFF by default (count 0 gates the shader block bit-exact);
     * length/sharpness carry the reference animeSun values so turning the
     * feature on needs only a count + strength. */
    r.sky_dome_anchor_radius  = 0.0f;   /* 0 = legacy view dome */
    r.gi_dynamic              = 0.0f;   /* dynamic probe GI off  */
    r.sky_dome_ray_count      = 0.0f;
    r.sky_dome_ray_length     = 0.0352f;
    r.sky_dome_ray_sharpness  = 8.0f;
    r.sky_dome_ray_strength   = 0.8f;

    /* Sky IBL on by default (byte-identical for existing scenes). */
    r.ibl_enabled = true;

    /* Floating origin (off by default → runtime never rebases → byte-id). */
    r.floating_origin_enabled   = false;
    r.floating_origin_threshold = 4096.0f;

    /* SSAO (off by default → no depth pre-pass / SSAO pass → byte-identical). */
    r.ssao_enabled   = false;
    r.ssao_intensity = 1.5f;
    r.ssao_radius    = 1.0f;
    /* SSR (off by default → byte-identical). */
    r.ssr_enabled      = false;
    r.ssr_intensity    = 0.6f;
    r.ssr_max_distance = 8.0f;
    /* TAA tuning: 0 = engine defaults (feedback 0.9, clamps 1.0) → byte-id.
     * (MUST init — this default() does not zero the struct.) */
    r.taa_feedback     = 0.0f;
    r.taa_luma_clamp   = 0.0f;
    r.taa_motion_clamp = 0.0f;

    /* Look Profile — all NEUTRAL (algebraic no-op; byte-identical baseline). */
    r.wrap_factor         = 0.0f;       /* hard Lambert */
    r.ambient_hemisphere  = false;      /* flat ambient only */
    r.ambient_ground_color[0] = 0.0f;
    r.ambient_ground_color[1] = 0.0f;
    r.ambient_ground_color[2] = 0.0f;
    r.rim_color[0] = 1.0f; r.rim_color[1] = 1.0f; r.rim_color[2] = 1.0f;
    r.rim_power      = 4.0f;            /* harmless: rim_intensity=0 masks it */
    r.rim_intensity  = 0.0f;           /* off */
    r.tonemap_op     = JCE_TONEMAP_ACES;
    r.lut_path[0]    = '\0';
    r.lut_strength   = 0.0f;           /* off */
    r.toon_character = false;
    r.bloom_knee     = 0.0f;           /* hard cutoff = current bloom */
    /* Matches jce_environment_default(); a scene that does not author a
     * temperature must not change what the environment already does. */
    r.temperature_c = 15.0f;
    /* Auto exposure OFF, but the desc filled with the LAW's own defaults --
     * not zeros.  Zeros would be a [0,0] EV clamp and two zero speeds, i.e.
     * a feature that reports itself as configured and cannot move. */
    r.auto_exposure      = false;
    r.auto_exposure_desc = jce_auto_exposure_desc_default();
    /* OFF, and the numbers are only consulted when it is on -- so a scene
     * authored before depth of field existed renders identically. */
    r.dof_enabled         = false;
    r.dof_focus_distance  = 10.0f;
    r.dof_focus_range     = 4.0f;
    r.dof_max_coc         = 0.012f;
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

/* ── Water fields ──────────────────────────────────────────────────────
 *
 * Lazily created on first use, so a scene with no water never pays for one.
 * Returns NULL only if that allocation fails; every caller treats NULL as
 * "no water this frame" rather than crashing, because a failed 300-byte
 * allocation should not take down a level. */
JceEnvironmentState *jce_scene_environment(JceScene *s)
{
    if (!s) return NULL;
    if (!s->env_init) { s->env = jce_environment_default(); s->env_init = true; }
    return &s->env;
}

/* Fill a water-field descriptor for `e` from its Water component, its WORLD
 * transform and this scene's environment.
 *
 * ONE function because there were two hand-written descs for one field, and
 * they disagreed about three things at once. The renderer published
 * `base_height = wc->base_height` -- which is ENTITY-LOCAL, as the comment
 * twenty lines above its own shader uniform says -- so on hidden_cove the CPU
 * water surface sat at y = 0.000 while the water everyone could see was drawn
 * at 14.99. It also left center_x/center_z at zero, putting the body's centre
 * 40 m from where the body is. Buoyancy, shoreline and every gameplay query
 * read that field.
 *
 * Taking the entity rather than the component is what makes the mistake
 * unrepresentable: the caller no longer does the local-to-world mapping, so it
 * can no longer get it wrong.
 *
 * `waves` points into the component and is borrowed for the acquire call only,
 * exactly as jce_water_field.h requires. */
bool jce_scene_water_field_desc(JceScene *s, JceEntity e,
                                JceWaterFieldDesc *out)
{
    if (!s || !out) return false;
    JceWaterComponent *wc = jce_scene_get_water(s, e);
    if (!wc) return false;

    const jce_mat4 m = jce_scene_get_world_matrix(s, e);
    const JceEnvironmentState *env = jce_scene_environment(s);

    memset(out, 0, sizeof *out);
    out->model          = (JceWaterFieldModel)wc->water_mode;
    out->base_height    = wc->base_height + m.col[3].y;
    out->center_x       = m.col[3].x;
    out->center_z       = m.col[3].z;
    out->size_x         = wc->size_x;
    out->size_z         = wc->size_z;
    out->waves          = wc->waves;
    out->wave_count     = wc->wave_count;
    out->fft_resolution = wc->fft_resolution;
    out->fft_patch_size = wc->fft_patch_size;
    /* U4: the authored value is a MULTIPLIER of the one wind, and this is the
     * only place that applies it -- when the renderer applied it alone, the
     * runtime's desc differed by exactly this factor and every frame of
     * weather rebuilt the whole Tessendorf spectrum twice. */
    /* SUSTAINED, not instantaneous. Two independent reasons, either of which
     * alone settles it:
     *
     *   Physics -- the Phillips spectrum this feeds is parameterised by the
     *   wind that has blown over the fetch for hours. A seven-second gust does
     *   not restructure a developed sea; it puts ripples on one.
     *
     *   Cost -- spectrum_differs() in jce_water_field.c keys the spectrum
     *   cache on exactly this number, so a value that moves every frame
     *   re-solves the whole Tessendorf spectrum every frame. That is the same
     *   trap the comment above describes, arrived at from the other side. */
    out->fft_wind_speed = wc->fft_wind_speed *
                          jce_environment_wind_speed_sustained(env);
    out->fft_wind_dir_x = wc->fft_wind_dir_x;
    out->fft_wind_dir_z = wc->fft_wind_dir_z;
    out->fft_amplitude  = wc->fft_amplitude;
    out->fft_seed       = JCE_WATER_FIELD_SEED(e);
    out->fft_fetch      = wc->fft_fetch;
    out->fft_swell      = wc->fft_swell;
    return true;
}

JceWaterFieldSet *jce_scene_water_fields(JceScene *s)
{
    if (!s) return NULL;
    if (!s->water_fields) s->water_fields = jce_water_field_set_create();
    return s->water_fields;
}

/* JCE_API / JCE_CALL 必须与 jce_scene.h:3180 的声明一致。
 * 缺了它们时 MSVC 过得去（JCE_CALL 展开成 __cdecl，本来就是默认约定），
 * 而 clang/wasm 报 conflicting types —— 于是 wasm SDK 从此构建不了，
 * dist/sdk/wasm 停在 06-08，两个多月无人发现，因为没人为 wasm 构建过。
 * 症状是 web 版所有 T() 退化成原始 key（那份旧 SDK 里没有 loc_translate）。 */
JCE_API JceWaterRipple *JCE_CALL jce_scene_water_ripple(
    JceScene *s, const JceWaterRippleDesc *desc)
{
    if (!s) return NULL;
    /* Create only when asked WITH a desc. A reader passing NULL gets whatever
     * exists and never causes creation, so the size of the grid is decided by
     * the one caller that knows the water body, not by whoever happens to read
     * first. */
    if (!s->water_ripple && desc)
        s->water_ripple = jce_water_ripple_create(desc);
    return s->water_ripple;
}

/* ── Terrain cache ─────────────────────────────────────────────────────
 *
 * Lazily created, like the water fields.  Returns NULL only if that allocation
 * fails; callers treat NULL as "load it yourself this once" rather than
 * crashing a level over a few hundred bytes. */
JceTerrainCache *jce_scene_terrain_cache(JceScene *s)
{
    if (!s) return NULL;
    if (!s->terrain_cache) s->terrain_cache = jce_terrain_cache_create();
    return s->terrain_cache;
}

void JCE_CALL jce_scene_invalidate_terrain(JceScene *s, const char *path)
{
    /* Deliberately does NOT create the cache: invalidating a cache that was
     * never populated is a no-op, and allocating one to no-op on it would be
     * the kind of tidy-looking waste that shows up in a profile later. */
    if (!s || !s->terrain_cache) return;
    jce_terrain_cache_invalidate(s->terrain_cache, path);
}

bool JCE_CALL jce_scene_adopt_terrain(JceScene *s, const char *path,
                                      JceTerrain *terrain)
{
    if (!s || !path || !path[0] || !terrain) return false;
    JceTerrainCache *cache = jce_scene_terrain_cache(s);
    return cache && jce_terrain_cache_adopt(cache, path, terrain);
}

JceTerrain *JCE_CALL jce_scene_peek_terrain(JceScene *s, const char *path)
{
    if (!s || !s->terrain_cache) return NULL;
    return jce_terrain_cache_peek(s->terrain_cache, path);
}

bool JCE_CALL jce_scene_touch_terrain(JceScene *s, const char *path)
{
    if (!s || !s->terrain_cache) return false;
    return jce_terrain_cache_touch(s->terrain_cache, path);
}

/* ── Create / destroy ──────────────────────────────────────────────── */
/* Zero-init leaves the interned path pointers NULL; every reader dereferences
 * them unconditionally.  Point them at the shared empty string instead. */

static void jce__em_ctor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceEditorMeta *m = (JceEditorMeta *)ptr;
    for (int32_t i = 0; i < count; i++) jce_editor_meta_init(&m[i]);
}

static void jce__mr_ctor(void *ptr, int32_t count, const ecs_type_info_t *ti)
{
    (void)ti;
    JceMeshRenderer *m = (JceMeshRenderer *)ptr;
    const char *e = jce_str_empty();
    for (int32_t i = 0; i < count; i++) {
        memset(&m[i], 0, sizeof(JceMeshRenderer));
        m[i].mesh_path     = e;
        m[i].material_path = e;
        m[i].albedo_tex    = e;
        m[i].mr_tex        = e;
        m[i].normal_tex    = e;
        m[i].ao_tex        = e;
        m[i].emissive_tex  = e;
    }
}


JceScene *jce_scene_create(void)
{
    JceScene *s = (JceScene *)JCE_CALLOC(1, sizeof(*s));
    if (!s) return NULL;

    /* Populate the dense component-type registry exactly once per
     * process (idempotent) — serializer, editor surfaces and the
     * id-based enable state all key off it. */
    jce_scene_components_register_all();

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
    /* Start at 1 so a freshly zeroed persistent-cache slot (epoch 0) is always
       seen as stale on first access. */
    s->structural_epoch = 1;
    s->roster_epoch     = 1;
    s->enable_gen       = 1;
    s->cull_data_gen    = 1;
    s->xform_counter    = 1;

    /* Register components. */
    ECS_COMPONENT_DEFINE(s->world, JceCompEnableState);
    ECS_COMPONENT_DEFINE(s->world, JceCompScriptDriven);
    ECS_COMPONENT_DEFINE(s->world, JceCompAuthoredName);
    ECS_COMPONENT_DEFINE(s->world, JceTransform);
    ECS_COMPONENT_DEFINE(s->world, JcePivotComponent);
    ECS_COMPONENT_DEFINE(s->world, JceMeshRenderer);

    {
        /* The seven asset paths are interned pointers now, and a zero-filled
         * component would leave them NULL.  Every read site does
         * `mr->mesh_path[0]` with no null check -- correctly, because these
         * used to be char[256] where zero-init meant "".  A ctor covers EVERY
         * creation path, including the ones flecs takes internally when a
         * component is added without a value, which is more than the setter
         * could reach. */
        ecs_type_hooks_t h;
        memset(&h, 0, sizeof(h));
        h.ctor = jce__mr_ctor;
        ecs_set_hooks_id(s->world, ecs_id(JceMeshRenderer), &h);
    }
    ECS_COMPONENT_DEFINE(s->world, JceCameraComponent);
    ECS_COMPONENT_DEFINE(s->world, JceDirectionalLight);
    ECS_COMPONENT_DEFINE(s->world, JcePointLight);
    ECS_COMPONENT_DEFINE(s->world, JceSpotLight);
    ECS_COMPONENT_DEFINE(s->world, JceAreaLight);
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
    ECS_COMPONENT_DEFINE(s->world, JceMusicTrackComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVideoPlayerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceScriptComponent);
    ECS_COMPONENT_DEFINE(s->world, JceEditorMeta);
    {   /* Same reason as the JceMeshRenderer ctor: zero-filling would leave
         * the interned prefab paths NULL and every reader dereferences them.
         * A ctor covers the creation paths flecs takes internally, which the
         * setter alone cannot reach. */
        ecs_type_hooks_t h;
        memset(&h, 0, sizeof(h));
        h.ctor = jce__em_ctor;
        ecs_set_hooks_id(s->world, ecs_id(JceEditorMeta), &h);
    }
    ECS_COMPONENT_DEFINE(s->world, JceTerrainComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVegetationScatterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceGrassFieldComponent);
    ECS_COMPONENT_DEFINE(s->world, JceFoliageClusterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceWaterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBuoyancyComponent);
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
    ECS_COMPONENT_DEFINE(s->world, JceLayoutElementComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIImageComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUITextComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIButtonComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUISliderComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIToggleComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIInputFieldComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIScrollViewComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIProgressBarComponent);
    ECS_COMPONENT_DEFINE(s->world, JceUIDropdownComponent);
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
    ECS_COMPONENT_DEFINE(s->world, JceFootIkComponent);
    ECS_COMPONENT_DEFINE(s->world, JceContentSizeFitterComponent);
    ECS_COMPONENT_DEFINE(s->world, JceBoneAttachmentComponent);
    ECS_COMPONENT_DEFINE(s->world, JceFullBodyIkComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSequencePlayerComponent);
    ECS_COMPONENT_DEFINE(s->world, JceMorphWeightsComponent);
    ECS_COMPONENT_DEFINE(s->world, JceNetworkVariableComponent);
    ECS_COMPONENT_DEFINE(s->world, JceGameplayAbilitySystemComponent);
    ECS_COMPONENT_DEFINE(s->world, JceRagdollComponent);
    /* Ragdoll pose relay: TRANSIENT runtime pose hand-off (runtime writes,
     * renderer reads).  Registered so it can be attached at Play, but it is
     * explicitly NOT serialized (it is rebuilt every physics step). */
    ECS_COMPONENT_DEFINE(s->world, JceRagdollPoseRelay);
    ECS_COMPONENT_DEFINE(s->world, JceAnimCmdRelay);   /* script->renderer anim relay (transient) */
    ECS_COMPONENT_DEFINE(s->world, JceFractureComponent);
    ECS_COMPONENT_DEFINE(s->world, JceVehicleComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSoftBodyComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSimLodComponent);
    ECS_COMPONENT_DEFINE(s->world, JceSceneFullscreenEffect);

    /* VideoPlayer owns a live decoder handle + a GPU texture; install
     * lifecycle hooks so those resources follow correct ownership across
     * table moves / copies / removal / entity delete / world fini. */
    jce_scene_video_install_hooks(s->world, ecs_id(JceVideoPlayerComponent));
    /* SequencePlayer owns an open JceSequencer (runtime blob); same hook
     * contract as VideoPlayer so the handle is freed on component remove,
     * entity delete, and scene destroy (world fini). */
    jce_scene_sequencer_install_hooks(s->world,
                                      ecs_id(JceSequencePlayerComponent));

    /* Starts valid and empty. It only turns invalid on overflow or an
     * allocation failure, and the query then falls back to the component
     * lookup -- correct, just slower. Missing this initialiser is not: the flag
     * stays false, the set is never populated, and the fast path it exists for
     * is silently dead. */
    s->comp_enable_ents_valid = true;
    s->str_pool = jce_str_pool_create();

    LOG_SUCCESS(LOG_TAG, "scene created");
    return s;
}

void jce_scene_destroy(JceScene *s)
{
    if (!s) return;
    jce_scene_particles_shutdown(s);                    /* free lazy particle sys */
    if (s->cloth_query) ecs_query_fini(s->cloth_query); /* before world fini */
    if (s->each_query)  ecs_query_fini(s->each_query);  /* before world fini */
    if (s->spin_query)  ecs_query_fini(s->spin_query);  /* before world fini */
    if (s->world) ecs_fini(s->world);
    if (s->world_cache.slots) JCE_FREE(s->world_cache.slots);
    if (s->xgen) JCE_FREE(s->xgen);
    if (s->comp_enable_ents) JCE_FREE(s->comp_enable_ents);
    jce_str_pool_destroy(s->str_pool);   /* after ecs_fini: components are gone */
    jce_scene_clear_streaming_settings(s);   /* frees the lazy heap block */
    jce_water_field_set_destroy(s->water_fields);
    jce_water_ripple_destroy(s->water_ripple);
    jce_terrain_cache_destroy(s->terrain_cache);
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
    /* roster_epoch is documented as "entity create/destroy" and every
     * cross-frame consumer keys cache validity on it, but a CLEAR - the
     * largest destroy there is - did not bump it.  A consumer that skipped
     * work because "the roster has not moved" therefore kept operating on a
     * roster that had just been emptied.  Found by the editor's prune
     * verifier (JCE_DBG_VERIFY_PRUNE=1), which reported 168 of 363 entities
     * dead across an unchanged epoch when Play mode reloaded the scene. */
    if (count > 0) s->roster_epoch++;

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
    if (name && name[0]) {
        /* flecs's name index is unique per scope, so ecs_set_name on a name
         * already taken in this (root) scope ABORTS the process.  Spawning the
         * same prefab repeatedly collides (e.g. many "Ped" from a SpawnManager),
         * and loading a scene with duplicate-named entities would too — so
         * uniquify by appending the entity id when the name is already in use. */
        jce_scene_name_set_unique(s, (JceEntity)e, name);
        /* Record what the caller asked for.  The flecs name above may have
         * been uniquified; that must not be observable (see the
         * JceCompAuthoredName comment). */
        {
            JceCompAuthoredName an;
            an.v = jce_str_intern(s->str_pool, name);
            ecs_set_ptr(s->world, e, JceCompAuthoredName, &an);
        }
    }

    /* Default transform. */
    JceTransform t;
    t.position = jce_v3(0, 0, 0);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1, 1, 1);
    ecs_set_ptr(s->world, e, JceTransform, &t);

    /* Active by default. */
    ecs_add(s->world, e, JceTagActive);

    s->roster_epoch++;
    return (JceEntity)e;
}

void jce_scene_destroy_entity(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    ecs_delete(s->world, (ecs_entity_t)e);
    s->roster_epoch++;
}

/* The authored name if one was recorded, else the flecs name.  The fallback
 * covers entities named through paths that predate JceCompAuthoredName and
 * entities flecs named itself. */
static const char *scene_authored_name(const JceScene *s, JceEntity e)
{
    const JceCompAuthoredName *an =
        ecs_get(s->world, (ecs_entity_t)e, JceCompAuthoredName);
    if (an && an->v && an->v[0]) return an->v;
    return ecs_get_name(s->world, (ecs_entity_t)e);
}

const char *jce_scene_entity_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return "(none)";
    const char *name = scene_authored_name(s, e);
    return name ? name : "(unnamed)";
}

const char *jce_scene_entity_registered_name(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return NULL;
    return scene_authored_name(s, e);
}

void jce_scene_set_entity_name(JceScene *s, JceEntity e, const char *name)
{
    if (!s || e == JCE_ENTITY_INVALID) return;

    /* Mirror jce_scene_create_entity: uniquify the flecs index name when it is
     * already taken (this used to call ecs_set_name unconditionally), and
     * record the authored name as the entity's identity. */
    if (name && name[0]) {
        jce_scene_name_set_unique(s, e, name);
        JceCompAuthoredName an;
        an.v = jce_str_intern(s->str_pool, name);
        ecs_set_ptr(s->world, (ecs_entity_t)e, JceCompAuthoredName, &an);
    } else {
        ecs_set_name(s->world, (ecs_entity_t)e, name);
        ecs_remove(s->world, (ecs_entity_t)e, JceCompAuthoredName);
    }
}

/* ── Parent / child hierarchy ──────────────────────────────────────── */

static void scene_set_parent_unchecked(JceScene *s, JceEntity child,
                                       JceEntity parent)
{
    if (parent == JCE_ENTITY_INVALID) {
        /* Remove parent (make root entity).  The root is a scope too, so the
         * same rule applies on the way out. */
        ecs_entity_t cur = ecs_get_parent(s->world, (ecs_entity_t)child);
        if (cur) {
            jce_scene_name_reserve_for_scope(s, child, JCE_ENTITY_INVALID);
            ecs_remove_pair(s->world, (ecs_entity_t)child, EcsChildOf, cur);
        }
    } else {
        jce_scene_name_reserve_for_scope(s, child, parent);
        ecs_add_pair(s->world, (ecs_entity_t)child,
                     EcsChildOf, (ecs_entity_t)parent);
    }
    /* Reparent changes the world matrix of `child` and its whole subtree.
       Drop the cache so a same-frame re-read (e.g. the editor's reparent
       back-solve that captures child world, reparents, then reads the new
       parent's world) never returns a pre-reparent matrix. */
    jce_scene_invalidate_world_cache(s);
}

void jce_scene_set_parent(JceScene *s, JceEntity child, JceEntity parent)
{
    (void)jce_scene_reparent(s, child, parent, false);
}

JceEntity jce_scene_get_parent(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return JCE_ENTITY_INVALID;
    /* Defense-in-depth: ecs_get_parent ACCESS_VIOLATEs on a non-alive id.
     * Editor mirrors (e.g. g_entity_order) can briefly hold ids that a flecs
     * cascade-delete already freed, so never dereference a dead entity. */
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return JCE_ENTITY_INVALID;
    ecs_entity_t p = ecs_get_parent(s->world, (ecs_entity_t)e);
    return (JceEntity)p;
}

static JcePivotComponent *scene_active_pivot(JceScene *s, JceEntity e)
{
    JcePivotComponent *p = jce_scene_get_pivot(s, e);
    if (p) {
        static int s_pivot_cid = -2;
        if (s_pivot_cid == -2) s_pivot_cid = jce_component_find("Pivot");
        if (s_pivot_cid >= 0 && !jce_scene_comp_enabled(s, e, s_pivot_cid)) p = NULL;
    }
    return p;
}

/* Local TRS matrix for one entity (scale-0 components default to 1). */
static jce_mat4 scene_local_matrix(JceScene *s, JceEntity e)
{
    JceTransform *t = jce_scene_get_transform(s, e);
    if (!t) return jce_m4_identity();
    jce_mat4 local = jce_m4_from_trs(t->position, t->rotation,
                                     jce_v3_safe_scale(t->scale));

    JcePivotComponent *p = scene_active_pivot(s, e);
    if (p && (p->local_position.x != 0.0f ||
              p->local_position.y != 0.0f ||
              p->local_position.z != 0.0f)) {
        jce_mat4 pivot = jce_m4_translate(jce_v3_negate(p->local_position));
        local = jce_m4_multiply(&local, &pivot);
    }
    return local;
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

/* Drop the intra-frame world-matrix memo (begin a new generation).  Does NOT
   touch structural_epoch — this is the per-frame drop used by
   jce_scene_update / the renderer / the pick pass, none of which represent a
   structural edit.

   O(1): bumping world_epoch alone invalidates every cached matrix, because
   the lookup (scene_world_matrix_memo) only trusts a slot whose epoch equals
   the CURRENT world_epoch — stale entries simply miss and get overwritten in
   place by the next put.  This used to also memset the whole table every
   call, which was called 2-3x per frame and cost O(cap): multi-MB of pure
   memory traffic per frame at large entity counts (150k entities => ~4MB
   table => ~60MB/s of memset at 60fps x 2-3 calls), dwarfing the work it
   "saved".  The ONLY thing the wipe provided beyond the epoch bump was
   reclaiming slots of since-destroyed entities so a streaming world cannot
   grow the table without bound — that is now done on a slow cadence instead
   (every 512 generations, amortized ~O(cap/512) per frame ~= zero). */
static void scene_drop_world_cache_frame(JceScene *s)
{
    s->world_epoch++;
    if ((s->world_epoch & 511u) == 0u &&
        s->world_cache.slots && s->world_cache.live) {
        memset(s->world_cache.slots, 0,
               (size_t)s->world_cache.cap * sizeof(JceWorldCacheSlot));
        s->world_cache.live = 0;
    }
}

/* Public invalidate: a real structural edit (set_transform / set_pivot /
   reparent / component add-remove / clear / floating-origin shift) changed at
   least one entity's world matrix.  Bumps structural_epoch (invalidating the
   renderer's cross-frame persistent static cache) AND drops the intra-frame
   memo.  See scene_drop_world_cache_frame for the per-frame, non-structural
   counterpart. */
void jce_scene_invalidate_world_cache(JceScene *s)
{
    if (!s) return;
    s->structural_epoch++;
    scene_drop_world_cache_frame(s);
}

/* Public, non-structural per-frame drop for hosts that render without
   jce_scene_update (the editor scene renderer + pick pass). */
void jce_scene_begin_render_world_cache(JceScene *s)
{
    if (!s) return;
    scene_drop_world_cache_frame(s);
}

/* Cross-frame structural epoch accessor for the scene renderer's persistent
   static world-matrix + AABB cache (large-world-opt M1 #2). */
uint64_t jce_scene_get_structural_epoch(const JceScene *s)
{
    return s ? s->structural_epoch : 0;
}

/* Frame-invariance counters (DOTS-floor slice 1) — see jce_scene.h. */
uint64_t jce_scene_get_roster_epoch(const JceScene *s)
{
    return s ? s->roster_epoch : 0;
}

uint64_t jce_scene_get_cull_data_gen(const JceScene *s)
{
    return s ? s->cull_data_gen : 0;
}

void jce_scene_bump_cull_data_gen(JceScene *s)
{
    if (s) s->cull_data_gen++;
}

uint64_t jce_scene_get_enable_gen(const JceScene *s)
{
    return s ? s->enable_gen : 0;
}

uint64_t jce_scene_get_xform_counter(const JceScene *s)
{
    return s ? s->xform_counter : 0;
}

void jce_scene_bump_enable_gen(JceScene *s)
{
    if (s) s->enable_gen++;
}

void jce_scene_notify_physics_writeback(JceScene *s)
{
    /* Physics wrote Transforms in place (no set_transform, no xgen bump) —
     * fold it into the same "some world matrix changed" counter.  The mutated
     * entity set is unknown, so incremental repair is off the table for this
     * frame: mark the dirty ring overflowed. */
    if (s) { s->xform_counter++; s->dirty_overflow = true; }
}

/* Ring-push a subtree WITHOUT bumping per-entity xgen or the structural
 * epoch: the in-place physics/nav/net write-back mutates Transforms of
 * KNOWN entities, so name them (plus descendants, whose worlds inherit the
 * change) for the renderer's L2 incremental repair instead of overflowing.
 * Any bound hit degrades to dirty_overflow ONLY — never a structural bump,
 * which would evict the whole persistent static world cache every frame. */
static void scene_dirty_push_subtree(JceScene *s, JceEntity e)
{
    JceEntity stack[256];
    int sp = 0;
    stack[sp++] = e;
    int processed = 0;
    while (sp > 0) {
        JceEntity cur = stack[--sp];
        if (s->dirty_count < (uint32_t)(sizeof s->dirty_ring / sizeof s->dirty_ring[0]))
            s->dirty_ring[s->dirty_count++] = cur;
        else { s->dirty_overflow = true; return; }
        if (++processed > 4096) { s->dirty_overflow = true; return; }
        JceEntity kids[64];
        int n = jce_scene_get_children(s, cur, kids, 64);
        if (n >= 64) { s->dirty_overflow = true; return; }
        for (int i = 0; i < n; i++) {
            if (sp >= 256) { s->dirty_overflow = true; return; }
            stack[sp++] = kids[i];
        }
    }
}

void jce_scene_notify_physics_writeback_entity(JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    s->xform_counter++;
    scene_dirty_push_subtree(s, e);
}

uint32_t jce_scene_peek_dirty_entities(const JceScene *s, JceEntity *out,
                                       uint32_t cap, bool *out_overflow)
{
    if (!s) { if (out_overflow) *out_overflow = true; return 0; }
    uint32_t n = s->dirty_count;
    bool ovf = s->dirty_overflow;
    if (out && n > 0) {
        if (n > cap) { n = cap; ovf = true; }
        memcpy(out, s->dirty_ring, (size_t)n * sizeof(JceEntity));
    }
    if (out_overflow) *out_overflow = ovf;
    return n;
}

uint32_t jce_scene_take_dirty_entities(JceScene *s, JceEntity *out,
                                       uint32_t cap, bool *out_overflow)
{
    if (!s) { if (out_overflow) *out_overflow = true; return 0; }
    uint32_t n = s->dirty_count;
    bool ovf = s->dirty_overflow;
    if (out && n > 0) {
        if (n > cap) { n = cap; ovf = true; }
        memcpy(out, s->dirty_ring, (size_t)n * sizeof(JceEntity));
    }
    if (out_overflow) *out_overflow = ovf;
    s->dirty_count    = 0;
    s->dirty_overflow = false;
    return n;
}

/* ── Per-entity transform generation (dynamic-scene world-cache) ─────────── */

static struct JceXGenSlot *xgen_find_slot(JceScene *s, uint64_t entity, bool insert)
{
    if (insert && (!s->xgen ||
        (uint64_t)(s->xgen_count + 1u) * 10u >= (uint64_t)s->xgen_cap * 7u)) {
        uint32_t newcap = s->xgen_cap ? s->xgen_cap * 2u : 256u;
        struct JceXGenSlot *nw =
            (struct JceXGenSlot *)JCE_CALLOC(newcap, sizeof(*nw));
        if (!nw) return NULL;
        /* newcap is always a power of two (256 << k), so index with a mask
         * instead of a runtime modulo (a real 32-bit DIV the compiler can't
         * strength-reduce on a runtime cap). */
        uint32_t nmask = newcap - 1u;
        for (uint32_t i = 0; i < s->xgen_cap; i++) {
            if (!s->xgen[i].entity) continue;
            uint32_t h = (uint32_t)(s->xgen[i].entity * 2654435761u) & nmask;
            for (uint32_t j = 0; j < newcap; j++) {
                uint32_t sl = (h + j) & nmask;
                if (!nw[sl].entity) { nw[sl] = s->xgen[i]; break; }
            }
        }
        if (s->xgen) JCE_FREE(s->xgen);
        s->xgen = nw; s->xgen_cap = newcap;
    }
    if (!s->xgen) return NULL;
    uint32_t cap = s->xgen_cap;
    uint32_t mask = cap - 1u;  /* cap is pow2 → mask instead of per-entity DIV */
    uint32_t h = (uint32_t)(entity * 2654435761u) & mask;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t sl = (h + i) & mask;
        struct JceXGenSlot *e = &s->xgen[sl];
        if (e->entity == entity) return e;
        if (!e->entity) {
            if (!insert) return NULL;
            e->entity = entity; e->gen = 0; s->xgen_count++;
            return e;
        }
    }
    return NULL; /* full (load<0.7 keeps this unreachable after a successful grow) */
}

uint64_t jce_scene_entity_xform_gen(const JceScene *s, JceEntity e)
{
    if (!s || !s->xgen_active) return 0;
    struct JceXGenSlot *slot = xgen_find_slot((JceScene *)s, (uint64_t)e, false);
    return slot ? slot->gen : 0;
}

bool jce_scene_xgen_active(const JceScene *s) { return s && s->xgen_active; }

/* Per-entity MATERIAL generation (lever ③ persistent draw-cmd cache). Mirrors
 * xform_gen but for material/mesh content: bumped by jce_scene_invalidate_entity_
 * material on a MeshRenderer set + async texture/model pop-in. A draw-cmd cache
 * stores the mat_gen it saw and re-builds when it differs. Reuses the xgen slot
 * table (one slot per entity holds both gen + mat_gen). */
uint64_t jce_scene_entity_material_gen(const JceScene *s, JceEntity e)
{
    if (!s || !s->xgen_active) return 0;
    struct JceXGenSlot *slot = xgen_find_slot((JceScene *)s, (uint64_t)e, false);
    return slot ? slot->mat_gen : 0;
}

/* Invalidate ONLY entity e's cached draw command (lever ③): bump its per-entity
 * material generation. Single-entity (unlike the world cache) — a material edit
 * does not propagate to children. Safe to call every frame; a persistent
 * draw-cmd cache observes the bump and rebuilds that entity. OOM (no slot) is
 * benign: mat_gen stays 0 so the entity simply never scores a cache hit. */
void jce_scene_invalidate_entity_material(JceScene *s, JceEntity e)
{
    if (!s) return;
    s->xgen_active = true;
    struct JceXGenSlot *slot = xgen_find_slot(s, (uint64_t)e, true);
    if (slot) slot->mat_gen++;
}

/* Invalidate ONLY entity e's subtree in the renderer's cross-frame static world
 * cache (bump the per-entity gen of e + every descendant, since a parent move
 * changes its children's world), plus drop the intra-frame memo.  set_transform
 * / set_pivot use this instead of the global structural_epoch bump so a moving
 * entity no longer drops the whole scene's static cache.  Bounded: a
 * pathologically wide/deep/huge subtree falls back to the global invalidate
 * (correct, just less granular). */
void jce_scene_invalidate_entity_world(JceScene *s, JceEntity e)
{
    if (!s) return;
    s->xform_counter++;   /* frame-invariance key: "some world matrix changed" */
    /* Kill-switch: fall back to the old whole-scene invalidate for A/B + safety. */
    static int s_xgen_disabled = -1;
    if (s_xgen_disabled < 0) {
        const char *v = getenv("JCE_DISABLE_XGEN");
        s_xgen_disabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    if (s_xgen_disabled) { jce_scene_invalidate_world_cache(s); return; }
    scene_drop_world_cache_frame(s);   /* intra-frame memo: same as the global path */
    s->xgen_active = true;
    JceEntity stack[256];
    int sp = 0;
    stack[sp++] = e;
    int processed = 0;
    while (sp > 0) {
        JceEntity cur = stack[--sp];
        struct JceXGenSlot *slot = xgen_find_slot(s, (uint64_t)cur, true);
        if (!slot) { jce_scene_invalidate_world_cache(s); return; }     /* OOM */
        slot->gen++;
        /* L2 dirty ring: record the invalidated entity for incremental ecull
         * repair.  Past capacity => overflow (consumer falls back to a full
         * rebuild; the fallback paths below bump structural_epoch which
         * already forces one). */
        if (s->dirty_count < (uint32_t)(sizeof s->dirty_ring / sizeof s->dirty_ring[0]))
            s->dirty_ring[s->dirty_count++] = cur;
        else
            s->dirty_overflow = true;
        if (++processed > 4096) { jce_scene_invalidate_world_cache(s); return; }
        JceEntity kids[64];
        int n = jce_scene_get_children(s, cur, kids, 64);
        if (n >= 64) { jce_scene_invalidate_world_cache(s); return; }   /* too wide */
        for (int i = 0; i < n; i++) {
            if (sp >= 256) { jce_scene_invalidate_world_cache(s); return; } /* too deep */
            stack[sp++] = kids[i];
        }
    }
}

/* Floating-origin rebase: shift the LOCAL position of every ROOT entity (no
 * parent) that has a JceTransform by `shift`, then invalidate the world cache
 * once.  Children are parent-relative, so shifting only roots moves their whole
 * subtrees by exactly `shift` while preserving all relative geometry — a child
 * is never double-shifted.  An all-zero shift (or NULL scene) is a no-op and
 * does NOT bump the world epoch, so a quiescent floating-origin pass adds zero
 * cache churn.  Implemented with the same JceTransform query jce_scene_each_
 * entity uses; positions are mutated in place (no add/remove of components), so
 * iterating the live query mid-shift is table-stable. */
static void jce_scene_apply_world_shift_impl(JceScene *s, jce_vec3 shift)
{
    if (!s->each_query) {
        s->each_query = ecs_query(s->world, {
            .terms = {{ .id = ecs_id(JceTransform) }},
        });
        if (!s->each_query) return;
    }

    ecs_iter_t it = ecs_query_iter(s->world, s->each_query);
    while (ecs_query_next(&it)) {
        JceTransform *xf = ecs_field(&it, JceTransform, 0);
        for (int i = 0; i < it.count; i++) {
            /* Roots only — a child's local position is parent-relative and
             * already follows when its (shifted) parent moves. */
            if (ecs_get_parent(s->world, it.entities[i]) != 0)
                continue;
            xf[i].position.x += shift.x;
            xf[i].position.y += shift.y;
            xf[i].position.z += shift.z;
        }
    }

    jce_scene_invalidate_world_cache(s);
}

void jce_scene_apply_world_shift(JceScene *s, const float shift[3])
{
    if (!s || !shift) return;
    jce_vec3 sh = jce_v3(shift[0], shift[1], shift[2]);
    if (sh.x == 0.0f && sh.y == 0.0f && sh.z == 0.0f)
        return;                          /* zero shift = no-op, no cache churn */
    jce_scene_apply_world_shift_impl(s, sh);
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

/* The frame an entity lives in: its parent's world matrix, or nothing at all
 * for a root.  Both world-pose calls go through this so they cannot drift on
 * what "world" means -- the getter composing one frame and the setter
 * inverting another is the shape that makes set(get(e)) move things. */
static bool scene_parent_frame(const JceScene *s, JceEntity e,
                               jce_mat4 *out_parent_world)
{
    const JceEntity p = jce_scene_get_parent(s, e);
    if (p == JCE_ENTITY_INVALID || p == e)
        return false;                                 /* root: world == local */
    *out_parent_world = jce_scene_get_world_matrix(s, p);
    return true;
}

bool jce_scene_get_world_pose(const JceScene *s, JceEntity e,
                              jce_vec3 *out_position, jce_quat *out_rotation,
                              jce_vec3 *out_scale)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    JceTransform *t = jce_scene_get_transform((JceScene *)s, e);
    if (!t) return false;

    jce_mat4 parent_world;
    if (!scene_parent_frame(s, e, &parent_world)) {
        /* VERBATIM, not round-tripped through a matrix.  jce_m4_decompose
         * costs three sqrtf and does not return exactly what jce_m4_from_trs
         * consumed, so composing-and-decomposing a root would hand every
         * caller a value that differs from the Transform it can read itself.
         * Unparented scenes -- which is every scene authored before this
         * existed -- stay bit-identical rather than nearly so. */
        if (out_position) *out_position = t->position;
        if (out_rotation) *out_rotation = t->rotation;
        if (out_scale)    *out_scale    = t->scale;
        return true;
    }

    /* safe_scale matches what scene_local_matrix feeds the renderer: a zero
     * scale would make the local matrix singular and hand the decomposition a
     * rotation built from three zero-length axes. */
    const jce_mat4 local = jce_m4_from_trs(t->position, t->rotation,
                                           jce_v3_safe_scale(t->scale));
    const jce_mat4 world = jce_m4_multiply(&parent_world, &local);
    jce_m4_decompose(&world, out_position, out_rotation, out_scale);
    return true;
}

bool jce_scene_solve_local_pose(const JceScene *s, JceEntity e,
                                jce_vec3 world_position,
                                jce_quat world_rotation,
                                jce_vec3 *out_local_position,
                                jce_quat *out_local_rotation)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    if (!jce_scene_get_transform((JceScene *)s, e)) return false;

    jce_mat4 parent_world;
    if (!scene_parent_frame(s, e, &parent_world)) {
        /* Root: the world pose IS the local one, handed back unchanged rather
         * than through jce_m4_from_trs and jce_m4_decompose, which cost three
         * sqrtf and do not round-trip.  Every scene authored before this
         * existed is unparented, so this branch is the common one AND the one
         * that has to be exact. */
        if (out_local_position) *out_local_position = world_position;
        if (out_local_rotation) *out_local_rotation = world_rotation;
        return true;
    }

    /* A SINGULAR PARENT FRAME MUST FAIL LOUDLY RATHER THAN QUIETLY.
     * jce_m4_inverse returns IDENTITY when |det| < 1e-8, so inverting a
     * collapsed parent would yield a perfectly ordinary matrix that places the
     * entity at the world pose as though it had no parent -- a wrong answer
     * wearing a correct one's clothes.  Test the basis lengths here, before
     * the inverse, and leave the entity where it is.
     *
     * THIS CANNOT CURRENTLY FIRE, AND SAYING SO IS THE POINT.  Every TRS
     * composition in this file goes through jce_v3_safe_scale, which
     * substitutes 1 for a zero scale component, so a parent authored at scale
     * 0 arrives here as a well-conditioned unit frame -- a test that sets a
     * parent's scale to zero and expects a refusal FAILS, which is how this
     * comment came to be written instead of a fixture pretending otherwise.
     * It stays as a floor: it costs nine multiplies, and the day
     * scene_local_matrix stops sanitising, the difference between this branch
     * and jce_m4_inverse's identity fallback is the difference between a
     * refusal and a body silently placed in the wrong world. */
    {
        int axis;
        for (axis = 0; axis < 3; ++axis) {
            const float *r = &parent_world.raw[axis][0];
            if (r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < 1e-16f)
                return false;
        }
    }

    /* Unit scale: the world pose being asked for is a position and a
     * rotation, and the entity's authored scale is not up for negotiation. */
    const jce_mat4 world = jce_m4_from_trs(world_position, world_rotation,
                                           jce_v3(1.0f, 1.0f, 1.0f));
    const jce_mat4 inv_parent = jce_m4_inverse(&parent_world);
    const jce_mat4 local = jce_m4_multiply(&inv_parent, &world);
    jce_m4_decompose(&local, out_local_position, out_local_rotation, NULL);
    return true;
}

bool jce_scene_set_world_pose(JceScene *s, JceEntity e, jce_vec3 position,
                              jce_quat rotation)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    JceTransform *t = jce_scene_get_transform(s, e);
    if (!t) return false;

    JceTransform next = *t;                  /* scale carried through intact */
    if (!jce_scene_solve_local_pose(s, e, position, rotation, &next.position,
                                    &next.rotation))
        return false;
    jce_scene_set_transform(s, e, &next);
    return true;
}

static bool scene_parent_chain_accepts(const JceScene *s, JceEntity child,
                                       JceEntity parent)
{
    JceEntity slow = parent;
    JceEntity fast = parent;

    while (slow != JCE_ENTITY_INVALID) {
        if (slow == child)
            return false;
        slow = jce_scene_get_parent(s, slow);

        for (int step = 0; step < 2 && fast != JCE_ENTITY_INVALID; step++) {
            if (fast == child)
                return false;
            fast = jce_scene_get_parent(s, fast);
        }

        if (slow != JCE_ENTITY_INVALID && slow == fast)
            return false;
    }

    return true;
}

bool jce_scene_reparent(JceScene *s, JceEntity child, JceEntity parent,
                        bool preserve_world)
{
    if (!s || child == JCE_ENTITY_INVALID ||
        !ecs_is_alive(s->world, (ecs_entity_t)child))
        return false;
    if (parent != JCE_ENTITY_INVALID &&
        !ecs_is_alive(s->world, (ecs_entity_t)parent))
        return false;
    if (child == parent)
        return false;
    if (!scene_parent_chain_accepts(s, child, parent))
        return false;

    if (jce_scene_get_parent(s, child) == parent)
        return true;

    JceTransform *current = jce_scene_get_transform(s, child);
    bool solve_local = preserve_world && current != NULL;
    JceTransform next = solve_local ? *current : (JceTransform){0};
    jce_mat4 child_world = solve_local
        ? jce_scene_get_world_matrix(s, child)
        : jce_m4_identity();

    scene_set_parent_unchecked(s, child, parent);

    if (solve_local) {
        jce_mat4 parent_world = parent != JCE_ENTITY_INVALID
            ? jce_scene_get_world_matrix(s, parent)
            : jce_m4_identity();
        jce_mat4 inv_parent = jce_m4_inverse(&parent_world);
        jce_mat4 local_model = jce_m4_multiply(&inv_parent, &child_world);
        JcePivotComponent *pivot = scene_active_pivot(s, child);
        if (pivot && (pivot->local_position.x != 0.0f ||
                      pivot->local_position.y != 0.0f ||
                      pivot->local_position.z != 0.0f)) {
            jce_mat4 pivot_offset = jce_m4_translate(pivot->local_position);
            local_model = jce_m4_multiply(&local_model, &pivot_offset);
        }

        jce_m4_decompose(&local_model, &next.position, &next.rotation,
                         &next.scale);
        jce_scene_set_transform(s, child, &next);
    }

    return true;
}

static jce_vec3 scene_transform_point(const jce_mat4 *m, jce_vec3 p)
{
    jce_vec4 v = jce_m4_mul_v4(m, jce_v4(p.x, p.y, p.z, 1.0f));
    if (v.w != 0.0f && v.w != 1.0f) {
        float inv_w = 1.0f / v.w;
        return jce_v3(v.x * inv_w, v.y * inv_w, v.z * inv_w);
    }
    return jce_v3(v.x, v.y, v.z);
}

static jce_vec3 scene_transform_vector(const jce_mat4 *m, jce_vec3 v)
{
    jce_vec4 r = jce_m4_mul_v4(m, jce_v4(v.x, v.y, v.z, 0.0f));
    return jce_v3(r.x, r.y, r.z);
}

jce_vec3 jce_scene_get_pivot_world_position(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID)
        return jce_v3(0.0f, 0.0f, 0.0f);

    JceScene *ms = (JceScene *)s;
    JceTransform *t = jce_scene_get_transform(ms, e);
    if (!t)
        return jce_v3(0.0f, 0.0f, 0.0f);

    JceEntity parent = jce_scene_get_parent(s, e);
    if (parent == JCE_ENTITY_INVALID || parent == e)
        return t->position;

    jce_mat4 parent_world = jce_scene_get_world_matrix(s, parent);
    return scene_transform_point(&parent_world, t->position);
}

void jce_scene_set_pivot_local_position_preserve_model(
    JceScene *s, JceEntity e, jce_vec3 local_position)
{
    if (!s || e == JCE_ENTITY_INVALID) return;

    JceTransform *cur_t = jce_scene_get_transform(s, e);
    if (!cur_t) return;

    JceTransform next_t = *cur_t;
    JcePivotComponent next_p;
    JcePivotComponent *cur_p = jce_scene_get_pivot(s, e);
    if (cur_p) {
        next_p = *cur_p;
    } else {
        memset(&next_p, 0, sizeof(next_p));
        next_p.local_rotation = jce_q_identity();
    }

    jce_vec3 delta = jce_v3_sub(local_position, next_p.local_position);
    jce_mat4 rs = jce_m4_from_trs(jce_v3(0.0f, 0.0f, 0.0f),
                                  next_t.rotation,
                                  jce_v3_safe_scale(next_t.scale));
    jce_vec3 world_delta = scene_transform_vector(&rs, delta);
    next_t.position = jce_v3_add(next_t.position, world_delta);
    next_p.local_position = local_position;

    jce_scene_set_transform(s, e, &next_t);
    jce_scene_set_pivot(s, e, &next_p);
}

void jce_scene_set_pivot_world_position_preserve_model(
    JceScene *s, JceEntity e, jce_vec3 world_position)
{
    if (!s || e == JCE_ENTITY_INVALID) return;

    JceTransform *cur_t = jce_scene_get_transform(s, e);
    if (!cur_t) return;

    jce_scene_invalidate_world_cache(s);
    jce_mat4 old_world = jce_scene_get_world_matrix(s, e);
    jce_mat4 inv_old_world = jce_m4_inverse(&old_world);
    jce_vec3 new_local_pivot =
        scene_transform_point(&inv_old_world, world_position);

    jce_vec3 new_parent_space_position = world_position;
    JceEntity parent = jce_scene_get_parent((const JceScene *)s, e);
    if (parent != JCE_ENTITY_INVALID && parent != e) {
        jce_mat4 parent_world =
            jce_scene_get_world_matrix((const JceScene *)s, parent);
        jce_mat4 inv_parent = jce_m4_inverse(&parent_world);
        new_parent_space_position =
            scene_transform_point(&inv_parent, world_position);
    }

    JceTransform next_t = *cur_t;
    JcePivotComponent next_p;
    JcePivotComponent *cur_p = jce_scene_get_pivot(s, e);
    if (cur_p) {
        next_p = *cur_p;
    } else {
        memset(&next_p, 0, sizeof(next_p));
        next_p.local_rotation = jce_q_identity();
    }

    next_t.position = new_parent_space_position;
    next_p.local_position = new_local_pivot;

    jce_scene_set_transform(s, e, &next_t);
    jce_scene_set_pivot(s, e, &next_p);
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

/* Resolve a generation-stripped bare index (high 32 bits zero — e.g. an id the
 * editor truncated from uint64 JceEntity to uint32, losing the flecs
 * generation) back to the LIVE entity with its current generation.  After a
 * world-streamed chunk unloads and reloads, the recycled index returns with a
 * higher generation; without this, the editor's gen-0 handle no longer matches
 * the live entity (ecs_is_alive false) and selection/has-component fail.  Ids
 * that already carry a generation (high bits set) pass through unchanged, so
 * engine-internal callers are byte-identical to before. */
/* Resolve a caller-supplied handle to a LIVE entity, or 0.
 *
 * This used to guess from the bit pattern: "high 32 bits zero => the caller
 * stripped the generation, so look up whoever is in that slot now".  That
 * guess is unsound, because a FIRST-GENERATION entity has generation 0 — its
 * full, correct handle is bit-identical to a bare index.  So every gen-0
 * handle took the stripped path and resolved through ecs_get_alive(), which
 * answers "who lives at this index" rather than "is this still the same
 * entity".  Destroy an entity, let flecs recycle its index, and a stale
 * handle to it silently resolved to the NEW occupant: reads returned another
 * entity's components and, worse, jce_scene_set_* wrote to it.  Gen-0 covers
 * every entity in a scene that has not recycled yet, i.e. most of them.
 *
 * Now the generation is always checked.  ecs_is_alive() compares the full id
 * including generation, so a recycled slot rejects the old handle even when
 * the old generation was 0.  Callers that genuinely hold only an index must
 * say so via jce_scene_entity_from_index(), which documents that it cannot
 * detect staleness. */
/* Returns 0 for a dead or invalid entity, so a NON-ZERO result is already proof
 * of liveness.  Every accessor below relies on that and does NOT re-test it:
 * ecs_is_alive is a flecs sparse-set lookup, and doing it twice per component
 * read cost real cycles -- the submit loop measured 70 of them per visible
 * entity for jce_scene_get_mesh_renderer alone, on 25.7k entities a frame. */
static ecs_entity_t jce_scene_resolve_entity(const JceScene *s, JceEntity e)
{
    if (!s || e == 0) return 0;
    const ecs_entity_t id = (ecs_entity_t)e;
    return ecs_is_alive(s->world, id) ? id : 0;
}

bool jce_scene_entity_alive(const JceScene *s, JceEntity e)
{
    return jce_scene_resolve_entity(s, e) != 0;
}

JceEntity jce_scene_entity_from_index(const JceScene *s, uint32_t index)
{
    if (!s || index == 0) return 0;
    /* ecs_get_alive() answers "which live entity occupies this index", which
     * is all an index can support — see the header for why that is not a
     * dangling check. */
    return (JceEntity)ecs_get_alive(s->world, (ecs_entity_t)index);
}

/* Whether a setter expanded from the macros below invalidates the entity-cull
 * freeze.  Off by default: a component the cull pass never reads must not cost
 * a full cull rebuild when a script writes it every frame.  The block that
 * declares the components jce_sr_cull.c DOES read flips this on around them,
 * and check_cull_gen_consumers.py fails the build if that file grows a read
 * whose component was declared with the no-op form. */
#define JCE_COMP_CULL_BUMP(s)   ((void)0)

#define JCE_COMP_IMPL(TYPE, NAME)                                       \
void jce_scene_set_##NAME(JceScene *s, JceEntity e, const TYPE *v)      \
{                                                                       \
    if (!s || !v) return;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    ecs_set_ptr(s->world, re, TYPE, v);                                 \
    JCE_COMP_CULL_BUMP(s);                                              \
}                                                                       \
                                                                        \
TYPE *jce_scene_get_##NAME(JceScene *s, JceEntity e)                    \
{                                                                       \
    if (!s) return NULL;                                                \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return NULL;   /* resolve already proved liveness */     \
    return (TYPE *)ecs_get_mut(s->world, re, TYPE);                    \
}                                                                       \
                                                                        \
bool jce_scene_has_##NAME(const JceScene *s, JceEntity e)               \
{                                                                       \
    if (!s) return false;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return false;  /* resolve already proved liveness */     \
    return ecs_has(s->world, re, TYPE);                                \
}                                                                       \
                                                                        \
void jce_scene_remove_##NAME(JceScene *s, JceEntity e)                  \
{                                                                       \
    if (!s) return;                                                     \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    ecs_remove(s->world, re, TYPE);                                    \
}

/* Like JCE_COMP_IMPL but the runtime set path bumps the per-entity material_gen
 * (lever ③): a gameplay/script SetMeshRenderer must invalidate any cached draw
 * command for that entity. (The editor inspector mutates via get_mut with no set
 * call; the runtime-only draw-cmd cache does not run in-editor, so that seam is
 * covered separately when/if the cache is extended to the editor.) */
#define JCE_COMP_IMPL_MATERIAL(TYPE, NAME)                              \
void jce_scene_set_##NAME(JceScene *s, JceEntity e, const TYPE *v)      \
{                                                                       \
    if (!s || !v) return;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    /* NULL interned strings: jce_scene_component_normalise.h. */       \
    TYPE _n = *v;                                                       \
    jce_scene_normalise_mesh_renderer(s, &_n);                          \
    ecs_set_ptr(s->world, re, TYPE, &_n);                               \
    jce_scene_invalidate_entity_material(s, e);                         \
    JCE_COMP_CULL_BUMP(s);                                              \
}                                                                       \
                                                                        \
TYPE *jce_scene_get_##NAME(JceScene *s, JceEntity e)                    \
{                                                                       \
    if (!s) return NULL;                                                \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return NULL;   /* resolve already proved liveness */     \
    return (TYPE *)ecs_get_mut(s->world, re, TYPE);                    \
}                                                                       \
                                                                        \
bool jce_scene_has_##NAME(const JceScene *s, JceEntity e)               \
{                                                                       \
    if (!s) return false;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return false;  /* resolve already proved liveness */     \
    return ecs_has(s->world, re, TYPE);                                \
}                                                                       \
                                                                        \
void jce_scene_remove_##NAME(JceScene *s, JceEntity e)                  \
{                                                                       \
    if (!s) return;                                                     \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    ecs_remove(s->world, re, TYPE);                                    \
    jce_scene_invalidate_entity_material(s, e);                         \
}

#define JCE_COMP_IMPL_WORLD(TYPE, NAME)                                 \
void jce_scene_set_##NAME(JceScene *s, JceEntity e, const TYPE *v)      \
{                                                                       \
    if (!s || !v) return;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    ecs_set_ptr(s->world, re, TYPE, v);                                 \
    /* No JCE_COMP_CULL_BUMP here: this macro serves Transform/Pivot, whose  \
     * changes the cull freeze already tracks through xform_counter below.   \
     * Bumping the cull generation as well would invalidate the freeze on    \
     * every entity move -- exactly what the per-entity gen exists to avoid. */\
    /* Per-entity world-cache invalidation: a transform/pivot edit changes only \
     * this entity's subtree, so bump its gen instead of the global epoch (a     \
     * moving camera/script entity no longer drops the whole static cache).      \
     * Component add/remove keep the global bump (render-kind etc. may change). */\
    jce_scene_invalidate_entity_world(s, e);                            \
}                                                                       \
                                                                        \
TYPE *jce_scene_get_##NAME(JceScene *s, JceEntity e)                    \
{                                                                       \
    if (!s) return NULL;                                                \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return NULL;   /* resolve already proved liveness */     \
    return (TYPE *)ecs_get_mut(s->world, re, TYPE);                    \
}                                                                       \
                                                                        \
bool jce_scene_has_##NAME(const JceScene *s, JceEntity e)               \
{                                                                       \
    if (!s) return false;                                               \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return false;  /* resolve already proved liveness */     \
    return ecs_has(s->world, re, TYPE);                                \
}                                                                       \
                                                                        \
void jce_scene_remove_##NAME(JceScene *s, JceEntity e)                  \
{                                                                       \
    if (!s) return;                                                     \
    ecs_entity_t re = jce_scene_resolve_entity(s, e);                   \
    if (!re) return;                                                     \
    ecs_remove(s->world, re, TYPE);                                    \
    jce_scene_invalidate_world_cache(s);                                \
}

JCE_COMP_IMPL_WORLD(JceTransform,          transform)
JCE_COMP_IMPL_WORLD(JcePivotComponent,     pivot)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL_MATERIAL(JceMeshRenderer,       mesh_renderer)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)

/* Read-only ecs_get_id variant for CONCURRENT worker-thread reads under
 * multi-threaded readonly mode. jce_scene_get_mesh_renderer uses ecs_get_mut
 * (write-intent → trips the exclusive-access assert, disallowed in readonly
 * mode); this is a verified pure read (flecs.c:9147) safe from many threads. */
const JceMeshRenderer *jce_scene_get_mesh_renderer_const(const JceScene *s, JceEntity e)
{
    if (!s) return NULL;
    ecs_entity_t re = jce_scene_resolve_entity((JceScene *)s, e);
    if (!re) return NULL;   /* resolve already proved liveness */
    return (const JceMeshRenderer *)ecs_get_id(s->world, re, ecs_id(JceMeshRenderer));
}

/* Multi-threaded readonly-mode wrappers — call from the single coordinator
 * thread, bracketing a parallel ecs_get_*_const fan-out. Makes the world fully
 * immutable so concurrent reads are flecs-sanctioned. Not thread-safe themselves. */
void jce_scene_parallel_read_begin(JceScene *s) { if (s && s->world) ecs_readonly_begin(s->world, true); }
void jce_scene_parallel_read_end(JceScene *s)   { if (s && s->world) ecs_readonly_end(s->world); }

/* O(1) scene-wide component counts (flecs table counts).  Per-frame renderer
 * loops that PROBE every collected entity for a rare component can skip
 * entirely when the scene holds none — a 150k-static-primitive world paid
 * ~0.5 µs/entity/frame in animator probes that could never hit. */
int jce_scene_count_skeletal_animators(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceSkeletalAnimatorComponent));
}

int jce_scene_count_sprite_animators(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceSpriteAnimatorComponent));
}

/* Component-filtered entity walks (flecs ecs_each: uncached, O(#matches)) —
 * lets the renderer's light selection visit ONLY the light entities instead of
 * probing every collected entity (150k probes/frame on a large static world). */
void jce_scene_each_point_light(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JcePointLight));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

/* O(#water) — the runtime buoyancy pass previously found its water surface
 * with a FULL-scene jce_scene_each_entity walk every fixed tick. */
void jce_scene_each_water(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceWaterComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

/* O(#cameras) — the runtime viewer-position fallback (no character
 * controller) previously scanned the FULL scene for the primary camera every
 * fixed tick and every frame. */
void jce_scene_each_camera(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceCameraComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_fullscreen_effect(JceScene *s, JceEntityCallback cb,
                                      void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceSceneFullscreenEffect));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; ++i)
            cb(s, (JceEntity)it.entities[i], user_data);
}

/* O(#emitters) — the editor's scene-view gizmo-icon system needs to visit ONLY
 * the particle-emitter entities: they carry no MeshRenderer and so have no GPU
 * id-buffer footprint, meaning they can't be clicked from the id readback.  A
 * screen-space source icon (drawn + hit-tested like the light/camera icons)
 * makes them selectable, which in turn lets the inspector edit them. */
void jce_scene_each_particle_emitter(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceParticleEmitterComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

/* O(1) (flecs table-count aggregate) — lets per-tick physics passes bail on
 * "no such component anywhere" without a per-body pre-walk. */
int jce_scene_count_constant_force(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceConstantForceComponent));
}

/* O(#holders) walks + O(1) counts for the per-frame renderer/system gates.
 * Each of these systems used a FULL jce_scene_each_entity probe walk every
 * frame (twice with both editor viewports) — on a 150k-entity static world
 * that was ~10ms/frame of has_component() misses across the family. */
void jce_scene_each_skybox(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceSkyboxComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}
void jce_scene_each_skeletal_animator(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceSkeletalAnimatorComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}
void jce_scene_each_video_player(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceVideoPlayerComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_volume(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceVolumeComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_decal(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceDecalComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_cloth(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceClothComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

int jce_scene_count_particle_emitters(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceParticleEmitterComponent));
}

int jce_scene_count_video_players(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceVideoPlayerComponent));
}

int jce_scene_count_volumes(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceVolumeComponent));
}

int jce_scene_count_decals(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceDecalComponent));
}

int jce_scene_count_cloth(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceClothComponent));
}

void jce_scene_each_spot_light(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceSpotLight));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

int jce_scene_count_point_lights(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JcePointLight));
}

int jce_scene_count_spot_lights(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceSpotLight));
}

int jce_scene_count_dir_lights(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceDirectionalLight));
}

int jce_scene_count_reflection_probes(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceReflectionProbeComponent));
}

int jce_scene_count_light_probe_groups(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceLightProbeGroupComponent));
}

int jce_scene_count_lod_groups(JceScene *s)
{
    if (!s || !s->world) return 0;
    return (int)ecs_count_id(s->world, ecs_id(JceLodGroupComponent));
}

void jce_scene_each_dir_light(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceDirectionalLight));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

/* Canvas entities only.  The UI overlay used to find its canvases by walking
 * EVERY entity in the world and probing has_canvas per element - 200,833
 * probes per frame in a large scene, 4.14 ms, and the "no canvases" early-out
 * came after the walk, so a scene with no UI at all paid the full price.
 * Iterating the component instead is O(canvases). */
void jce_scene_each_canvas(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceCanvasComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_reflection_probe(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceReflectionProbeComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

void jce_scene_each_light_probe_group(JceScene *s, JceEntityCallback cb, void *user_data)
{
    if (!s || !s->world || !cb) return;
    ecs_iter_t it = ecs_each_id(s->world, ecs_id(JceLightProbeGroupComponent));
    while (ecs_each_next(&it))
        for (int i = 0; i < it.count; i++)
            cb(s, (JceEntity)it.entities[i], user_data);
}

JCE_COMP_IMPL(JceCameraComponent,             camera)
JCE_COMP_IMPL(JceDirectionalLight,            dir_light)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JcePointLight,                  point_light)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JceSpotLight,                   spot_light)
/* Same cull bump as the spot beside it: a light that moves or changes size
 * changes what the culler must reconsider. */
JCE_COMP_IMPL(JceAreaLight,                   area_light)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
JCE_COMP_IMPL(JceSkyboxComponent,             skybox)
JCE_COMP_IMPL(JceSpriteRendererComponent,     sprite_renderer)
JCE_COMP_IMPL(JceSpriteAnimatorComponent,     sprite_animator)
JCE_COMP_IMPL(JceAnimatorComponent,           animator)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JceSkeletalAnimatorComponent,   skeletal_animator)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
JCE_COMP_IMPL(JceConstraintComponent,         constraint)
JCE_COMP_IMPL(JceRigidBodyComponent,          rigidbody)
JCE_COMP_IMPL(JceRigidBody2DComponent,        rigidbody2d)
JCE_COMP_IMPL(JceBoxColliderComponent,        box_collider)
JCE_COMP_IMPL(JceSphereColliderComponent,     sphere_collider)
JCE_COMP_IMPL(JceCharacterControllerComponent,character_controller)
JCE_COMP_IMPL(JceAudioSourceComponent,        audio_source)
JCE_COMP_IMPL(JceMusicTrackComponent,         music_track)
JCE_COMP_IMPL(JceVideoPlayerComponent,        video_player)
JCE_COMP_IMPL(JceScriptComponent,             script)
JCE_COMP_IMPL(JceParticleEmitterComponent,    particle_emitter)
JCE_COMP_IMPL(JceBehaviorTree,                behavior_tree)
JCE_COMP_IMPL(JceEditorMeta,                  editor_meta)
JCE_COMP_IMPL(JceTerrainComponent,            terrain)
JCE_COMP_IMPL(JceVegetationScatterComponent,  vegetation_scatter)
JCE_COMP_IMPL(JceGrassFieldComponent,         grass_field)
JCE_COMP_IMPL(JceFoliageClusterComponent,     foliage_cluster)
JCE_COMP_IMPL(JceWaterComponent,              water)
JCE_COMP_IMPL(JceBuoyancyComponent,           buoyancy)
JCE_COMP_IMPL(JceLodGroupComponent,           lod_group)
JCE_COMP_IMPL(JceVirtualCameraComponent,      virtual_camera)
JCE_COMP_IMPL(JceTriggerVolumeComponent,      trigger_volume)
JCE_COMP_IMPL(JceCapsuleColliderComponent,    capsule_collider)
JCE_COMP_IMPL(JceMeshColliderComponent,       mesh_collider)
JCE_COMP_IMPL(JceCompoundColliderComponent,   compound_collider)
JCE_COMP_IMPL(JceCollider2DComponent,         collider2d)
JCE_COMP_IMPL(JceTrailRendererComponent,      trail_renderer)
JCE_COMP_IMPL(JceLineRendererComponent,       line_renderer)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JceReflectionProbeComponent,    reflection_probe)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
JCE_COMP_IMPL(JceDecalComponent,              decal)
/* Cull-relevant: jce_sr_cull.c reads fields of this component, so a
 * write has to invalidate the entity-cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JceLightProbeGroupComponent,    light_probe_group)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
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
JCE_COMP_IMPL(JceLayoutElementComponent,      layout_element)
JCE_COMP_IMPL(JceUIImageComponent,            ui_image)
JCE_COMP_IMPL(JceUITextComponent,             ui_text)
JCE_COMP_IMPL(JceUIButtonComponent,           ui_button)
JCE_COMP_IMPL(JceUISliderComponent,           ui_slider)
JCE_COMP_IMPL(JceUIToggleComponent,           ui_toggle)
JCE_COMP_IMPL(JceUIInputFieldComponent,       ui_input_field)
JCE_COMP_IMPL(JceUIScrollViewComponent,       ui_scroll_view)
JCE_COMP_IMPL(JceUIProgressBarComponent,      ui_progress_bar)
JCE_COMP_IMPL(JceUIDropdownComponent,         ui_dropdown)
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
/* Cull-relevant: the depth/velocity prepass reads the entity layer for the
 * camera culling mask, so a layer edit must invalidate the cull freeze. */
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((s)->cull_data_gen++)
JCE_COMP_IMPL(JceLayerComponent,              layer_component)
#undef  JCE_COMP_CULL_BUMP
#define JCE_COMP_CULL_BUMP(s)   ((void)0)
JCE_COMP_IMPL(JceVolumeComponent,             volume)
JCE_COMP_IMPL(JceOcclusionPortalComponent,    occlusion_portal)
JCE_COMP_IMPL(JceNavAgentComponent,           nav_agent)
JCE_COMP_IMPL(JceIkConstraintComponent,       ik_constraints)
JCE_COMP_IMPL(JceFootIkComponent,             foot_ik)
JCE_COMP_IMPL(JceContentSizeFitterComponent,  content_size_fitter)
JCE_COMP_IMPL(JceBoneAttachmentComponent,     bone_attachment)
JCE_COMP_IMPL(JceFullBodyIkComponent,         full_body_ik)
JCE_COMP_IMPL(JceSequencePlayerComponent,     sequence_player)
JCE_COMP_IMPL(JceMorphWeightsComponent,       morph_weights)
JCE_COMP_IMPL(JceNetworkVariableComponent,    network_variable)
JCE_COMP_IMPL(JceGameplayAbilitySystemComponent, gas)
JCE_COMP_IMPL(JceRagdollComponent,            ragdoll)
JCE_COMP_IMPL(JceFractureComponent,           fracture)
JCE_COMP_IMPL(JceVehicleComponent,            vehicle)
JCE_COMP_IMPL(JceSoftBodyComponent,           soft_body)
JCE_COMP_IMPL(JceSimLodComponent,             sim_lod)
JCE_COMP_IMPL(JceSceneFullscreenEffect,       fullscreen_effect)

#undef JCE_COMP_IMPL_WORLD
#undef JCE_COMP_IMPL

/* ── Ragdoll pose relay (TRANSIENT) — custom accessors ──────────────────
 * Not generated by JCE_COMP_IMPL because the public getter copies the pose
 * into a caller buffer (out-params) rather than returning the component, and
 * the setter clamps `count` to JCE_MAX_BONES.  The relay is registered as a
 * flecs component (set/get via ecs) but is NEVER serialized — the component
 * JSON registry has no entry for it, so scene save/load skips it. */
void jce_scene_set_ragdoll_pose(JceScene *s, JceEntity e,
                                const jce_mat4 *locals, uint32_t count)
{
    if (!s || !locals) return;
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return;
    if (count > (uint32_t)JCE_MAX_BONES) count = (uint32_t)JCE_MAX_BONES;

    JceRagdollPoseRelay *r =
        (JceRagdollPoseRelay *)ecs_get_mut(s->world, (ecs_entity_t)e,
                                           JceRagdollPoseRelay);
    if (!r) {
        JceRagdollPoseRelay seed;
        memset(&seed, 0, sizeof seed);
        ecs_set_ptr(s->world, (ecs_entity_t)e, JceRagdollPoseRelay, &seed);
        r = (JceRagdollPoseRelay *)ecs_get_mut(s->world, (ecs_entity_t)e,
                                               JceRagdollPoseRelay);
        if (!r) return;
    }
    if (count) memcpy(r->locals, locals, (size_t)count * sizeof(jce_mat4));
    r->count = count;
    r->valid = true;
}

bool jce_scene_get_ragdoll_pose(const JceScene *s, JceEntity e,
                                jce_mat4 *out_locals, uint32_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!s || !out_locals) return false;
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return false;
    const JceRagdollPoseRelay *r =
        (const JceRagdollPoseRelay *)ecs_get(s->world, (ecs_entity_t)e,
                                             JceRagdollPoseRelay);
    if (!r || !r->valid) return false;
    uint32_t n = r->count;
    if (n > (uint32_t)JCE_MAX_BONES) n = (uint32_t)JCE_MAX_BONES;
    if (n) memcpy(out_locals, r->locals, (size_t)n * sizeof(jce_mat4));
    if (out_count) *out_count = n;
    return true;
}

bool jce_scene_has_ragdoll_pose(const JceScene *s, JceEntity e)
{
    if (!s) return false;
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return false;
    if (!ecs_has(s->world, (ecs_entity_t)e, JceRagdollPoseRelay)) return false;
    const JceRagdollPoseRelay *r =
        (const JceRagdollPoseRelay *)ecs_get(s->world, (ecs_entity_t)e,
                                             JceRagdollPoseRelay);
    return r && r->valid;
}

/* ── Animation SM command relay (script -> renderer, transient) ─────── */

void jce_scene_anim_push_param(JceScene *s, JceEntity e,
                               const JceAnimParamCmd *cmd)
{
    if (!s || !cmd) return;
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return;
    JceAnimCmdRelay *r =
        (JceAnimCmdRelay *)ecs_get_mut(s->world, (ecs_entity_t)e, JceAnimCmdRelay);
    if (!r) {
        JceAnimCmdRelay seed;
        memset(&seed, 0, sizeof seed);
        ecs_set_ptr(s->world, (ecs_entity_t)e, JceAnimCmdRelay, &seed);
        r = (JceAnimCmdRelay *)ecs_get_mut(s->world, (ecs_entity_t)e, JceAnimCmdRelay);
        if (!r) return;
    }
    if (r->count >= (uint32_t)JCE_ANIM_CMD_RELAY_MAX) return;   /* cap: drop overflow */
    r->cmds[r->count++] = *cmd;
}

uint32_t jce_scene_anim_take_params(JceScene *s, JceEntity e,
                                    JceAnimParamCmd *out, uint32_t max)
{
    if (!s) return 0;
    if (!ecs_is_alive(s->world, (ecs_entity_t)e)) return 0;
    if (!ecs_has(s->world, (ecs_entity_t)e, JceAnimCmdRelay)) return 0;
    JceAnimCmdRelay *r =
        (JceAnimCmdRelay *)ecs_get_mut(s->world, (ecs_entity_t)e, JceAnimCmdRelay);
    if (!r || r->count == 0) return 0;
    uint32_t n = r->count;
    if (out && max) {
        if (n > max) n = max;
        memcpy(out, r->cmds, (size_t)n * sizeof(JceAnimParamCmd));
    } else {
        n = 0;
    }
    r->count = 0;   /* drain: clear after read */
    return n;
}

/* ── Component enumeration ─────────────────────────────────────────── */

/* ── Id-keyed enable state (component registry) ───────────────────── */

/* Beyond this the set stops paying for itself and the query falls back to the
 * component lookup.  A scene with more disabled components than this is not the
 * case being optimised; correctness does not depend on the bound. */
#define JCE_COMP_DISABLE_SET_MAX 64

static void jce__disable_set_add(JceScene *s, JceEntity e)
{
    if (!s->comp_enable_ents_valid) return;
    for (int32_t i = 0; i < s->comp_enable_ents_count; i++)
        if (s->comp_enable_ents[i] == e) return;
    if (s->comp_enable_ents_count >= JCE_COMP_DISABLE_SET_MAX) {
        /* Give up rather than truncate: a partial set would answer "enabled"
         * for an entity that is disabled, which is a wrong picture, not a slow
         * one.  From here the query takes the component lookup again. */
        s->comp_enable_ents_valid = false;
        return;
    }
    if (s->comp_enable_ents_count == s->comp_enable_ents_cap) {
        const int32_t nc = s->comp_enable_ents_cap ? s->comp_enable_ents_cap * 2 : 8;
        JceEntity *ng = (JceEntity *)JCE_REALLOC(s->comp_enable_ents,
                                                 (size_t)nc * sizeof(JceEntity));
        if (!ng) { s->comp_enable_ents_valid = false; return; }
        s->comp_enable_ents = ng;
        s->comp_enable_ents_cap = nc;
    }
    s->comp_enable_ents[s->comp_enable_ents_count++] = e;
}

static void jce__disable_set_remove(JceScene *s, JceEntity e)
{
    if (!s->comp_enable_ents_valid) return;
    for (int32_t i = 0; i < s->comp_enable_ents_count; i++)
        if (s->comp_enable_ents[i] == e) {
            s->comp_enable_ents[i] =
                s->comp_enable_ents[--s->comp_enable_ents_count];
            return;
        }
}

JceStrPool *jce_scene_str_pool(JceScene *s)
{
    return s ? s->str_pool : NULL;
}

const char *jce_scene_intern(JceScene *s, const char *str)
{
    return jce_str_intern(s ? s->str_pool : NULL, str);
}

bool jce_scene_has_component_disables(const JceScene *s)
{
    return s && s->comp_enable_rows > 0;
}

int32_t jce_scene_component_disable_count(const JceScene *s)
{
    return s ? s->comp_enable_rows : 0;
}

bool jce_scene_comp_enabled_alive(const JceScene *s, JceEntity e, int comp_id)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX) return true;
    if (s->comp_enable_rows <= 0) return true;
    if (s->comp_enable_ents_valid) {
        for (int32_t i = 0; i < s->comp_enable_ents_count; i++)
            if (s->comp_enable_ents[i] == e) goto lookup;
        return true;
    }
lookup: {
        const JceCompEnableState *st =
            ecs_get(s->world, (ecs_entity_t)e, JceCompEnableState);
        if (!st) return true;
        return (st->disabled[comp_id >> 6] & (UINT64_C(1) << (comp_id & 63))) == 0;
    }
}

bool jce_scene_comp_enabled(const JceScene *s, JceEntity e, int comp_id)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX) return true;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return false;
    /* All-clear: no entity in the world carries an enable-state row (the row
     * is removed when fully re-enabled, see set_comp_enabled), so every
     * component is enabled - skip the per-entity ecs_get.  Runs AFTER
     * is_alive so dead entities still report disabled.  Read from the
     * maintained counter, not ecs_count_id: same answer, no iterator. */
    if (s->comp_enable_rows <= 0) return true;
    /* A scene typically has a handful of disable rows -- one, in the 200k
     * bench -- and the all-or-nothing check above does nothing for the other
     * 199,999 entities.  Scanning the id list answers those in a couple of
     * cycles instead of a component lookup. */
    if (s->comp_enable_ents_valid) {
        bool listed = false;
        for (int32_t i = 0; i < s->comp_enable_ents_count; i++)
            if (s->comp_enable_ents[i] == e) { listed = true; break; }
        if (!listed) return true;
    }
    const JceCompEnableState *st = ecs_get(s->world, ent, JceCompEnableState);
    if (!st) return true;   /* default enabled */
    return (st->disabled[comp_id >> 6] & (UINT64_C(1) << (comp_id & 63))) == 0;
}

void jce_scene_set_comp_enabled(JceScene *s, JceEntity e, int comp_id,
                                bool enabled)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX) return;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return;

    JceCompEnableState st = {{0}};
    const JceCompEnableState *cur = ecs_get(s->world, ent, JceCompEnableState);
    if (cur) st = *cur;

    if (enabled)
        st.disabled[comp_id >> 6] &= ~(UINT64_C(1) << (comp_id & 63));
    else
        st.disabled[comp_id >> 6] |=  (UINT64_C(1) << (comp_id & 63));

    /* Any change to a per-component enable bit invalidates every cached
     * draw list keyed on enable_gen.  jce_scene_bump_enable_gen existed but
     * had no caller anywhere in the engine, so the counter sat at 1 for the
     * lifetime of a scene: a renderer that had frozen its list kept drawing
     * meshes the script had already switched off.  The editor Game View hit
     * this and the standalone runtime did not -- their caches happen to be
     * invalidated by different things -- which is exactly the class of
     * editor/runtime divergence the parity gate exists to prevent. */
    if ((st.disabled[0] | st.disabled[1] | st.disabled[2] | st.disabled[3]) == 0) {
        if (ecs_has(s->world, ent, JceCompEnableState)) {
            ecs_remove(s->world, ent, JceCompEnableState);
            if (s->comp_enable_rows > 0) s->comp_enable_rows--;
            jce__disable_set_remove(s, e);
            jce_scene_bump_enable_gen(s);
        }
        return;
    }
    if (!cur || memcmp(cur, &st, sizeof st) != 0) {
        ecs_set_ptr(s->world, ent, JceCompEnableState, &st);
        if (!cur) {
            s->comp_enable_rows++;   /* a row was ADDED, not edited */
            jce__disable_set_add(s, e);
        }
        jce_scene_bump_enable_gen(s);
    }
}

void jce_scene_mark_comp_script_driven(JceScene *s, JceEntity e, int comp_id)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX) return;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return;

    JceCompScriptDriven st = {{0}};
    const JceCompScriptDriven *cur = ecs_get(s->world, ent, JceCompScriptDriven);
    if (cur) st = *cur;

    const uint64_t bit = UINT64_C(1) << (comp_id & 63);
    if (cur && (st.driven[comp_id >> 6] & bit)) return;   /* already marked */
    st.driven[comp_id >> 6] |= bit;
    ecs_set_ptr(s->world, ent, JceCompScriptDriven, &st);
}

bool jce_scene_comp_script_driven(const JceScene *s, JceEntity e, int comp_id)
{
    if (!s || e == JCE_ENTITY_INVALID) return false;
    if (comp_id < 0 || comp_id >= JCE_COMP_MAX) return false;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return false;
    if (ecs_count_id(s->world, ecs_id(JceCompScriptDriven)) == 0) return false;
    const JceCompScriptDriven *st = ecs_get(s->world, ent, JceCompScriptDriven);
    if (!st) return false;
    return (st->driven[comp_id >> 6] & (UINT64_C(1) << (comp_id & 63))) != 0;
}

/* ── Legacy 64-bit mask shims (flag-keyed callers keep working) ───── */

/* Rebuild the legacy 64-bit DISABLED mask from the id-keyed store: only
 * rows that carry a legacy flag can be expressed in it. */
uint64_t jce_scene_get_disabled_components(const JceScene *s, JceEntity e)
{
    if (!s || e == JCE_ENTITY_INVALID) return 0;
    ecs_entity_t ent = (ecs_entity_t)e;
    if (!ecs_is_alive(s->world, ent)) return 0;
    const JceCompEnableState *st = ecs_get(s->world, ent, JceCompEnableState);
    if (!st) return 0;

    uint64_t mask = 0;
    const int n = jce_component_count();
    for (int id = 0; id < n; id++) {
        if ((st->disabled[id >> 6] & (UINT64_C(1) << (id & 63))) == 0) continue;
        mask |= jce_component_legacy_flag(id);
    }
    return mask;
}

/* Apply a legacy mask: sets/clears the bits of every flag-carrying row.
 * Id-keyed disabled state for post-64 components is preserved. */
void jce_scene_set_disabled_components(JceScene *s, JceEntity e, uint64_t mask)
{
    if (!s || e == JCE_ENTITY_INVALID) return;
    const int n = jce_component_count();
    for (int id = 0; id < n; id++) {
        uint64_t flag = jce_component_legacy_flag(id);
        if (!flag) continue;
        jce_scene_set_comp_enabled(s, e, id, (mask & flag) == 0);
    }
}

bool jce_scene_component_enabled_alive(const JceScene *s, JceEntity e,
                                       uint64_t flag)
{
    int id = jce_component_from_legacy_flag(flag);
    if (id == JCE_COMP_ID_INVALID) return true;
    return jce_scene_comp_enabled_alive(s, e, id);
}

bool jce_scene_component_enabled(const JceScene *s, JceEntity e, uint64_t flag)
{
    /* Default enabled: unknown/retired flags read as enabled. */
    int id = jce_component_from_legacy_flag(flag);
    if (id == JCE_COMP_ID_INVALID) return true;
    return jce_scene_comp_enabled(s, e, id);
}

void jce_scene_set_component_enabled(JceScene *s, JceEntity e, uint64_t flag, bool enabled)
{
    int id = jce_component_from_legacy_flag(flag);
    if (id == JCE_COMP_ID_INVALID) return;
    jce_scene_set_comp_enabled(s, e, id, enabled);
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

/* ── Scene-graph queries (Unity Find / OverlapSphere, Godot groups) ─────
 * Collect matching entities into a caller buffer; return the count written
 * (<= max).  All iterate transform-bearing entities via jce_scene_each_entity.
 * (Query-by-component awaits a public generic comp-presence-by-id accessor.) */
typedef struct {
    JceEntity  *out;
    int         max;
    int         n;
    const char *str;     /* tag / name to match (by_tag, by_name) */
    jce_vec3    center;   /* sphere centre (query_sphere) */
    float       r2;       /* sphere radius squared */
} SceneQueryCtx;

static void scene_q_tag_cb(JceScene *s, JceEntity e, void *ud)
{
    SceneQueryCtx *q = (SceneQueryCtx *)ud;
    if (q->n >= q->max) return;
    const char *t = jce_scene_get_entity_tag_name(s, e);
    if (t && q->str && t[0] && strcmp(t, q->str) == 0) q->out[q->n++] = e;
}

static void scene_q_name_cb(JceScene *s, JceEntity e, void *ud)
{
    SceneQueryCtx *q = (SceneQueryCtx *)ud;
    if (q->n >= q->max) return;
    const char *nm = jce_scene_entity_name(s, e);
    if (nm && q->str && strcmp(nm, q->str) == 0) q->out[q->n++] = e;
}

static void scene_q_sphere_cb(JceScene *s, JceEntity e, void *ud)
{
    SceneQueryCtx *q = (SceneQueryCtx *)ud;
    if (q->n >= q->max) return;
    JceTransform *t = jce_scene_get_transform(s, e);
    if (!t) return;
    float dx = t->position.x - q->center.x;
    float dy = t->position.y - q->center.y;
    float dz = t->position.z - q->center.z;
    if (dx*dx + dy*dy + dz*dz <= q->r2) q->out[q->n++] = e;
}

int jce_scene_query_by_tag(JceScene *s, const char *tag,
                           JceEntity *out, int max)
{
    if (!s || !tag || !out || max <= 0) return 0;
    SceneQueryCtx q; memset(&q, 0, sizeof(q));
    q.out = out; q.max = max; q.str = tag;
    jce_scene_each_entity(s, scene_q_tag_cb, &q);
    return q.n;
}

int jce_scene_query_by_name(JceScene *s, const char *name,
                            JceEntity *out, int max)
{
    if (!s || !name || !out || max <= 0) return 0;
    SceneQueryCtx q; memset(&q, 0, sizeof(q));
    q.out = out; q.max = max; q.str = name;
    jce_scene_each_entity(s, scene_q_name_cb, &q);
    return q.n;
}

int jce_scene_query_sphere(JceScene *s, jce_vec3 center, float radius,
                           JceEntity *out, int max)
{
    if (!s || !out || max <= 0 || radius < 0.0f) return 0;
    SceneQueryCtx q; memset(&q, 0, sizeof(q));
    q.out = out; q.max = max; q.center = center; q.r2 = radius * radius;
    jce_scene_each_entity(s, scene_q_sphere_cb, &q);
    return q.n;
}

void jce_scene_update(JceScene *s, float dt)
{
    JCE_PROFILE_ZONE_N("Scene::Update");
    if (!s) { JCE_PROFILE_ZONE_END; return; }

    /* New frame → drop last frame's intra-frame world-matrix memo. Done before
       ecs_progress so any system that reads world matrices this frame builds
       a fresh, consistent cache against transforms as they are at read time.
       Per-frame, NOT a structural edit → use the non-structural drop so the
       renderer's cross-frame persistent static cache survives a quiet frame. */
    scene_drop_world_cache_frame(s);

    /* Headless has no renderer, which used to be the only driver of this. */
    jce_scene_environment_advance(s, dt);
    ecs_progress(s->world, dt);

    /* JCE_STRESS_SPIN (#4 ECS entity-count benchmark): each frame iterate every
     * entity with a JceTransform and integrate it (the standard flecs N-entity
     * Transform/Velocity throughput test) — pure component read+write over the
     * flecs storage, measured via the "ecs_move" perf-phase.  Off by default
     * (env unset) → zero cost.
     *
     * Two callers, two visibility contracts:
     *   • headless env JCE_STRESS_SPIN (bench_root == 0): does NOT invalidate the
     *     world cache — the metric is raw ECS iteration throughput, not the
     *     rendered result, so the persistent static render caches stay warm.
     *   • the editor "Entity Count" benchmark (bench_root != 0): the spawned
     *     cubes are plain STATIC MeshRenderers (no rigidbody), so the renderer
     *     keeps them in its cross-frame persistent static world cache and draws
     *     STALE matrices — the Transforms update (visible on select) but the
     *     render is frozen.  After the in-place integrate we bump structural_
     *     epoch once (jce_scene_invalidate_world_cache — the same pattern the
     *     floating-origin rebase uses after ITS in-place bulk move) so the static
     *     world cache + collect-list freeze drop and every visible entity
     *     recomposes its world matrix from the updated Transforms. */
    {
        static int s_spin = -1;
        if (s_spin < 0) { const char *e = getenv("JCE_STRESS_SPIN");
                          s_spin = (e && e[0] && e[0] != '0') ? 1 : 0; }
        if (s_spin || jce_scene_stress_spin_runtime) {
            uint64_t _t0 = jce_time_perf_counter();
            if (!s->spin_query) {
                /* Move every JceTransform EXCEPT the entities that drive the
                 * view (Camera / CharacterController): the benchmark is a raw
                 * N-entity throughput test and must not translate the player /
                 * camera, or the game-view third-person follow drifts steadily
                 * in +X/+Y (the integrate direction) as reported. */
                s->spin_query = ecs_query(s->world, {
                    .terms = {
                        { .id = ecs_id(JceTransform) },
                        { .id = ecs_id(JceCameraComponent),
                          .oper = EcsNot },
                        { .id = ecs_id(JceCharacterControllerComponent),
                          .oper = EcsNot },
                    }
                });
            }
            if (s->spin_query) {
                /* Benchmark scoping: when the entity-count benchmark set a root,
                 * integrate ONLY entities under it (walk the ChildOf chain, the
                 * same way the editor's bench_root_of does).  This is what stops
                 * the REAL scene's textured models from translating away while
                 * the benchmark runs; env JCE_STRESS_SPIN (root 0) still moves
                 * everything. */
                const uint32_t bench_root = jce_scene_stress_spin_root;
                int moved = 0;
                /* Bounded orbit velocity: integrating sin/cos over time keeps each
                 * cube's accumulated offset within ~±1–2 units of where it spawned
                 * (∫sin dt = 1−cos ∈ [0,2]), so the grid gently orbits + spins in
                 * PLACE.  The old raw `position += dt` accumulated unbounded, and
                 * combined with the root being spun too it smeared the group into
                 * a spiral — that is the "扭曲伸展" (twist/stretch) seen on the
                 * selection outline.  sin/cos are evaluated once per frame here,
                 * not per entity, so the ecs_move throughput cost is unchanged. */
                static float s_spin_phase = 0.0f;
                s_spin_phase += dt;
                const float vx = sinf(s_spin_phase);
                const float vy = cosf(s_spin_phase);
                ecs_iter_t it = ecs_query_iter(s->world, s->spin_query);
                while (ecs_query_next(&it)) {
                    JceTransform *xf = ecs_field(&it, JceTransform, 0);
                    for (int i = 0; i < it.count; i++) {
                        const uint32_t ent = (uint32_t)it.entities[i];
                        if (bench_root) {
                            /* Anchor the group root itself — spinning BOTH the
                             * root and its children compounded (parent∘child) into
                             * the spiral distortion.  Move strict descendants. */
                            if (ent == bench_root) continue;
                            uint32_t p = ent;
                            int under = 0;
                            for (int g = 0; p && g < 64; ++g) {
                                if (p == bench_root) { under = 1; break; }
                                uint32_t np =
                                    (uint32_t)jce_scene_get_parent(s, (JceEntity)p);
                                if (np == 0u || np == (uint32_t)JCE_ENTITY_INVALID)
                                    break;
                                p = np;
                            }
                            if (!under) continue;
                        }
                        xf[i].position.x += vx * dt;     /* bounded orbit  */
                        xf[i].position.y += vy * dt * 0.5f;
                        xf[i].rotation.y += dt;          /* spin in place  */
                        if (xf[i].rotation.y > 6.2831853f)
                            xf[i].rotation.y -= 6.2831853f;
                        moved = 1;
                    }
                }
                /* Editor "Entity Count" benchmark: the cubes are plain STATIC
                 * MeshRenderers, so the renderer caches their world matrices in
                 * its cross-frame persistent static cache (keyed on structural_
                 * epoch + per-entity xform_gen).  The in-place integrate above
                 * bumps NEITHER key, so the cache serves stale matrices and the
                 * render is frozen even though the Transforms update.  Bump
                 * structural_epoch once for the whole batch (the proven pattern
                 * jce_scene_apply_world_shift_impl uses after its in-place bulk
                 * move): it invalidates the static world cache + the collect-list
                 * freeze so every visible entity recomposes from the updated
                 * Transforms and the cubes visibly move.  Headless env path
                 * (bench_root == 0) stays raw so its ecs_move number is unchanged. */
                if (bench_root && moved) jce_scene_invalidate_world_cache(s);
            }
            jce_perf_phase_add("ecs_move",
                               jce_time_perf_to_ms(_t0, jce_time_perf_counter()));
        }
    }

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
                        /* Authoring a cloth is an explicit opt-in: force the
                         * soft-body sim ON so the patch actually steps, mirroring
                         * rt_try_spawn_softbody.  Without this it stays at the
                         * render-tier default (OFF on LOW/MID) and the authored
                         * cloth sits frozen — the reported "首次运行全部静止" bug. */
                        if (cc->handle != 0)
                            jce_cloth_set_simulation_enabled(true);
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
