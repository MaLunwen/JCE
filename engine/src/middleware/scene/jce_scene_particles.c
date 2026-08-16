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
#include <jce/resource/jce_pak_loader.h>   /* PAK-first .particles.json (single-exe) */
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <stdio.h>    /* snprintf (asset-root anchor) */
#include <string.h>
#include <stdlib.h>   /* getenv (JCE_GPU_PARTICLES_FORCE) */

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
    /* Routing: default to the CPU particle path on EVERY backend.  The GPU
     * compute pool is a dynamic VB used as both a compute UAV and instance
     * data — bgfx 1.129 mis-renders that dual use on Vulkan/D3D12 (over-bright
     * blob / device-removed; exhaustively investigated — even the official
     * nbody same-view pattern didn't fix it, and Conan Center has no newer
     * bgfx).  Rather than keep two divergent-looking paths, the CPU path now
     * renders with the SAME vs/fs_particle sprite shaders (sr_draw_particles),
     * so a single, backend-consistent soft-circle billboard covers all four
     * backends.  JCE_GPU_PARTICLES_FORCE=1 opts an emitter back onto the GPU
     * compute pool (for A/B / large-count testing on D3D11/GL). */
    {
        static int s_force = -1;
        if (s_force < 0) {
            const char *v = getenv("JCE_GPU_PARTICLES_FORCE");
            s_force = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (!s_force) return false;   /* CPU path everywhere */
    }
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

/* Optional asset-root anchor for the *.particles.json reads.  The component
 * stores a project-relative path (e.g. "particles/flame.particles.json") but
 * jce_particles_desc_load_json reads CWD-relative, which only works when the
 * process happens to run beside the assets.  The editor anchors this at the
 * project's source_assets dir; default_main anchors it at <exe>/cooked_assets.
 * Process-global (like the GPU-particles fallback latch): one project is
 * active per process. */
static char s_sp_asset_root[512];

/* Embedded PAK for the single-exe path: the *.particles.json is packed into the
 * game's PAK (assetPath is a bundle dep key), but the host-fs reads below miss
 * in a single-exe build (no loose cooked tree).  default_main publishes the
 * overlaid engine PAK here so the desc loads straight from the PAK.  NULL in
 * the editor (which anchors on the source-assets root instead). */
static const JcePakArchive *s_sp_pak = NULL;

void jce_scene_particles_set_asset_root(const char *root)
{
    if (!root) { s_sp_asset_root[0] = '\0'; return; }
    snprintf(s_sp_asset_root, sizeof s_sp_asset_root, "%s", root);
}

void jce_scene_particles_set_pak(const struct JcePakArchive *pak)
{
    s_sp_pak = (const JcePakArchive *)pak;
}

void jce_scene_particle_emitter_desc_tex(const JceParticleEmitterComponent *c,
                                         JceParticleEmitterDesc *out,
                                         char *tex_path, int tex_cap)
{
    if (!out) return;
    if (tex_path && tex_cap > 0) tex_path[0] = '\0';
    /* Seed a valid descriptor BEFORE any load is attempted.  Callers pass an
     * uninitialised stack struct (jce_scene_particles.c sp_emitter_build), and
     * the authored-asset branch below used to return without writing *out when
     * every load path missed -- handing garbage emit_rate/lifetimes and a wild
     * sub-emitter pointer to jce_particles_emitter_add, which then got freed.
     * Defaulting first makes an unreadable asset degrade to a visible plain
     * emitter instead of undefined behaviour. */
    jce_particles_desc_default(out);
    if (c && c->asset_path[0]) {
        /* PAK-first (single-exe: the .particles.json is in the embedded PAK). */
        if (s_sp_pak) {
            const JcePakAsset *a =
                jce_pak_find((JcePakArchive *)s_sp_pak, c->asset_path);
            if (a && a->original_size && a->original_size <= (1u << 20)) {
        char *buf = (char *)JCE_MALLOC((size_t)a->original_size);
                if (buf) {
                    bool ok =
                        jce_pak_decompress(a, buf, (size_t)a->original_size) ==
                            (size_t)a->original_size &&
                        jce_particles_desc_load_json_mem(
                            buf, (size_t)a->original_size, out, tex_path, tex_cap);
        JCE_FREE(buf);
                    if (ok) return;
                }
            }
        }
        /* Anchored host read next (quiet when the root is authoritative), then
         * the raw path (absolute paths / CWD-staged layouts keep working). */
        if (s_sp_asset_root[0]) {
            char full[768];
            snprintf(full, sizeof full, "%s/%s", s_sp_asset_root, c->asset_path);
            if (jce_particles_desc_load_json(full, out, tex_path, tex_cap))
                return;
        }
        if (jce_particles_desc_load_json(c->asset_path, out, tex_path, tex_cap))
            return;
        /* Every path missed.  This used to be silent, so a wrong asset root
         * (the editor Game View never set one) produced empty emitters with
         * nothing in the log to say why. */
        LOG_WARN(LOG_TAG,
                 "particle asset '%s' not found in PAK or under root '%s';"
                 " emitter falls back to the default descriptor",
                 c->asset_path, s_sp_asset_root[0] ? s_sp_asset_root : "(unset)");
        return;
    }
    /* No asset: synthesize from the legacy quick-tune fields. */
    if (!c) return;
    if (c->emit_rate    > 0.0f) out->emit_rate    = c->emit_rate;
    if (c->lifetime_min > 0.0f) out->lifetime_min = c->lifetime_min;
    if (c->lifetime_max > 0.0f) out->lifetime_max = c->lifetime_max;
    if (out->lifetime_max < out->lifetime_min)
        out->lifetime_max = out->lifetime_min;
}

void jce_scene_particle_emitter_desc(const JceParticleEmitterComponent *c,
                                     JceParticleEmitterDesc *out)
{
    jce_scene_particle_emitter_desc_tex(c, out, NULL, 0);
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
    /* emitter_add deep-copied any loader-owned sub-emitter child synchronously;
     * release the heap child desc now so it does not leak (no-op when none). */
    jce_particles_desc_free(&desc);
    if (!jce_emitter_valid(h)) {
        LOG_WARN(LOG_TAG, "emitter pool full; particle component skipped");
        return;
    }
    c->emitter_handle_idx = h.idx;
    c->loaded             = true;
    c->asset_epoch        = jce_scene_particle_emitter_epoch(c);
    /* Honour a stop that arrived before this emitter existed (see
     * emit_suppressed): otherwise a lazily-built emitter ignores the script. */
    if (c->emit_suppressed) jce_particles_emitter_stop(ctx->sys, h);
    else                    jce_particles_emitter_start(ctx->sys, h);
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
void jce_scene_particles_update(JceScene *s, float dt)
{
    if (!s) return;

    JceParticleSystem *sys =
        (JceParticleSystem *)jce_scene_internal_particles_get(s);

    if (!sys) {
        /* O(1) holder count instead of a full-entity probe walk: on a
         * particle-free 150k-entity world the old walk burned a full scan
         * EVERY frame just to conclude "nothing to do". */
        if (jce_scene_count_particle_emitters(s) == 0)
            return;         /* nothing to do; stay allocation-free */
        sys = jce_particles_create(jce_allocator_default());
        if (!sys) return;
        jce_scene_internal_particles_set(s, sys);
    }

    SpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scene = s;
    ctx.sys   = sys;

    /* Per-emitter: build/sync/draw, marking referenced emitters.
     * Component-filtered walk (O(#emitters), not O(#entities)). */
    jce_scene_each_particle_emitter(s, sp_each, &ctx);

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

/* ── Per-entity control (scripting last-mile) ─────────────────────────────
 *
 * Resolve entity -> JceParticleEmitterComponent -> the live emitter handle in
 * the scene-owned JceParticleSystem.  Tolerant: a no-op when the scene has no
 * particle system yet (built lazily on the first jce_scene_particles_update
 * tick), the entity has no emitter component, or the component is GPU-routed /
 * not yet loaded (emitter_handle_idx == UINT32_MAX). */
static JceParticleSystem *sp_resolve(JceScene *s, JceEntity e,
                                     JceEmitterHandle *out)
{
    if (!s) return NULL;
    JceParticleSystem *sys =
        (JceParticleSystem *)jce_scene_internal_particles_get(s);
    if (!sys) return NULL;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c || !c->loaded || c->emitter_handle_idx == UINT32_MAX) return NULL;
    JceEmitterHandle h = { c->emitter_handle_idx };
    if (!jce_particles_emitter_is_alive(sys, h)) return NULL;
    *out = h;
    return sys;
}

void jce_scene_particle_burst(JceScene *s, JceEntity e, int count)
{
    if (count <= 0) return;
    JceEmitterHandle h;
    JceParticleSystem *sys = sp_resolve(s, e, &h);
    if (!sys) return;
    /* Sync the emitter origin to the entity's CURRENT world position before
     * bursting.  A script that does jce.set_position + jce.particle_burst in
     * the same tick must spawn AT the just-set position: the per-tick origin
     * sync in sp_each runs only during jce_scene_particles_update, AFTER the
     * script tick, so without this the burst fires from the PREVIOUS sync's
     * origin (e.g. lightning bursts landing at the prior strike point). */
    jce_mat4 w = jce_scene_get_world_matrix(s, e);
    jce_particles_emitter_set_position(
        sys, h, jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]));
    jce_particles_emitter_burst(sys, h, (uint32_t)count);
}

void jce_scene_particle_set_emitting(JceScene *s, JceEntity e, bool on)
{
    /* Record the request first: the emitter may not have been built yet, and
     * sp_emitter_build applies emit_suppressed when it eventually is. */
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (c) c->emit_suppressed = !on;

    JceEmitterHandle h;
    JceParticleSystem *sys = sp_resolve(s, e, &h);
    if (!sys) return;
    if (on) jce_particles_emitter_start(sys, h);
    else    jce_particles_emitter_stop(sys, h);
}

void jce_scene_particle_set_color(JceScene *s, JceEntity e,
                                  float r, float g, float b)
{
    JceEmitterHandle h;
    JceParticleSystem *sys = sp_resolve(s, e, &h);
    if (!sys) return;
    jce_vec3 rgb = { r, g, b };
    jce_particles_emitter_set_color(sys, h, rgb);
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
