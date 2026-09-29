/*
 * jce_sr_particles.c  Scene-renderer particle module (split from
 * jce_scene_renderer.c).
 *
 * CPU particle visualisation (debug-cross billboards) + compute-driven GPU
 * particles.  Pure move from the monolithic renderer; the two driver entry
 * points are declared in jce_sr_internal.h, everything else stays file-static.
 */

#include "jce_sr_internal.h"

/* ── CPU particle billboard sprite (shares the GPU pool's shaders) ─────
 *
 * ALL particle emitters render through this CPU path.  The GPU compute path
 * (sr_drive_gpu_particles) is unreliable on Vulkan/D3D12 (bgfx compute-buffer
 * barrier — an over-bright blob), so CPU is the one backend-consistent path.
 * Alive particles from the scene-owned JceParticleSystem are packed into a
 * transient INSTANCE buffer (4x vec4 per particle — the exact layout the GPU
 * pool / cs_particle_update uses) and submitted with the SAME
 * vs_particle+fs_particle program, so the on-screen result is IDENTICAL on
 * every backend: a view-aligned soft-circle sprite (billboarded in the vertex
 * shader off u_invView, additive glow).  Single render path for the editor +
 * shipping runtime.  (Set JCE_GPU_PARTICLES_FORCE=1 to route to the GPU pool.) */

/* One instance = 4 vec4 (16 floats): i_data0=(pos,age) i_data2=colour
 * i_data3=(size,..); i_data1 unused (vel/life).  Matches vs_particle. */
#define SR_PARTICLE_INST_STRIDE 64u

typedef struct {
    float   *data;   /* transient instance buffer (16 floats per particle) */
    uint32_t count;
    uint32_t cap;
} SrParticleBatch;

static void sr_particle_visit(const JceParticleView *p, void *ud)
{
    SrParticleBatch *b = (SrParticleBatch *)ud;
    if (b->count >= b->cap) return;
    float *d = b->data + (size_t)b->count * 16u;
    d[0]  = p->position.x; d[1] = p->position.y; d[2] = p->position.z; d[3] = 0.0f;
    /* i_data1.xyz = velocity * stretch (world) → vs_particle elongates the
     * billboard along it (rain/spark streaks).  stretch == 0 (the default for
     * every non-streak emitter) leaves this at zero, so the shader keeps the
     * classic round view-aligned sprite. */
    d[4]  = p->velocity.x * p->stretch;
    d[5]  = p->velocity.y * p->stretch;
    d[6]  = p->velocity.z * p->stretch;
    d[7]  = 0.0f;
    d[8]  = p->color.x; d[9] = p->color.y; d[10] = p->color.z; d[11] = p->color.w;
    float sz = p->size; if (sz < 0.0001f) sz = 0.0001f;
    d[12] = sz; d[13] = 0.0f; d[14] = 0.0f; d[15] = 0.0f;
    b->count++;
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
    /* GPU-forced emitters (JCE_GPU_PARTICLES_FORCE=1) draw on the GPU pool. */
    if (jce_scene_particle_emitter_uses_gpu(c)) return;
    JceEmitterHandle h = { c->emitter_handle_idx };
    jce_particles_emitter_for_each(ctx->sys, h, sr_particle_visit, ctx->batch);
}

/* Lazily build the sprite program (vs_particle+fs_particle), the shared unit
 * quad and the textured-flag uniform — mirrors jce_gpu_particles' render
 * resources so the CPU and GPU paths draw byte-identically. */
