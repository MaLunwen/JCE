/*
 * jce_scene_particles.c  ParticleEmitter component runtime (P2-particle-vfx).
 *
 * Bridges authored particle assets to the real engine particle system:
 *
 *   - The scene lazily owns ONE JceParticleSystem (created on first tick that
 *     finds a JceParticleEmitterComponent, freed in jce_scene_destroy).
 *   - Each component maps to one emitter in that system.  On first sight (or
 *     when asset_path changes) the emitter is (re)created from the authored
 *     `*.particles.json`; with no asset set, the legacy quick-tune fields
 *     (emit_rate / lifetime) drive a default emitter so old scenes still run.
 *   - Every tick the emitter origin is synced to the entity's world position
 *     and the simulation is stepped once.  Rendering (debug-draw billboards)
 *     is owned by the scene renderer (jce_scene_renderer.c) so there is a
 *     single render path for both the editor and the shipping runtime.
 *   - Orphaned emitters (component deleted / asset cleared) are reaped via a
 *     mark-and-sweep keyed on the live components seen this frame, so no
 *     flecs dtor hook (which has no access to the scene's system) is needed.
 *
 * Layer: Middleware/scene.  Consumes renderer/particles.
 */

#include "jce_scene_internal.h"

#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "scene_particles"

/* Mirror the engine particle system cap so the sweep set is the same size. */
#define SP_MAX_EMITTERS 256

/* ── GPU routing predicate (shared with jce_scene_renderer.c) ──────────
 *
 * Process-wide latch: flipped when a GPU particle system failed to come up
 * (compute caps lied / shaders missing from the pak).  Once set, every
 * emitter routes back to the CPU path for the rest of the process. */
static bool s_gpu_particles_blocked = false;

void jce_scene_internal_gpu_particles_set_blocked(void)
{
    if (!s_gpu_particles_blocked)
        LOG_WARN(LOG_TAG, "GPU particle system unavailable "
                 "(compute unsupported or shaders missing); "
                 "all emitters fall back to CPU simulation");
    s_gpu_particles_blocked = true;
}

bool jce_scene_internal_gpu_particles_blocked(void)
{
    return s_gpu_particles_blocked;
}

bool jce_scene_particle_emitter_uses_gpu(const JceParticleEmitterComponent *c)
{
    if (!c || !c->gpu) return false;
    if (s_gpu_particles_blocked) return false;
    if (!jce_render_pipeline_is_feature_enabled("gpu_particles")) return false;
    /* jce_renderer_get_caps() is NULL-safe pre-bgfx-init (returns 0). */
    if ((jce_renderer_get_caps() & JCE_CAP_COMPUTE) == 0) return false;
    return true;
}

/* ── Asset-change marker ───────────────────────────────────────────────
 *
 * A cheap FNV-1a over asset_path + the legacy tuning fields lets us detect
 * authoring edits (path changed, or emit_rate/lifetime nudged in the
 * inspector when no asset is set) and rebuild the emitter only then.  The
 * gpu flag is folded in so toggling CPU↔GPU rebuilds on either side. */
uint64_t jce_scene_particle_emitter_epoch(const JceParticleEmitterComponent *c)
{
    size_t len = 0;
    while (len < sizeof(c->asset_path) && c->asset_path[len]) len++;
    uint64_t h = jce_fnv1a64_append(JCE_FNV1A64_INIT, c->asset_path, len);
    if (c->asset_path[0] == '\0') {
        /* Legacy fields only matter when no asset drives the emitter. */
        h = jce_fnv1a64_append(h, &c->emit_rate, sizeof(float) * 3);
    }
    const uint8_t tag = c->gpu ? 0x47u : 0x43u;   /* 'G' / 'C' */
    h = jce_fnv1a64_append(h, &tag, sizeof(tag));
    if (h == 0) h = 1; /* reserve 0 for "never loaded" */
    return h;
}

/* ── Authored desc resolution (shared with the GPU driver) ───────────── */

void jce_scene_particle_emitter_desc(const JceParticleEmitterComponent *c,
                                     JceParticleEmitterDesc *out)
{
    if (!out) return;
    if (c && c->asset_path[0]) {
        jce_particles_desc_load_json(c->asset_path, out, NULL, 0);
        return;
    }
    /* No asset: synthesize from the legacy quick-tune fields. */
    jce_particles_desc_default(out);
    if (!c) return;
    if (c->emit_rate    > 0.0f) out->emit_rate    = c->emit_rate;
    if (c->lifetime_min > 0.0f) out->lifetime_min = c->lifetime_min;
    if (c->lifetime_max > 0.0f) out->lifetime_max = c->lifetime_max;
    if (out->lifetime_max < out->lifetime_min)
        out->lifetime_max = out->lifetime_min;
}

/* ── Per-frame context ─────────────────────────────────────────────── */

