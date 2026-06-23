/*
 * jce_sr_particles.c  Scene-renderer particle module (split from
 * jce_scene_renderer.c).
 *
 * CPU particle visualisation (debug-cross billboards) + compute-driven GPU
 * particles.  Pure move from the monolithic renderer; the two driver entry
 * points are declared in jce_sr_internal.h, everything else stays file-static.
 */

#include "jce_sr_internal.h"

/* ── Particle visualisation (P2-particle-vfx-runtime) ─────────────────
 *
 * The CPU particle backend (jce_particles.c) has no dedicated GPU
 * billboard pass yet, so alive particles from the scene-owned
 * JceParticleSystem are submitted through the debug-line pipeline as small
 * axis crosses (camera-agnostic, depth-tested) and flushed with the
 * renderer's color program.  This is the single render path used by both
 * the editor and the shipping runtime, so it owns the flush — no reliance
 * on an external debug-draw flush, and the buffer is always cleared. */
static void sr_particle_visit(const JceParticleView *p, void *ud)
{
    (void)ud;
    float r = p->size * 0.5f;
    if (r < 0.02f) r = 0.02f;

    int rr = (int)(p->color.x * 255.0f); rr = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
    int gg = (int)(p->color.y * 255.0f); gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
    int bb = (int)(p->color.z * 255.0f); bb = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
    int aa = (int)(p->color.w * 255.0f); aa = aa < 0 ? 0 : (aa > 255 ? 255 : aa);
    uint32_t abgr = ((uint32_t)aa << 24) | ((uint32_t)bb << 16) |
                    ((uint32_t)gg << 8)  |  (uint32_t)rr;

    jce_vec3 c = p->position;
    jce_debug_draw_line(jce_v3(c.x - r, c.y, c.z), jce_v3(c.x + r, c.y, c.z), abgr);
    jce_debug_draw_line(jce_v3(c.x, c.y - r, c.z), jce_v3(c.x, c.y + r, c.z), abgr);
    jce_debug_draw_line(jce_v3(c.x, c.y, c.z - r), jce_v3(c.x, c.y, c.z + r), abgr);
}

typedef struct {
    const JceParticleSystem *sys;
    JceScene                *scene;
} SrParticleEachCtx;

static void sr_particle_each_entity(JceScene *s, JceEntity e, void *ud)
{
    SrParticleEachCtx *ctx = (SrParticleEachCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c || !c->loaded || c->emitter_handle_idx == UINT32_MAX) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;
    /* GPU-routed emitters render through sr_drive_gpu_particles (instanced
     * billboards), not the CPU debug-cross path. */
    if (jce_scene_particle_emitter_uses_gpu(c)) return;
    JceEmitterHandle h = { c->emitter_handle_idx };
    jce_particles_emitter_for_each(ctx->sys, h, sr_particle_visit, NULL);
}

void sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                       uint16_t view_id)
{
    const JceParticleSystem *sys =
        (const JceParticleSystem *)jce_scene_internal_particles_get(scene);
    if (!sys || jce_particles_alive_count(sys) == 0) return;

    SrParticleEachCtx ctx = { sys, scene };
    jce_scene_each_entity(scene, sr_particle_each_entity, &ctx);
    jce_debug_draw_flush(view_id, sr->renderer);
}

/* ── GPU particles (compute-driven; P3-E wiring) ──────────────────────
 *
 * Drives one JceGpuParticleSystem per GPU-flagged JceParticleEmitterComponent
 * (routing predicate shared with jce_scene_particles.c, which suppresses the
 * CPU emitter for the same component).  Per frame and per emitter: one
 * simulate+emit dispatch on the dedicated compute view (base+9, ordered
 * before the color view) and one instanced billboard draw into the color
 * view at the same transparency position as the CPU debug draw.
 *
 * KNOWN semantic divergences from the CPU path (by design of the compute
 * backend): always world-space, procedural soft-circle sprite (the authored
 * texture and world_space=false are ignored), additive blend. */

typedef struct {
    JceSceneRenderer *sr;
    uint16_t          compute_view;  /* base+9: dispatches only            */
    uint16_t          color_view;    /* base+0: instanced billboard draw   */
    float             dt;
    bool              dispatch;      /* false on 2nd+ render of a bgfx frame */
} SrGpuParticleCtx;

static SrGpuParticleRec *sr_gpu_particle_find_or_add(JceSceneRenderer *sr,
                                                     JceEntity e)
{
    SrGpuParticleRec *free_rec = NULL;
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++) {
        SrGpuParticleRec *r = &sr->gpu_particles[i];
        if (r->used && r->entity == e) return r;
        if (!r->used && !free_rec) free_rec = r;
    }
    if (free_rec) {
        memset(free_rec, 0, sizeof(*free_rec));
        free_rec->entity = e;
        free_rec->used   = true;
    }
    return free_rec;   /* NULL when the table is full (emitter skipped) */
}

