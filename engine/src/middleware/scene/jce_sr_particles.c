/*
 * Scene particle rendering shared by the editor and shipping runtime.
 * CPU simulation draws instanced billboards when supported, or expands
 * ordinary indexed quads into transient buffers on older devices.
 * The opt-in GPU simulation shares the instanced billboard shaders.
 */

#include "jce_sr_internal.h"
#include "renderer/jce_gpu_particles_internal.h"
#include "jce_sr_debug_targets.h"
#include "jce_sr_snow_mesh_cover.h"
#include "jce_sr_rain_surface.h"

typedef struct {
    JceSceneRenderer *sr;
    JceParticleCover cover;
    bool collected;
} SrPrecipitationCover;

/* Pack once per pass, only for an opted-in emitter. Geometry is borrowed
 * only while packing, so edits and residency changes take effect next draw. */
static bool sr_particle_is_precipitation(JceScene *scene, JceEntity emitter)
{
    return strcmp(jce_scene_get_entity_tag_name(scene, emitter), "Precipitation") == 0;
}

static const JceParticleCover *sr_particle_precipitation_cover(
    SrPrecipitationCover *state, JceScene *scene)
{
    JceParticleCover *cover = &state->cover;
    if (!state->collected) {
        state->collected = true;
        JceEntity roofs[JCE_PARTICLE_COVER_MAX];
        const int count = jce_scene_find_all_with_tag(scene, "SnowOccluder", roofs,
                                                       JCE_PARTICLE_COVER_MAX);
        for (int i = 0; i < count && i < JCE_PARTICLE_COVER_MAX &&
                        cover->count < JCE_PARTICLE_COVER_MAX; ++i) {
            if (strcmp(jce_scene_get_entity_tag_name(scene, roofs[i]), "SnowOccluder") != 0)
                continue;
            const jce_mat4 world = jce_scene_get_world_matrix(scene, roofs[i]);
            cover->count += sr_snow_mesh_cover(state->sr, scene, roofs[i], &world,
                &cover->triangles[cover->count], JCE_PARTICLE_COVER_MAX - cover->count);
        }
        cover->triangles[0][0][3] = (float)cover->count;
    }
    return cover->count ? cover : NULL;
}

/* Follow the instantaneous direction back toward the sky. In the roof's
 * XZ barycentric basis, solve t = roofY(xz + direction*t) - particleY. */
static bool sr_particle_roof_blocks(const JceParticleCover *cover,
                                    jce_vec3 pos, jce_vec3 velocity)
{
    if (!(velocity.y < -0.05f)) return false;
    const float dx = velocity.x / velocity.y;
    const float dz = velocity.z / velocity.y;
    for (uint16_t i = 0; i < cover->count; ++i) {
        const float (*triangle)[4] = cover->triangles[i];
        const float *origin = triangle[0];
        const float *basis = triangle[1];
        const float *slope = triangle[2];
        const float x = pos.x - origin[0], z = pos.z - origin[2];
        const float u = basis[0]*x + basis[1]*z;
        const float v = basis[2]*x + basis[3]*z;
        const float du = basis[0]*dx + basis[1]*dz;
        const float dv = basis[2]*dx + basis[3]*dz;
        const float rise = 1.0f - (du*slope[0] + dv*slope[1]);
        if (!(rise > 0.0001f)) continue;
        const float lift = (origin[1] + (u*slope[0] + v*slope[1]) - pos.y) / rise;
        if (!(lift > 0.08f && lift <= 24.0f)) continue;
        const float hit_u = u + du*lift, hit_v = v + dv*lift;
        if (hit_u >= -0.00002f && hit_v >= -0.00002f && hit_u + hit_v <= 1.00002f)
            return true;
    }
    return false;
}

/* The 16-float CPU instance matches the compute pool's vertex layout. */
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
    JceParticleVisitFn       visit;
    void                    *batch;
    SrPrecipitationCover    *precipitation;
    const JceParticleCover  *cover;
} SrParticleEachCtx;

