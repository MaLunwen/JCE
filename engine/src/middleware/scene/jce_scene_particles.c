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
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "scene_particles"

/* Mirror the engine particle system cap so the sweep set is the same size. */
#define SP_MAX_EMITTERS 256

/* ── Asset-change marker ───────────────────────────────────────────────
 *
 * A cheap FNV-1a over asset_path + the legacy tuning fields lets us detect
 * authoring edits (path changed, or emit_rate/lifetime nudged in the
 * inspector when no asset is set) and rebuild the emitter only then. */
static uint64_t sp_asset_epoch(const JceParticleEmitterComponent *c)
{
    uint64_t h = 1469598103934665603ull;
    const unsigned char *p = (const unsigned char *)c->asset_path;
    for (int i = 0; i < (int)sizeof(c->asset_path) && p[i]; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    if (c->asset_path[0] == '\0') {
        /* Legacy fields only matter when no asset drives the emitter. */
        const unsigned char *f = (const unsigned char *)&c->emit_rate;
        for (size_t i = 0; i < sizeof(float) * 3; ++i) {
            h ^= f[i];
            h *= 1099511628211ull;
        }
    }
    if (h == 0) h = 1; /* reserve 0 for "never loaded" */
    return h;
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
    if (c->asset_path[0]) {
        jce_particles_desc_load_json(c->asset_path, &desc, NULL, 0);
    } else {
        /* No asset: synthesize from the legacy quick-tune fields. */
        jce_particles_desc_default(&desc);
        if (c->emit_rate    > 0.0f) desc.emit_rate    = c->emit_rate;
        if (c->lifetime_min > 0.0f) desc.lifetime_min = c->lifetime_min;
        if (c->lifetime_max > 0.0f) desc.lifetime_max = c->lifetime_max;
        if (desc.lifetime_max < desc.lifetime_min)
            desc.lifetime_max = desc.lifetime_min;
    }

    JceEmitterHandle h = jce_particles_emitter_add(ctx->sys, &desc);
    if (!jce_emitter_valid(h)) {
        LOG_WARN(LOG_TAG, "emitter pool full; particle component skipped");
        return;
    }
    c->emitter_handle_idx = h.idx;
    c->loaded             = true;
    c->asset_epoch        = sp_asset_epoch(c);
    jce_particles_emitter_start(ctx->sys, h);
}

/* ── Per-entity tick ──────────────────────────────────────────────── */

static void sp_each(JceScene *s, JceEntity e, void *ud)
{
    SpCtx *ctx = (SpCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;

    /* (Re)build when never loaded or when authoring data changed. */
    uint64_t epoch = sp_asset_epoch(c);
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
