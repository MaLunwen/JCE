/*
 * jce_gpu_particles.c -- compute-driven particle system.
 *
 * One dynamic vertex buffer doubles as the GPU particle pool *and*
 * the per-instance data stream for the billboard draw.  Each frame
 * we dispatch (in this order):
 *
 *   1. cs_particle_update : one thread per slot
 *   2. cs_particle_emit   : one thread per requested spawn
 *   3. instanced billboard draw of `max_particles` quads
 *
 * Slot layout (mirrors varying_particles.def.sc):
 *   vec4 0 : (px, py, pz, age)
 *   vec4 1 : (vx, vy, vz, lifetime)   -- lifetime <= 0 == dead
 *   vec4 2 : (r,  g,  b,  a)
 *   vec4 3 : (size, seed, _, _)
 */

#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_gpu_particles.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_shader_load.h"   /* backend suffix + engine-pak fallback */

#include <bgfx/c99/bgfx.h>

#include <stdio.h>
#include <string.h>

#define LOG_TAG "gpu-particles"

#define PARTICLE_VEC4_PER_SLOT 4u
#define PARTICLE_STRIDE_BYTES  (PARTICLE_VEC4_PER_SLOT * 16u)
#define EMIT_THREADS_X         64u
#define UPDATE_THREADS_X       64u

struct JceGpuParticleSystem {
    bool      supported;
    bool      needs_reset;          /* pool not yet GPU-zeroed (first tick) */
    /* Frames to suppress the draw after creation.  The draw (color view,
     * base+0) is sorted BEFORE the reset/update compute (base+9) within a
     * bgfx frame, so on the creation frame the draw would read the
     * uninitialized pool — garbage instances on Vulkan (a giant glowing
     * blob), NaN geometry that TDRs the device on D3D12 (the scene-switch
     * crash).  Draw only once a reset+update has executed in a PRIOR frame.
     * Decremented once per update(); render skips while > 0. */
    uint8_t   warmup;
    uint32_t  max_particles;        /* rounded to multiple of 64 */
    uint32_t  emit_cursor;          /* round-robin probe base */

    /* Allocator stashed at create() so destroy() can free with the
     * matching free function (paired-allocator invariant). */
    jce_allocator_t alloc;

    /* Pool buffer (compute UAV + instance VBO). */
    bgfx_dynamic_vertex_buffer_handle_t pool;

    /* Quad mesh (4 verts, 6 indices) shared across all instances. */
    bgfx_vertex_buffer_handle_t quad_vb;
    bgfx_index_buffer_handle_t  quad_ib;

    /* Vertex layout used for instancing the pool buffer. */
    bgfx_vertex_layout_t instance_layout;

    /* Programs. */
    bgfx_program_handle_t emit_program;
    bgfx_program_handle_t update_program;
    bgfx_program_handle_t render_program;
    bgfx_program_handle_t reset_program;   /* one-shot pool zero-fill */

    /* Compute uniforms. */
    bgfx_uniform_handle_t u_emit_params;
    bgfx_uniform_handle_t u_emit_origin;
    bgfx_uniform_handle_t u_emit_v_min;
    bgfx_uniform_handle_t u_emit_v_max;
    bgfx_uniform_handle_t u_emit_life_size;
    bgfx_uniform_handle_t u_emit_color;

    bgfx_uniform_handle_t u_particle_misc;  /* .x = textured flag (render) */
    bgfx_uniform_handle_t s_particle_tex;   /* billboard texture sampler   */

    bgfx_uniform_handle_t u_update_params;
    bgfx_uniform_handle_t u_update_grav;
    bgfx_uniform_handle_t u_size_lerp;
    bgfx_uniform_handle_t u_color_start;
    bgfx_uniform_handle_t u_color_end;
};

static bgfx_program_handle_t load_compute(const JcePakArchive *pak,
                                            const char *name, const char *sfx)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    bgfx_shader_handle_t cs = jce_shader_load_from_pak(pak, name, sfx, LOG_TAG);
    if (cs.idx == UINT16_MAX) return invalid;
    return bgfx_create_compute_program(cs, true);
}

static bgfx_program_handle_t load_vsfs(const JcePakArchive *pak,
                                         const char *vs, const char *fs,
                                         const char *sfx)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    bgfx_shader_handle_t vsh = jce_shader_load_from_pak(pak, vs, sfx, LOG_TAG);
    if (vsh.idx == UINT16_MAX) return invalid;
    bgfx_shader_handle_t fsh = jce_shader_load_from_pak(pak, fs, sfx, LOG_TAG);
    if (fsh.idx == UINT16_MAX) { bgfx_destroy_shader(vsh); return invalid; }
    return bgfx_create_program(vsh, fsh, true);
}