static void sr_particle_visit_under_cover(const JceParticleView *p, void *ud)
{
    SrParticleEachCtx *ctx = (SrParticleEachCtx *)ud;
    if (!sr_particle_roof_blocks(ctx->cover, p->position, p->velocity))
        ctx->visit(p, ctx->batch);
}

static void sr_particle_each_entity(JceScene *s, JceEntity e, void *ud)
{
    SrParticleEachCtx *ctx = (SrParticleEachCtx *)ud;
    JceParticleEmitterComponent *c = jce_scene_get_particle_emitter(s, e);
    if (!c || !c->loaded || c->emitter_handle_idx == UINT32_MAX) return;
    if (!jce_scene_component_enabled(s, e, JCE_COMP_FLAG_PARTICLE_EMITTER)) return;
    /* GPU-forced emitters (JCE_GPU_PARTICLES_FORCE=1) draw on the GPU pool. */
    if (jce_scene_particle_emitter_uses_gpu(s, c)) return;
    JceEmitterHandle h = { c->emitter_handle_idx };
    ctx->cover = sr_particle_is_precipitation(s, e) ?
        sr_particle_precipitation_cover(ctx->precipitation, s) : NULL;
    if (ctx->cover)
        jce_particles_emitter_for_each(ctx->sys, h, sr_particle_visit_under_cover, ctx);
    else
        jce_particles_emitter_for_each(ctx->sys, h, ctx->visit, ctx->batch);
}

static void sr_particle_quad_init(JceSceneRenderer *sr)
{
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
}

/* The device caps and program layout stay paired for this renderer lifetime. */
static void sr_particle_sprite_lazy_init(JceSceneRenderer *sr, bool instanced)
{
    if (sr->particle_sprite_tried) return;
    sr->particle_sprite_tried = true;

    JceShaderHandle h = instanced
        ? shader_load_program(sr->pak, "particle")
        : shader_load_program_named(sr->pak, "particle_expanded", "particle");
    sr->prog_particle_sprite.idx = h.idx;
    if (h.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "%s particle sprite shader unavailable",
                 instanced ? "instanced" : "expanded");
        return;
    }

    if (instanced) sr_particle_quad_init(sr);
    else LOG_INFO(LOG_TAG, "%s", "CPU particles use expanded transient quads");
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

/* Optional simulation-total witness; no transient-render capacity assumption. */
static bool sr_particle_cpu_snapshot(bgfx_texture_handle_t *texture, uint32_t alive)
{
    JceCvar *enabled = jce_cvar_find("r.particles.debug_stats");
    uint8_t rgba[4];
    if (!enabled || !jce_cvar_get_bool(enabled) ||
        !jce_sr_particle_count_encode(alive, rgba)) return false;
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->supported & BGFX_CAPS_TEXTURE_BLIT) ||
        !(caps->supported & BGFX_CAPS_TEXTURE_READ_BACK)) return false;
    if (!BGFX_HANDLE_IS_VALID((*texture))) {
        *texture = bgfx_create_texture_2d(1, 1, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA8,
            BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT, NULL, 0);
        if (!BGFX_HANDLE_IS_VALID((*texture))) return false;
    }
    const bgfx_memory_t *memory = bgfx_copy(rgba, sizeof rgba);
    if (!memory) return false;
    bgfx_update_texture_2d(*texture, 0, 0, 0, 0, 1, 1,
                           memory, UINT16_MAX);
    return true;
}