static void sr_particle_sprite_lazy_init(JceSceneRenderer *sr)
{
    if (sr->particle_sprite_tried) return;
    sr->particle_sprite_tried    = true;
    sr->prog_particle_sprite.idx = UINT16_MAX;

    JceShaderHandle h = shader_load_program(sr->pak, "particle");
    sr->prog_particle_sprite.idx = h.idx;
    if (h.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "particle sprite shader (vs/fs_particle) not in PAK; "
                          "CPU particles will not render");
        return;
    }

    bgfx_vertex_layout_t ql;
    bgfx_vertex_layout_begin(&ql, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&ql, BGFX_ATTRIB_POSITION,  3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&ql, BGFX_ATTRIB_TEXCOORD0, 2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&ql);
    static const float qv[] = {
        -0.5f,-0.5f, 0.0f,  0.0f, 0.0f,
         0.5f,-0.5f, 0.0f,  1.0f, 0.0f,
         0.5f, 0.5f, 0.0f,  1.0f, 1.0f,
        -0.5f, 0.5f, 0.0f,  0.0f, 1.0f,
    };
    static const uint16_t qi[] = { 0, 1, 2, 0, 2, 3 };
    sr->particle_quad_vb = bgfx_create_vertex_buffer(
        bgfx_copy(qv, sizeof qv), &ql, BGFX_BUFFER_NONE);
    sr->particle_quad_ib = bgfx_create_index_buffer(
        bgfx_copy(qi, sizeof qi), BGFX_BUFFER_NONE);
    sr->u_particle_misc = bgfx_create_uniform("u_particle_misc",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->u_particle_soft = bgfx_create_uniform("u_particle_soft",
                                              BGFX_UNIFORM_TYPE_VEC4, 1);
    sr->s_particle_scene_depth = bgfx_create_uniform("s_sceneDepth",
                                              BGFX_UNIFORM_TYPE_SAMPLER, 1);
}

/* The frame's soft-particle block, or "off" when this frame has no depth.
 *
 * ONE builder for BOTH paths.  The CPU emitter walk and the compute pool
 * submit the same fs_particle program, so anything that differs between them
 * is a fade that works for some emitters and not others -- and the GPU path
 * is the one an author cannot tell apart by looking.
 *
 * Fails to OFF, deliberately: no pre-pass this frame, or a viewport of zero,
 * and the shader skips its soft block entirely.  Particles then look exactly
 * as they did before soft particles existed, which is the right failure --
 * the alternative (sampling an unbound or stale depth target) reads as
 * particles vanishing. */
static JceGpuParticleSoft sr_particle_soft_frame(const JceSceneRenderer *sr,
                                                 const JceCamera *camera)
{
    JceGpuParticleSoft soft;
    soft.depth_texture     = JCE_INVALID_TEXTURE;
    soft.fade_distance     = 0.0f;
    soft.inv_viewport_w    = 0.0f;
    soft.inv_viewport_h    = 0.0f;
    soft.near_z            = 0.1f;
    soft.far_z             = 1000.0f;

    const float fade = jce_particles_get_soft_fade_distance();
    if (fade <= 0.0f || !sr || !camera) return soft;
    if (!sr->depth_prepass_frame || !BGFX_HANDLE_IS_VALID(sr->ssao_depth_tex))
        return soft;
    if (sr->ssao_w == 0 || sr->ssao_h == 0) return soft;

    soft.depth_texture.idx = sr->ssao_depth_tex.idx;
    soft.fade_distance     = fade;
    soft.inv_viewport_w    = 1.0f / (float)sr->ssao_w;
    soft.inv_viewport_h    = 1.0f / (float)sr->ssao_h;
    soft.near_z            = jce_camera_get_near(camera);
    soft.far_z             = jce_camera_get_far(camera);
    return soft;
}

void sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                       const JceCamera *camera, uint16_t view_id)
{
    /* camera was unused (the billboard basis is u_invView, set by the view
     * transform); the soft fade needs its near/far to linearise depth. */
    const JceParticleSystem *sys =
        (const JceParticleSystem *)jce_scene_internal_particles_get(scene);
    if (!sys) return;
    uint32_t alive = jce_particles_alive_count(sys);
    if (alive == 0) return;

    sr_particle_sprite_lazy_init(sr);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_particle_sprite)) return;

    const JceGpuParticleSoft soft = sr_particle_soft_frame(sr, camera);

    uint32_t avail = bgfx_get_avail_instance_data_buffer(
        alive, (uint16_t)SR_PARTICLE_INST_STRIDE);
    uint32_t n = alive < avail ? alive : avail;
    if (n == 0) return;

    bgfx_instance_data_buffer_t idb;
    bgfx_alloc_instance_data_buffer(&idb, n, (uint16_t)SR_PARTICLE_INST_STRIDE);

    SrParticleBatch batch = { (float *)idb.data, 0u, n };
    SrParticleEachCtx ctx = { sys, scene, &batch };
    /* Component-filtered walk (O(#emitters)); O(1) all-clear gate. */
    if (jce_scene_count_particle_emitters(scene) > 0)
        jce_scene_each_particle_emitter(scene, sr_particle_each_entity, &ctx);
    if (batch.count == 0) return;

    bgfx_set_vertex_buffer(0, sr->particle_quad_vb, 0, 4);
    bgfx_set_index_buffer(sr->particle_quad_ib, 0, 6);
    bgfx_set_instance_data_buffer(&idb, 0, batch.count);

    /* .x=0 → procedural soft circle.  .y=1 → allow motion-stretch: vs_particle
     * elongates only the instances whose baked i_data1 (velocity*stretch) is
     * non-zero, so round emitters are unaffected while rain reads as streaks.
     * The GPU pool draw leaves .y=0, so its i_data1 (raw vel/life) never
     * accidentally stretches. */
    /* .z = soft-particle fade distance; 0 makes the shader skip the block, so
     * sampler stage 1 stays unbound and unread. */
    float misc[4] = { 0.0f, 1.0f, soft.fade_distance, 0.0f };
    bgfx_set_uniform(sr->u_particle_misc, misc, 1);
    if (soft.fade_distance > 0.0f) {
        float sp[4] = { soft.inv_viewport_w, soft.inv_viewport_h,
                        soft.near_z, soft.far_z };
        bgfx_set_uniform(sr->u_particle_soft, sp, 1);
        bgfx_texture_handle_t dt = { soft.depth_texture.idx };
        bgfx_set_texture(1, sr->s_particle_scene_depth, dt, UINT32_MAX);
    }

    /* Match the GPU pool draw: additive soft-glow, depth-tested, no Z-write,
     * cull-CW (the billboard winds the same as the GPU pool's quad). */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                  | BGFX_STATE_DEPTH_TEST_LESS
                  | BGFX_STATE_CULL_CW
                  | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                          BGFX_STATE_BLEND_ONE), 0);
    bgfx_submit(view_id, sr->prog_particle_sprite, 0, BGFX_DISCARD_ALL);
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
    int              *creates_left;  /* per-frame create budget (stagger)  */
    JceGpuParticleSoft soft;         /* depth fade, or off (fade == 0)     */
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
        /* Stagger creation: building a GPU particle system allocates a pool
         * VB + 4 programs (8 shaders) + uniforms and issues its first
         * compute dispatches.  A scene switch that brings up MANY emitters in
         * ONE frame (elemental_serenity = 8) spikes D3D12 resource creation
         * hard enough that a later graphics PSO create fails ("Failed to
         * create PSO!", device-removed) and the render thread crashes.  Cap
         * new systems per frame; the rest are created over the next frames
         * (a few frames' delay before those emitters appear — imperceptible). */
        if (ctx->creates_left && *ctx->creates_left <= 0) {
            rec->referenced = true;   /* keep the slot reserved for next frame */
            return;
        }
        if (ctx->creates_left) (*ctx->creates_left)--;
        jce_scene_particle_emitter_desc_tex(c, &rec->desc,
                                            rec->tex_path, sizeof rec->tex_path);
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
        /* Zero-fill the pool NOW, regardless of the dispatch gate: a system
         * created on a non-dispatch pass (scene switch lands mid-frame, the
         * second viewport creates it) would otherwise render an uninitialized
         * pool — garbage instances on VK, NaN geometry that TDRs the device
         * on D3D12 (the scene-switch crash chain). */
        jce_gpu_particles_reset(rec->sys, ctx->compute_view);
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

    /* Authored billboard texture resolves through the renderer's async
     * texture cache (INVALID while decoding — the draw falls back to the
     * procedural sprite for those first frames, then upgrades). */
    uint16_t tex_idx = UINT16_MAX;
    if (rec->tex_path[0]) {
        JceTexture t = sr_resolve_texture(sr, rec->tex_path);
        tex_idx = t.idx;
    }
    JceTextureHandle tex_h = { tex_idx };
    jce_gpu_particles_render_soft(rec->sys, ctx->color_view,
                                  tex_h, rec->desc.blend_alpha, &ctx->soft);
}

void sr_drive_gpu_particles(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera,
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
        ctx.soft         = sr_particle_soft_frame(sr, camera);
        /* At most 2 NEW GPU particle systems created per bgfx frame (see the
         * stagger note in sr_gpu_particle_each) — only meter on the dispatch
         * pass so the two viewports don't double-count.  Steady state (no new
         * emitters) never touches this. */
        int creates_left = dispatch ? 2 : 0;
        ctx.creates_left = &creates_left;
        /* Component-filtered walk (O(#emitters)); O(1) all-clear gate. */
        if (jce_scene_count_particle_emitters(scene) > 0)
            jce_scene_each_particle_emitter(scene, sr_gpu_particle_each, &ctx);

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