static void sr_gpu_particle_each(JceScene *s, JceEntity e, void *ud)
{
    SrGpuParticleCtx *ctx = (SrGpuParticleCtx *)ud;
    JceSceneRenderer *sr  = ctx->sr;

    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;
    if (!jce_scene_particle_emitter_uses_gpu(c)) return;

    SrGpuParticleRec *rec = sr_gpu_particle_find_or_add(sr, e);
    if (!rec) return;

    /* Authoring edits (asset path / quick-tune / gpu flag) rebuild the pool
     * so a changed max_particles takes effect. */
    uint64_t epoch = jce_scene_particle_emitter_epoch(c);
    if (rec->sys && rec->epoch != epoch) {
        jce_gpu_particles_destroy(rec->sys);
        rec->sys        = NULL;
        rec->emit_accum = 0.0f;
        rec->burst_done = false;
    }

    if (!rec->sys) {
        jce_scene_particle_emitter_desc(c, &rec->desc);
        /* GPU path consumes only scalar fields; drop any loader-owned CPU
         * sub-emitter child so rec->desc never holds a dangling/leaked ptr. */
        jce_particles_desc_free(&rec->desc);
        JceGpuParticleSystemDesc d;
        d.max_particles = rec->desc.max_particles;
        d.pak           = sr->pak;
        rec->sys = jce_gpu_particles_create(&d, jce_allocator_default());
        if (!rec->sys || !jce_gpu_particles_is_supported(rec->sys)) {
            /* Caps lied or shaders missing: latch CPU-forever (logs once)
             * and release the slot — sp_each re-grows a CPU emitter next
             * scene tick because the predicate now fails. */
            if (rec->sys) jce_gpu_particles_destroy(rec->sys);
            memset(rec, 0, sizeof(*rec));
            jce_scene_internal_gpu_particles_set_blocked();
            return;
        }
        rec->epoch = epoch;
    }

    rec->referenced = true;

    if (ctx->dispatch) {
        const JceParticleEmitterDesc *d = &rec->desc;

        /* Rate emission with fractional carry (+ the one-shot burst). */
        rec->emit_accum += d->emit_rate * ctx->dt;
        uint32_t n = (uint32_t)rec->emit_accum;
        rec->emit_accum -= (float)n;
        if (!rec->burst_done) {
            if (d->emit_burst > 0.0f) n += (uint32_t)(d->emit_burst + 0.5f);
            rec->burst_done = true;
        }

        jce_mat4 w = jce_scene_get_world_matrix(s, e);

        JceGpuParticleEmitConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.emit_count   = n;
        cfg.origin       = jce_v3(w.raw[3][0], w.raw[3][1], w.raw[3][2]);
        cfg.velocity_min = d->velocity_min;
        cfg.velocity_max = d->velocity_max;
        cfg.lifetime_min = d->lifetime_min;
        cfg.lifetime_max = d->lifetime_max;
        cfg.size_start   = d->size_start;
        cfg.size_end     = d->size_end;
        cfg.color_start  = d->color_start;
        cfg.color_end    = d->color_end;
        cfg.gravity      = d->gravity;
        cfg.damping      = 0.0f;   /* no CPU-desc counterpart */

        jce_gpu_particles_update(rec->sys, ctx->compute_view, ctx->dt, &cfg);
    }

    jce_gpu_particles_render(rec->sys, ctx->color_view);
}

void sr_drive_gpu_particles(JceSceneRenderer *sr, JceScene *scene,
                            uint16_t view_id_base, float dt_sec)
{
    if (!sr || !scene) return;

    /* Un-mark first so the sweep also runs when the feature toggles off
     * (deferred pipeline toggle frees every pool next frame). */
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++)
        sr->gpu_particles[i].referenced = false;

    if (sr->pak && jce_render_pipeline_is_feature_enabled("gpu_particles")) {
        /* Multi-viewport guard: the editor renders the scene more than once
         * per bgfx frame (scene view + game view); simulate/emit only on the
         * first render so dt is not applied twice. */
        uint32_t fi = jce_renderer_get_frame_index(sr->renderer);
        bool dispatch = !(sr->gpu_particle_frame_valid &&
                          sr->gpu_particle_frame == fi);

        if (dt_sec > 0.1f) dt_sec = 0.1f;   /* clamp long frames (CPU parity) */

        SrGpuParticleCtx ctx;
        ctx.sr           = sr;
        ctx.compute_view = (uint16_t)(view_id_base + JCE_VIEW_GPU_PARTICLE_OFFSET);
        ctx.color_view   = view_id_base;
        ctx.dt           = dt_sec;
        ctx.dispatch     = dispatch;
        jce_scene_each_entity(scene, sr_gpu_particle_each, &ctx);

        if (dispatch) {
            sr->gpu_particle_frame       = fi;
            sr->gpu_particle_frame_valid = true;
        }
    }

    /* Sweep: records whose entity vanished, toggled back to CPU, or whose
     * feature flag turned off are destroyed here. */
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++) {
        SrGpuParticleRec *r = &sr->gpu_particles[i];
        if (!r->used || r->referenced) continue;
        if (r->sys) jce_gpu_particles_destroy(r->sys);
        memset(r, 0, sizeof(*r));
    }
}