void sr_draw_particles(JceSceneRenderer *sr, JceScene *scene,
                       const JceCamera *camera, uint16_t view_id)
{
    /* camera was unused (the billboard basis is u_invView, set by the view
     * transform); the soft fade needs its near/far to linearise depth. */
    const JceSceneParticleState *state = jce_scene_internal_particle_state(scene);
    if (!state || !state->cpu) return;
    const JceParticleSystem *sys = state->cpu;
    uint32_t alive = jce_particles_alive_count(sys);
    sr->cpu_particle_count_valid =
        sr_particle_cpu_snapshot(&sr->cpu_particle_count_tex, alive) &&
        sr_particle_cpu_snapshot(&sr->cpu_particle_drawn_tex, 0);
    if (sr->cpu_particle_count_valid) {
        sr->cpu_particle_count_frame = jce_renderer_get_frame_index(sr->renderer);
        sr->cpu_particle_count_scene = scene;
    }
    if (alive == 0) return;

    const bool instanced = (jce_renderer_get_caps() & JCE_CAP_INSTANCING) != 0;
    sr_particle_sprite_lazy_init(sr, instanced);
    if (!BGFX_HANDLE_IS_VALID(sr->prog_particle_sprite)) return;

    const JceGpuParticleSoft soft = sr_particle_soft_frame(sr, camera);
    SrPrecipitationCover precipitation = {.sr = sr};
    uint32_t drawn;
    if (instanced) {
        if (!BGFX_HANDLE_IS_VALID(sr->particle_quad_vb) ||
            !BGFX_HANDLE_IS_VALID(sr->particle_quad_ib)) return;
        JceCvar *budget = jce_cvar_find("r.particles.cpu_upload_mb");
        int mb = budget ? jce_cvar_get_int(budget) : 128;
        if (mb < 0) mb = 0;
        if (mb > 256) mb = 256;
        SrParticleUpload upload;
        if (!sr_particle_upload_begin(sr->renderer, alive, (uint32_t)mb, &upload)) return;
        SrParticleBatch batch = {upload.data, 0u, upload.count};
        SrParticleEachCtx ctx = {sys, sr_particle_visit, &batch, &precipitation, NULL};
        if (jce_scene_count_particle_emitters(scene) > 0)
            jce_scene_each_particle_emitter(scene, sr_particle_each_entity, &ctx);
        if (!sr_particle_upload_bind(&upload, batch.count) || !batch.count) return;

        bgfx_set_vertex_buffer(0, sr->particle_quad_vb, 0, 4);
        bgfx_set_index_buffer(sr->particle_quad_ib, 0, 6);
        drawn = batch.count;
    } else {
        SrParticleExpandedUpload upload;
        if (!sr_particle_expanded_begin(sr->renderer, alive, &upload)) return;
        SrParticleEachCtx ctx = {sys, sr_particle_expanded_visit, &upload, &precipitation, NULL};
        if (jce_scene_count_particle_emitters(scene) > 0)
            jce_scene_each_particle_emitter(scene, sr_particle_each_entity, &ctx);
        if (!sr_particle_expanded_bind(&upload)) return;
        drawn = upload.count;
    }

    /* .x=0 selects the soft circle. CPU instances already carry
     * velocity*stretch, so .y enables elongation with .w left at zero. */
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
    if (sr->cpu_particle_count_valid)
        sr->cpu_particle_count_valid =
            sr_particle_cpu_snapshot(&sr->cpu_particle_drawn_tex, drawn);
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
 * GPU positions and box widths are in world space. Both paths use
 * the billboard programs; the GPU path also binds its authored texture. */

typedef struct {
    JceSceneRenderer *sr;
    uint16_t          compute_view;  /* base+9: dispatches only            */
    uint16_t          color_view;    /* base+0: instanced billboard draw   */
    float             dt;
    bool              dispatch;      /* false on 2nd+ render of a bgfx frame */
    bool              rain_impacts;  /* current rain and ordered transparency */
    int              *creates_left;  /* per-frame create budget (stagger)  */
    JceGpuParticleSoft soft;         /* depth fade, or off (fade == 0)     */
    SrPrecipitationCover precipitation;
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
    if (!jce_scene_particle_emitter_uses_gpu(s, c)) return;

    SrGpuParticleRec *rec = sr_gpu_particle_find_or_add(sr, e);
    if (!rec) {
        jce_scene_internal_gpu_particles_set_blocked();
        return;
    }

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
    const JceParticleEmitterDesc *d = &rec->desc;
    const bool precipitation = sr_particle_is_precipitation(s, e);
    const JceParticleCover *cover = precipitation ?
        sr_particle_precipitation_cover(&ctx->precipitation, s) : NULL;
    const bool want_rain = ctx->rain_impacts && precipitation && d->world_space &&
        d->velocity_max.y < -0.05f;
    jce_vec3 origin = jce_v3(0.0f, 0.0f, 0.0f);
    if (ctx->dispatch || want_rain) {
        const jce_mat4 world = jce_scene_get_world_matrix(s, e);
        origin = jce_v3(world.col[3].x, world.col[3].y, world.col[3].z);
    }
    JceParticleRainSurface surface;
    const JceParticleRainSurface *rain = want_rain &&
        sr_water_shore_gpu_rain_surface(sr, s, origin, d->spawn_box, &surface) ? &surface : NULL;

    if (ctx->dispatch) {

        /* Stop affects births; live particles still age on the GPU. */
        uint32_t n = 0;
        if (!c->emit_suppressed) {
            rec->emit_accum += d->emit_rate * ctx->dt;
            n = (uint32_t)rec->emit_accum;
            rec->emit_accum -= (float)n;
            if (!rec->burst_done) {
                if (d->emit_burst > 0.0f) n += (uint32_t)(d->emit_burst + 0.5f);
                rec->burst_done = true;
            }
        }

        JceGpuParticleEmitConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.emit_count   = n;
        cfg.origin       = origin;
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

        JceCvar *debug_stats = jce_cvar_find("r.particles.debug_stats");
        jce_gpu_particles_debug_enable(rec->sys,
            debug_stats && jce_cvar_get_bool(debug_stats));
        jce_gpu_particles_update_box(rec->sys, ctx->compute_view, ctx->dt,
                                     &cfg, d->spawn_box, rain, cover);
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
    jce_gpu_particles_render_stretched(rec->sys, ctx->color_view,
        tex_h, d->blend_alpha, &ctx->soft, d->velocity_stretch, cover, rain);
}

static void sr_gpu_particles_sweep(JceSceneRenderer *sr, bool all)
{
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++) {
        SrGpuParticleRec *r = &sr->gpu_particles[i];
        if (!r->used || (!all && r->referenced)) continue;
        if (r->sys) jce_gpu_particles_destroy(r->sys);
        memset(r, 0, sizeof(*r));
    }
}

void sr_drive_gpu_particles(JceSceneRenderer *sr, JceScene *scene,
                            const JceCamera *camera,
                            uint16_t view_id_base, uint16_t color_view, float dt_sec)
{
    if (!sr || !scene) return;
    const uint64_t scene_id = jce_scene_internal_instance_id(scene);
    if (sr->gpu_particle_scene_id != scene_id) {
        sr_gpu_particles_sweep(sr, true);
        sr->gpu_particle_scene_id = scene_id;
        sr->gpu_particle_frame_valid = false;
    }

    /* Un-mark first so the sweep also runs when the feature toggles off
     * (deferred pipeline toggle frees every pool next frame). */
    for (int i = 0; i < SR_GPU_PARTICLE_MAX; i++)
        sr->gpu_particles[i].referenced = false;

    /* Let pool creation report a missing PAK through the same CPU fallback. */
    if (jce_render_pipeline_is_feature_enabled("gpu_particles")) {
        /* Multi-viewport guard: the editor renders the scene more than once
         * per bgfx frame (scene view + game view); simulate/emit only on the
         * first render so dt is not applied twice. */
        uint32_t fi = jce_renderer_get_frame_index(sr->renderer);
        bool dispatch = !(sr->gpu_particle_frame_valid &&
                          sr->gpu_particle_frame == fi);

        if (dt_sec > 0.1f) dt_sec = 0.1f;   /* clamp long frames (CPU parity) */

        SrGpuParticleCtx ctx = {.precipitation = {.sr = sr}};
        ctx.sr           = sr;
        ctx.compute_view = (uint16_t)(view_id_base + JCE_VIEW_GPU_PARTICLE_OFFSET);
        ctx.color_view   = color_view;
        ctx.dt           = dt_sec;
        ctx.dispatch     = dispatch;
        const JceEnvironmentState *env = jce_scene_environment(scene);
        ctx.rain_impacts = color_view != view_id_base && env &&
            env->weather_type == JCE_ENV_WEATHER_RAIN && env->precipitation_rate > 0.0f;
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

    sr_gpu_particles_sweep(sr, false);
}