JceGpuParticleSystem *jce_gpu_particles_create(
    const JceGpuParticleSystemDesc *desc, jce_allocator_t alloc)
{
    if (!desc || !desc->pak || desc->max_particles == 0) return NULL;

    JceGpuParticleSystem *sys = (JceGpuParticleSystem *)alloc.alloc(
        sizeof(JceGpuParticleSystem), alloc.ctx);
    if (!sys) return NULL;
    memset(sys, 0, sizeof(*sys));
    sys->alloc = alloc;

    /* Round capacity up to the compute thread-group size. */
    uint32_t cap = desc->max_particles;
    cap = (cap + UPDATE_THREADS_X - 1) & ~(UPDATE_THREADS_X - 1);
    sys->max_particles = cap;

    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!(caps->supported & BGFX_CAPS_COMPUTE)) {
        LOG_WARN(LOG_TAG, "GPU has no compute support; particle system disabled");
        sys->supported = false;
        return sys; /* no-op mode */
    }

    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) {
        LOG_ERROR(LOG_TAG, "no shader suffix for current renderer");
        alloc.free(sys, alloc.ctx);
        return NULL;
    }

    /* Uniforms FIRST — GL order contract: bgfx's GL backend resolves a
     * program's user uniforms against the uniform registry at program-create
     * time; uniforms registered after the program never reach it (the shader
     * reads 0 — on GL the particle update saw count==0 and the pool never
     * simulated).  D3D/Vulkan read the blob's own uniform table, masking this.
     * See jce_gpu_scene.c (same fix, root-caused 2026-07-03). */
    sys->u_emit_params    = bgfx_create_uniform("u_emit_params",    BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_emit_origin    = bgfx_create_uniform("u_emit_origin",    BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_emit_v_min     = bgfx_create_uniform("u_emit_v_min",     BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_emit_v_max     = bgfx_create_uniform("u_emit_v_max",     BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_emit_life_size = bgfx_create_uniform("u_emit_life_size", BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_emit_color     = bgfx_create_uniform("u_emit_color",     BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_particle_misc  = bgfx_create_uniform("u_particle_misc",  BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->s_particle_tex   = bgfx_create_uniform("s_particleTex",    BGFX_UNIFORM_TYPE_SAMPLER, 1);
    sys->u_update_params  = bgfx_create_uniform("u_update_params",  BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_update_grav    = bgfx_create_uniform("u_update_grav",    BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_size_lerp      = bgfx_create_uniform("u_size_lerp",      BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_color_start    = bgfx_create_uniform("u_color_start",    BGFX_UNIFORM_TYPE_VEC4, 1);
    sys->u_color_end      = bgfx_create_uniform("u_color_end",      BGFX_UNIFORM_TYPE_VEC4, 1);

    sys->emit_program   = load_compute(desc->pak, "cs_particle_emit",   sfx);
    sys->update_program = load_compute(desc->pak, "cs_particle_update", sfx);
    sys->render_program = load_vsfs   (desc->pak, "vs_particle", "fs_particle", sfx);
    sys->reset_program  = load_compute(desc->pak, "cs_particle_reset",  sfx);
    if (sys->emit_program.idx == UINT16_MAX ||
        sys->update_program.idx == UINT16_MAX ||
        sys->render_program.idx == UINT16_MAX ||
        sys->reset_program.idx == UINT16_MAX) {
        LOG_ERROR(LOG_TAG, "shader load failed; particle system disabled");
        /* Uniforms are created BEFORE the loads (GL order contract) — free
         * them here since the !supported destroy path skips them. */
        bgfx_destroy_uniform(sys->u_emit_params);
        bgfx_destroy_uniform(sys->u_emit_origin);
        bgfx_destroy_uniform(sys->u_emit_v_min);
        bgfx_destroy_uniform(sys->u_emit_v_max);
        bgfx_destroy_uniform(sys->u_emit_life_size);
        bgfx_destroy_uniform(sys->u_emit_color);
        bgfx_destroy_uniform(sys->u_particle_misc);
        bgfx_destroy_uniform(sys->s_particle_tex);
        bgfx_destroy_uniform(sys->u_update_params);
        bgfx_destroy_uniform(sys->u_update_grav);
        bgfx_destroy_uniform(sys->u_size_lerp);
        bgfx_destroy_uniform(sys->u_color_start);
        bgfx_destroy_uniform(sys->u_color_end);
        if (sys->emit_program.idx   != UINT16_MAX) bgfx_destroy_program(sys->emit_program);
        if (sys->update_program.idx != UINT16_MAX) bgfx_destroy_program(sys->update_program);
        if (sys->render_program.idx != UINT16_MAX) bgfx_destroy_program(sys->render_program);
        if (sys->reset_program.idx  != UINT16_MAX) bgfx_destroy_program(sys->reset_program);
        sys->supported = false;
        return sys;
    }

    /* ------------------------------------------------------------- */
    /* Pool buffer: a single dynamic VB acting as compute UAV +      */
    /* instance source (4 vec4s per particle).                       */
    /* ------------------------------------------------------------- */
    bgfx_vertex_layout_t pool_layout;
    bgfx_vertex_layout_begin(&pool_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&pool_layout, BGFX_ATTRIB_TEXCOORD7,
                              4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pool_layout, BGFX_ATTRIB_TEXCOORD6,
                              4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pool_layout, BGFX_ATTRIB_TEXCOORD5,
                              4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&pool_layout, BGFX_ATTRIB_TEXCOORD4,
                              4, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&pool_layout);

    sys->instance_layout = pool_layout;

    sys->pool = bgfx_create_dynamic_vertex_buffer(
        cap, &pool_layout,
        BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_COMPUTE_FORMAT_32X4
        | BGFX_BUFFER_COMPUTE_TYPE_FLOAT);

    /* Zero-fill happens GPU-side (cs_particle_reset) on the first update
     * dispatch: CPU-updating a COMPUTE_READ_WRITE dynamic VB crashes the
     * D3D12 backend (BufferD3D12::update, renderer thread — the Upload-heap
     * staging hazard documented in cs_cull_reset.sc / jce_gpu_scene.c). */
    sys->needs_reset = true;
    /* 2 frames: skip the create frame (draw-view precedes compute-view) AND
     * the next, so the draw only reads a pool a full frame after its reset. */
    sys->warmup = 2u;

    /* ------------------------------------------------------------- */
    /* Quad mesh (POSITION + TEXCOORD0).                             */
    /* ------------------------------------------------------------- */
    bgfx_vertex_layout_t q_layout;
    bgfx_vertex_layout_begin(&q_layout, BGFX_RENDERER_TYPE_NOOP);
    bgfx_vertex_layout_add(&q_layout, BGFX_ATTRIB_POSITION,
                              3, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&q_layout, BGFX_ATTRIB_TEXCOORD0,
                              2, BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&q_layout);

    static const float quad_verts[] = {
        -0.5f,-0.5f, 0.0f,  0.0f, 0.0f,
         0.5f,-0.5f, 0.0f,  1.0f, 0.0f,
         0.5f, 0.5f, 0.0f,  1.0f, 1.0f,
        -0.5f, 0.5f, 0.0f,  0.0f, 1.0f,
    };
    static const uint16_t quad_idx[] = { 0, 1, 2, 0, 2, 3 };
    sys->quad_vb = bgfx_create_vertex_buffer(
        bgfx_copy(quad_verts, sizeof(quad_verts)), &q_layout, BGFX_BUFFER_NONE);
    sys->quad_ib = bgfx_create_index_buffer(
        bgfx_copy(quad_idx, sizeof(quad_idx)), BGFX_BUFFER_NONE);

    /* (uniforms created above, before the program loads — GL order contract.) */
    sys->supported = true;
    LOG_SUCCESS(LOG_TAG, "GPU particle system online (%u slots)", cap);
    return sys;
}

void jce_gpu_particles_destroy(JceGpuParticleSystem *sys)
{
    if (!sys) return;
    if (sys->supported) {
        bgfx_destroy_dynamic_vertex_buffer(sys->pool);
        bgfx_destroy_vertex_buffer(sys->quad_vb);
        bgfx_destroy_index_buffer (sys->quad_ib);
        bgfx_destroy_program(sys->emit_program);
        bgfx_destroy_program(sys->update_program);
        bgfx_destroy_program(sys->render_program);
        bgfx_destroy_program(sys->reset_program);
        bgfx_destroy_uniform(sys->u_emit_params);
        bgfx_destroy_uniform(sys->u_emit_origin);
        bgfx_destroy_uniform(sys->u_emit_v_min);
        bgfx_destroy_uniform(sys->u_emit_v_max);
        bgfx_destroy_uniform(sys->u_emit_life_size);
        bgfx_destroy_uniform(sys->u_emit_color);
        bgfx_destroy_uniform(sys->u_particle_misc);
        bgfx_destroy_uniform(sys->s_particle_tex);
        bgfx_destroy_uniform(sys->u_update_params);
        bgfx_destroy_uniform(sys->u_update_grav);
        bgfx_destroy_uniform(sys->u_size_lerp);
        bgfx_destroy_uniform(sys->u_color_start);
        bgfx_destroy_uniform(sys->u_color_end);
    }
    /* Free with the same allocator the caller passed to create(). */
    sys->alloc.free(sys, sys->alloc.ctx);
}

bool jce_gpu_particles_is_supported(const JceGpuParticleSystem *sys)
{
    return sys && sys->supported;
}

uint32_t jce_gpu_particles_capacity(const JceGpuParticleSystem *sys)
{
    return sys ? sys->max_particles : 0;
}

void jce_gpu_particles_update(JceGpuParticleSystem    *sys,
                                uint16_t                 view,
                                float                    dt,
                                const JceGpuParticleEmitConfig *cfg)
{
    if (!sys || !sys->supported || !cfg) return;
    JCE_PROFILE_ZONE_N("GpuParticles::Update");

    /* ── One-shot pool zero-fill (GPU-side; see create()). bgfx inserts
     * a UAV barrier between same-view compute dispatches, so the zeroes
     * are visible to the update/emit passes below. ────────────────── */
    jce_gpu_particles_reset(sys, view);

    /* ── Update dispatch (one workgroup per 64 slots). ───────────── */
    {
        float params[4]  = { (float)sys->max_particles, dt, 0.0f, 0.0f };
        float grav[4]    = { cfg->gravity.x, cfg->gravity.y, cfg->gravity.z,
                             cfg->damping };
        float size[4]    = { cfg->size_start, cfg->size_end, 0.0f, 0.0f };
        float c0[4]      = { cfg->color_start.x, cfg->color_start.y,
                             cfg->color_start.z, cfg->color_start.w };
        float c1[4]      = { cfg->color_end.x, cfg->color_end.y,
                             cfg->color_end.z, cfg->color_end.w };

        bgfx_set_uniform(sys->u_update_params, params, 1);
        bgfx_set_uniform(sys->u_update_grav,   grav,   1);
        bgfx_set_uniform(sys->u_size_lerp,     size,   1);
        bgfx_set_uniform(sys->u_color_start,   c0,     1);
        bgfx_set_uniform(sys->u_color_end,     c1,     1);

        bgfx_set_compute_dynamic_vertex_buffer(0, sys->pool, BGFX_ACCESS_READWRITE);
        uint32_t groups = (sys->max_particles + UPDATE_THREADS_X - 1)
                          / UPDATE_THREADS_X;
        bgfx_dispatch(view, sys->update_program, groups, 1, 1, BGFX_DISCARD_ALL);
    }

    /* ── Emit dispatch. ──────────────────────────────────────────── */
    if (cfg->emit_count > 0) {
        sys->emit_cursor = (sys->emit_cursor + 7919u) % sys->max_particles;
        float params[4] = { (float)cfg->emit_count, (float)sys->emit_cursor,
                            (float)sys->max_particles, dt };
        static uint32_t frame_seed = 1u;
        ++frame_seed;
        float origin[4] = { cfg->origin.x, cfg->origin.y, cfg->origin.z,
                            (float)frame_seed };
        float vmn[4] = { cfg->velocity_min.x, cfg->velocity_min.y,
                         cfg->velocity_min.z, 0.0f };
        float vmx[4] = { cfg->velocity_max.x, cfg->velocity_max.y,
                         cfg->velocity_max.z, 0.0f };
        float life_size[4] = { cfg->lifetime_min, cfg->lifetime_max,
                               cfg->size_start, cfg->size_end };
        float col[4] = { cfg->color_start.x, cfg->color_start.y,
                         cfg->color_start.z, cfg->color_start.w };

        bgfx_set_uniform(sys->u_emit_params,    params,    1);
        bgfx_set_uniform(sys->u_emit_origin,    origin,    1);
        bgfx_set_uniform(sys->u_emit_v_min,     vmn,       1);
        bgfx_set_uniform(sys->u_emit_v_max,     vmx,       1);
        bgfx_set_uniform(sys->u_emit_life_size, life_size, 1);
        bgfx_set_uniform(sys->u_emit_color,     col,       1);

        bgfx_set_compute_dynamic_vertex_buffer(0, sys->pool, BGFX_ACCESS_READWRITE);
        uint32_t groups = (cfg->emit_count + EMIT_THREADS_X - 1) / EMIT_THREADS_X;
        bgfx_dispatch(view, sys->emit_program, groups, 1, 1, BGFX_DISCARD_ALL);
    }

    /* Count down the post-create draw suppression: one full frame's
     * reset+update has now been submitted, so the pool a draw reads next
     * frame is valid.  (render skips while warmup > 0.) */
    if (sys->warmup > 0u) sys->warmup--;
    JCE_PROFILE_ZONE_END;
}

/* One-shot GPU pool zero-fill.  MUST run before the pool's first render:
 * the dynamic VB is uninitialized GPU memory at creation, and drawing it
 * produces garbage instances (VK: giant glowing blob) or NaN/huge triangles
 * that TDR the device (D3D12: DEVICE_REMOVED -> every later Create* fails,
 * the scene-switch crash).  Callers that create a system on a non-dispatch
 * view pass invoke this immediately after creation; the update path calls
 * it too (no-op once done). */
void jce_gpu_particles_reset(JceGpuParticleSystem *sys, uint16_t view)
{
    if (!sys || !sys->supported || !sys->needs_reset) return;
    float params[4] = { (float)sys->max_particles, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(sys->u_update_params, params, 1);
    bgfx_set_compute_dynamic_vertex_buffer(0, sys->pool, BGFX_ACCESS_WRITE);
    uint32_t groups = (sys->max_particles + UPDATE_THREADS_X - 1)
                      / UPDATE_THREADS_X;
    bgfx_dispatch(view, sys->reset_program, groups, 1, 1, BGFX_DISCARD_ALL);
    sys->needs_reset = false;
}

void jce_gpu_particles_render(JceGpuParticleSystem *sys, uint16_t view)
{
    if (!sys || !sys->supported) return;
    JCE_PROFILE_ZONE_N("GpuParticles::Render");
    jce_gpu_particles_render_ex(sys, view, UINT16_MAX, false);
    JCE_PROFILE_ZONE_END;
}

void jce_gpu_particles_render_ex(JceGpuParticleSystem *sys, uint16_t view,
                                 uint16_t texture_idx, bool blend_alpha)
{
    if (!sys || !sys->supported) return;
    /* Never draw an un-reset / not-yet-warmed pool: the instance buffer is
     * uninitialized GPU memory until cs_particle_reset runs AND a full frame
     * has passed (the draw view sorts before the compute view, so a
     * same-frame reset is not yet visible to the draw).  Drawing garbage =
     * a glowing blob on Vulkan / NaN geometry that TDRs D3D12 (the
     * scene-switch crash). */
    if (sys->needs_reset || sys->warmup > 0u) return;

    bgfx_set_vertex_buffer(0, sys->quad_vb, 0, 4);
    bgfx_set_index_buffer (sys->quad_ib, 0, 6);
    bgfx_set_instance_data_from_dynamic_vertex_buffer(
        sys->pool, 0, sys->max_particles);

    /* Optional billboard texture: u_particle_misc.x flags the shader path
     * (0 = the legacy procedural soft circle — byte-identical output). */
    bgfx_texture_handle_t tex = { texture_idx };
    const bool textured = (texture_idx != UINT16_MAX);
    float misc[4] = { textured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
    bgfx_set_uniform(sys->u_particle_misc, misc, 1);
    if (textured)
        bgfx_set_texture(0, sys->s_particle_tex, tex, UINT32_MAX);

    /* Additive (fire/glow, legacy default) vs classic alpha (smoke/dust). */
    const uint64_t blend = blend_alpha
        ? BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                BGFX_STATE_BLEND_INV_SRC_ALPHA)
        : BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA,
                                BGFX_STATE_BLEND_ONE);
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                  | BGFX_STATE_DEPTH_TEST_LESS
                  | BGFX_STATE_CULL_CW
                  | blend, 0);
    bgfx_submit(view, sys->render_program, 0, BGFX_DISCARD_ALL);
}