typedef struct {
    JceScene          *scene;
    JceParticleSystem *sys;
    /* mark-and-sweep: emitter idx -> referenced by a live component? */
    bool               referenced[SP_MAX_EMITTERS];
} SpCtx;

/* ── Emitter (re)build from authoring data ─────────────────────────── */

static void sp_build_emitter(SpCtx *ctx, JceParticleEmitterComponent *c)
{
    /* Drop any previous emitter for this component first. */
    if (c->loaded && c->emitter_handle_idx != UINT32_MAX) {
        JceEmitterHandle old = { c->emitter_handle_idx };
        jce_particles_emitter_remove(ctx->sys, old);
    }
    c->loaded             = false;
    c->emitter_handle_idx = UINT32_MAX;

    JceParticleEmitterDesc desc;
    jce_scene_particle_emitter_desc(c, &desc);

    JceEmitterHandle h = jce_particles_emitter_add(ctx->sys, &desc);
    if (!jce_emitter_valid(h)) {
        LOG_WARN(LOG_TAG, "emitter pool full; particle component skipped");
        return;
    }
    c->emitter_handle_idx = h.idx;
    c->loaded             = true;
    c->asset_epoch        = jce_scene_particle_emitter_epoch(c);
    jce_particles_emitter_start(ctx->sys, h);
}

/* ── Per-entity tick ──────────────────────────────────────────────── */

static void sp_each(JceScene *s, JceEntity e, void *ud)
{
    SpCtx *ctx = (SpCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;

    /* GPU-routed this frame: the scene renderer owns the compute-driven
     * system.  Drop any CPU emitter we previously built and do NOT mark it
     * referenced — the sweep below reaps it. */
    if (jce_scene_particle_emitter_uses_gpu(c)) {
        if (c->loaded && c->emitter_handle_idx != UINT32_MAX) {
            JceEmitterHandle old = { c->emitter_handle_idx };
            jce_particles_emitter_remove(ctx->sys, old);
        }
        c->loaded             = false;
        c->emitter_handle_idx = UINT32_MAX;
        return;
    }

    /* (Re)build when never loaded or when authoring data changed. */
    uint64_t epoch = jce_scene_particle_emitter_epoch(c);
    if (!c->loaded || c->emitter_handle_idx == UINT32_MAX ||
        c->asset_epoch != epoch) {
        sp_build_emitter(ctx, c);
    }
    if (!c->loaded || c->emitter_handle_idx == UINT32_MAX) return;

    JceEmitterHandle h = { c->emitter_handle_idx };
    ctx->referenced[c->emitter_handle_idx] = true;

    /* Keep the emitter origin at the entity's world position (the scene
     * renderer draws alive particles from this same emitter handle). */
    jce_mat4 w = jce_scene_get_world_matrix(s, e);
    jce_vec3 pos = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
    jce_particles_emitter_set_position(ctx->sys, h, pos);
}

/* Detect whether the scene has any particle component (so we don't create an
 * empty system — and step debug-draw — for scenes that have none). */
static void sp_probe(JceScene *s, JceEntity e, void *ud)
{
    if (jce_scene_has_particle_emitter(s, e)) *(bool *)ud = true;
}

void jce_scene_particles_update(JceScene *s, float dt)
{
    if (!s) return;

    JceParticleSystem *sys =
        (JceParticleSystem *)jce_scene_internal_particles_get(s);

    if (!sys) {
        bool any = false;
        jce_scene_each_entity(s, sp_probe, &any);
        if (!any) return;   /* nothing to do; stay allocation-free */
        sys = jce_particles_create(jce_allocator_default());
        if (!sys) return;
        jce_scene_internal_particles_set(s, sys);
    }

    SpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scene = s;
    ctx.sys   = sys;

    /* Per-entity: build/sync/draw, marking referenced emitters. */
    jce_scene_each_entity(s, sp_each, &ctx);

    /* Step the whole simulation once (clamp long frames). */
    if (dt > 0.1f) dt = 0.1f;
    jce_particles_update(sys, dt);

    /* Sweep: an emitter that is allocated but went unreferenced this frame
     * belonged to a now-deleted (or asset-cleared) component — reap it. */
    for (uint32_t i = 0; i < SP_MAX_EMITTERS; ++i) {
        if (ctx.referenced[i]) continue;
        JceEmitterHandle h = { i };
        if (jce_particles_emitter_is_alive(sys, h))
            jce_particles_emitter_remove(sys, h);
    }
}

void jce_scene_particles_shutdown(JceScene *s)
{
    if (!s) return;
    JceParticleSystem *sys =
        (JceParticleSystem *)jce_scene_internal_particles_get(s);
    if (sys) {
        jce_particles_destroy(sys);
        jce_scene_internal_particles_set(s, NULL);
    }
}
