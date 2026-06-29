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
 * Alive particles from the scene-owned JceParticleSystem are drawn as
 * camera-facing colour QUADS (batched into one transient buffer + the color
 * program, alpha-blended, depth-tested but no Z-write) — proper billboards,
 * not the old axis-cross debug lines.  Honours per-particle size + colour.
 * (Texture / flipbook UVs + soft-circle falloff need an unlit-textured shader
 * — a follow-up; this path is solid-colour.)  Single render path for the editor
 * + shipping runtime. */
typedef struct {
    struct SrPVtx { float x, y, z; uint32_t abgr; } *v;
    uint16_t *idx;
    uint32_t  cap;   /* max quads the transient buffer holds */
    uint32_t  n;     /* quads written so far */
    jce_vec3  right; /* camera basis, unit */
    jce_vec3  up;
} SrParticleBatch;

static void sr_particle_visit(const JceParticleView *p, void *ud)
{
    SrParticleBatch *b = (SrParticleBatch *)ud;
    if (b->n >= b->cap) return;

    float hr = p->size * 0.5f;
    if (hr < 0.02f) hr = 0.02f;
    jce_vec3 rx = jce_v3_scale(b->right, hr);
    jce_vec3 uy = jce_v3_scale(b->up,    hr);

    int rr = (int)(p->color.x * 255.0f); rr = rr < 0 ? 0 : (rr > 255 ? 255 : rr);
    int gg = (int)(p->color.y * 255.0f); gg = gg < 0 ? 0 : (gg > 255 ? 255 : gg);
    int bb = (int)(p->color.z * 255.0f); bb = bb < 0 ? 0 : (bb > 255 ? 255 : bb);
    int aa = (int)(p->color.w * 255.0f); aa = aa < 0 ? 0 : (aa > 255 ? 255 : aa);
    uint32_t abgr = ((uint32_t)aa << 24) | ((uint32_t)bb << 16) |
                    ((uint32_t)gg << 8)  |  (uint32_t)rr;

    jce_vec3 c  = p->position;
    jce_vec3 p0 = jce_v3_sub(jce_v3_sub(c, rx), uy);
    jce_vec3 p1 = jce_v3_sub(jce_v3_add(c, rx), uy);
    jce_vec3 p2 = jce_v3_add(jce_v3_add(c, rx), uy);
    jce_vec3 p3 = jce_v3_add(jce_v3_sub(c, rx), uy);

    uint32_t vb = b->n * 4u, ib = b->n * 6u;
    b->v[vb+0].x=p0.x; b->v[vb+0].y=p0.y; b->v[vb+0].z=p0.z; b->v[vb+0].abgr=abgr;
    b->v[vb+1].x=p1.x; b->v[vb+1].y=p1.y; b->v[vb+1].z=p1.z; b->v[vb+1].abgr=abgr;
    b->v[vb+2].x=p2.x; b->v[vb+2].y=p2.y; b->v[vb+2].z=p2.z; b->v[vb+2].abgr=abgr;
    b->v[vb+3].x=p3.x; b->v[vb+3].y=p3.y; b->v[vb+3].z=p3.z; b->v[vb+3].abgr=abgr;
    b->idx[ib+0]=(uint16_t)vb;     b->idx[ib+1]=(uint16_t)(vb+1); b->idx[ib+2]=(uint16_t)(vb+2);
    b->idx[ib+3]=(uint16_t)vb;     b->idx[ib+4]=(uint16_t)(vb+2); b->idx[ib+5]=(uint16_t)(vb+3);
    b->n++;
}

typedef struct {
    const JceParticleSystem *sys;
    JceScene                *scene;
    SrParticleBatch         *batch;
} SrParticleEachCtx;

static void sr_particle_each_entity(JceScene *s, JceEntity e, void *ud)
{
    SrParticleEachCtx *ctx = (SrParticleEachCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c || !c->loaded || c->emitter_handle_idx == UINT32_MAX) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;
    /* GPU-routed emitters render through sr_drive_gpu_particles (instanced
     * billboards), not this CPU path. */
    if (jce_scene_particle_emitter_uses_gpu(c)) return;
    JceEmitterHandle h = { c->emitter_handle_idx };
    jce_particles_emitter_for_each(ctx->sys, h, sr_particle_visit, ctx->batch);
}

void sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                       const JceCamera *camera, uint16_t view_id)
{
    const JceParticleSystem *sys =
        (const JceParticleSystem *)jce_scene_internal_particles_get(scene);
    if (!sys) return;
    uint32_t alive = jce_particles_alive_count(sys);
    if (alive == 0) return;

    bgfx_vertex_layout_t layout;
    bgfx_vertex_layout_begin(&layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_POSITION, 3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&layout, BGFX_ATTRIB_COLOR0,   4, BGFX_ATTRIB_TYPE_UINT8, true, false);
    bgfx_vertex_layout_end(&layout);

    /* Cap to what the transient ring can serve this frame (whole quads). */
    uint32_t quads = alive;
    uint32_t av = bgfx_get_avail_transient_vertex_buffer(quads * 4u, &layout) / 4u;
    uint32_t ai = bgfx_get_avail_transient_index_buffer(quads * 6u, false) / 6u;
    if (av < quads) quads = av;
    if (ai < quads) quads = ai;
    if (quads == 0) return;

    bgfx_transient_vertex_buffer_t tvb;
    bgfx_transient_index_buffer_t  tib;
    if (!bgfx_alloc_transient_buffers(&tvb, &layout, quads * 4u, &tib, quads * 6u, false))
        return;

    SrParticleBatch batch;
    batch.v     = (struct SrPVtx *)tvb.data;
    batch.idx   = (uint16_t *)tib.data;
    batch.cap   = quads;
    batch.n     = 0;
    batch.right = camera ? jce_camera_get_right(camera) : jce_v3(1.0f, 0.0f, 0.0f);
    batch.up    = camera ? jce_camera_get_up(camera)    : jce_v3(0.0f, 1.0f, 0.0f);

    SrParticleEachCtx ctx = { sys, scene, &batch };
    jce_scene_each_entity(scene, sr_particle_each_entity, &ctx);
    if (batch.n == 0) return;

    bgfx_set_transient_vertex_buffer(0, &tvb, 0, batch.n * 4u);
    bgfx_set_transient_index_buffer(&tib, 0, batch.n * 6u);
    jce_mat4 ident = jce_m4_identity();
    bgfx_set_transform(ident.raw[0], 1);
    /* Unlit, alpha-blended, depth-tested, no Z-write (particles don't occlude). */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA, 0);
    JceShaderHandle sh = jce_renderer_get_program_color(sr->renderer);
    bgfx_program_handle_t prog;
    prog.idx = sh.idx;
    if (BGFX_HANDLE_IS_VALID(prog))
        bgfx_submit(view_id, prog, 0, BGFX_DISCARD_ALL);
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
