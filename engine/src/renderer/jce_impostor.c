/*
 * jce_impostor.c  Octahedral impostors — terminal-LOD billboard cards.
 *
 * See jce_impostor.h for the contract.  Two halves:
 *   BAKE  — frame-driven GPU pass that renders the model from grid_n x grid_n
 *           octahedral viewpoints into one atlas FBO, reads it back (RGBA, alpha
 *           = coverage), and writes a PNG atlas + .impostor.json sidecar.
 *   DRAW  — a unit-quad VB + impostor program; far-cards are submitted as one
 *           instanced draw per atlas (the FS samples the octahedral cell for the
 *           card's view direction).
 *
 * The bake runs on the render thread (bgfx is not thread-safe) and consumes the
 * application's existing per-frame bgfx_frame(): _submit kicks it, _poll renders
 * the atlas views + blits to a read-back staging texture + issues
 * bgfx_read_texture, then harvests the CPU pixels once bgfx reports the promised
 * frame index has elapsed (the proven pick-pass readback pattern).
 */

#include <jce/renderer/jce_impostor.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "impostor"

/* Dedicated bgfx view RANGE for the bake: one view per atlas cell, each with
 * its own rect into the shared atlas FBO + its own ortho camera (view-level
 * transform/rect, so no per-draw scissor games that break multi-primitive
 * models).  Base 128 keeps clear of the scene/editor/post views below and the
 * init-clear band (232) / imgui (250) / ui (254) above: 128 + gridN^2 - 1 must
 * stay < 232, so the single-frame bake clamps gridN to 10 (100 cells). */
#define JCE_VIEW_IMPOSTOR_BAKE_BASE 128
#define JCE_IMPOSTOR_BAKE_MAX_GRID  10

/* ── Octahedral direction math (must match fs_impostor.sc oct_encode) ── */

jce_vec2 jce_impostor_oct_encode(jce_vec3 dir)
{
    float l1 = fabsf(dir.x) + fabsf(dir.y) + fabsf(dir.z);
    if (l1 < 1e-8f) l1 = 1e-8f;
    float x = dir.x / l1;
    float y = dir.y / l1;
    float z = dir.z / l1;
    jce_vec2 oct = jce_v2(x, z);
    if (y < 0.0f) {
        float ox = (1.0f - fabsf(oct.y)) * (oct.x >= 0.0f ? 1.0f : -1.0f);
        float oy = (1.0f - fabsf(oct.x)) * (oct.y >= 0.0f ? 1.0f : -1.0f);
        oct = jce_v2(ox, oy);
    }
    return jce_v2(oct.x * 0.5f + 0.5f, oct.y * 0.5f + 0.5f);
}

jce_vec3 jce_impostor_oct_decode(jce_vec2 uv)
{
    float ox = uv.x * 2.0f - 1.0f;
    float oz = uv.y * 2.0f - 1.0f;
    float oy = 1.0f - (fabsf(ox) + fabsf(oz));
    if (oy < 0.0f) {
        float nx = (1.0f - fabsf(oz)) * (ox >= 0.0f ? 1.0f : -1.0f);
        float nz = (1.0f - fabsf(ox)) * (oz >= 0.0f ? 1.0f : -1.0f);
        ox = nx; oz = nz;
    }
    jce_vec3 d = jce_v3(ox, oy, oz);
    return jce_v3_normalize(d);
}

/* ── Metadata sidecar ─────────────────────────────────────────────── */

bool jce_impostor_meta_write(const char *json_path, const JceImpostorMeta *meta)
{
    if (!json_path || !json_path[0] || !meta) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_string(root, "type", "impostor");
    jce_json_set_int(root, "gridN", meta->grid_n);
    jce_json_set_int(root, "cellPx", meta->cell_px);
    jce_json_set_float_array(root, "center", meta->center, 3);
    jce_json_set_number(root, "radius", meta->radius);
    jce_json_set_string(root, "atlasPath", meta->atlas_path);

    char *s = jce_json_print(root, true);
    jce_json_free(root);
    if (!s) return false;
    bool ok = jce_fs_host_write_all(json_path, s, strlen(s));
    jce_json_free_string(s);
    if (!ok) LOG_WARN(LOG_TAG, "cannot write impostor metadata: %s", json_path);
    return ok;
}

bool jce_impostor_meta_read(const char *json_path, JceImpostorMeta *out_meta)
{
    if (!json_path || !json_path[0] || !out_meta) return false;
    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(json_path, &sz);
    if (!buf || sz == 0) { if (buf) jce_fs_buffer_free(buf); return false; }
    JceJson *root = jce_json_parse(buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    if (!root) return false;

    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->grid_n  = jce_json_get_int(root, "gridN", 8);
    out_meta->cell_px = jce_json_get_int(root, "cellPx", 256);
    jce_json_get_floats(root, "center", out_meta->center, 3, NULL);
    out_meta->radius  = (float)jce_json_get_number(root, "radius", 1.0);
    const char *ap = jce_json_get_string(root, "atlasPath", NULL);
    if (ap) snprintf(out_meta->atlas_path, sizeof out_meta->atlas_path, "%s", ap);
    jce_json_free(root);

    if (out_meta->grid_n < 2) out_meta->grid_n = 2;
    if (out_meta->grid_n > JCE_IMPOSTOR_MAX_GRID) out_meta->grid_n = JCE_IMPOSTOR_MAX_GRID;
    if (out_meta->radius <= 0.0f) out_meta->radius = 1.0f;
    return out_meta->atlas_path[0] != '\0';
}

/* ── Shared GPU resources (program + unit quad + uniforms) ─────────── */

static struct {
    bool                  init_tried;
    bgfx_program_handle_t prog;            /* vs_impostor + fs_impostor      */
    bgfx_vertex_layout_t  quad_layout;     /* pos3 + uv2                      */
    bgfx_vertex_buffer_handle_t quad_vb;   /* unit quad (4 verts)            */
    bgfx_index_buffer_handle_t  quad_ib;   /* 2 tris                         */
    bgfx_uniform_handle_t u_atlas;         /* s_impostorAtlas (stage 0)      */
    bgfx_uniform_handle_t u_lightDir;      /* u_octLightDir                  */
    bgfx_uniform_handle_t u_lightColor;    /* u_octLightColor                */
} g_gfx;

static void impostor_gfx_init(JceRenderer *r)
{
    if (g_gfx.init_tried) return;
    g_gfx.init_tried = true;
    g_gfx.prog.idx = UINT16_MAX;
    g_gfx.quad_vb.idx = UINT16_MAX;
    g_gfx.quad_ib.idx = UINT16_MAX;
    g_gfx.u_atlas.idx = UINT16_MAX;
    g_gfx.u_lightDir.idx = UINT16_MAX;
    g_gfx.u_lightColor.idx = UINT16_MAX;

    /* Program from the engine-embedded PAK (the impostor shaders live in the
       pbr/ shader dir, baked into jce_renderer via JCE_EMBED_ENGINE_SHADERS, so
       they share varying_pbr.def.sc — vs_impostor + fs_impostor).  shader_load_
       program already falls back to the embedded engine pak on a miss, so a NULL
       pak here resolves the embedded shaders directly. */
    (void)r;
    JceShaderHandle prog = shader_load_program(jce_shaders_embedded_engine_pak(),
                                               "impostor");
    g_gfx.prog.idx = prog.idx;
    if (g_gfx.prog.idx == UINT16_MAX) {
        LOG_WARN(LOG_TAG, "impostor shader program not found in PAK; cards disabled");
        return;
    }

    /* Unit quad: pos.xy in [-0.5,0.5], z=0; uv in [0,1]. */
    bgfx_vertex_layout_begin(&g_gfx.quad_layout, bgfx_get_renderer_type());
    bgfx_vertex_layout_add(&g_gfx.quad_layout, BGFX_ATTRIB_POSITION, 3,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_add(&g_gfx.quad_layout, BGFX_ATTRIB_TEXCOORD0, 2,
                           BGFX_ATTRIB_TYPE_FLOAT, false, false);
    bgfx_vertex_layout_end(&g_gfx.quad_layout);

    static const float quad_verts[] = {
        /* pos.x  pos.y  pos.z   u    v   */
        -0.5f, -0.5f, 0.0f,   0.0f, 1.0f,
         0.5f, -0.5f, 0.0f,   1.0f, 1.0f,
         0.5f,  0.5f, 0.0f,   1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f,   0.0f, 0.0f,
    };
    static const uint16_t quad_idx[] = { 0, 1, 2, 0, 2, 3 };

    g_gfx.quad_vb = bgfx_create_vertex_buffer(
        bgfx_copy(quad_verts, sizeof quad_verts), &g_gfx.quad_layout, BGFX_BUFFER_NONE);
    g_gfx.quad_ib = bgfx_create_index_buffer(
        bgfx_copy(quad_idx, sizeof quad_idx), BGFX_BUFFER_NONE);

    g_gfx.u_atlas      = bgfx_create_uniform("s_impostorAtlas", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    g_gfx.u_lightDir   = bgfx_create_uniform("u_octLightDir",   BGFX_UNIFORM_TYPE_VEC4, 1);
    g_gfx.u_lightColor = bgfx_create_uniform("u_octLightColor", BGFX_UNIFORM_TYPE_VEC4, 1);
}

/* ── Runtime atlas load + instanced card draw ──────────────────────── */

bool jce_impostor_atlas_load(const char *meta_path,
                             JceImpostorTexLoadFn tex_load, void *user,
                             JceImpostorAtlas *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->atlas.idx = UINT16_MAX;
    if (!meta_path || !meta_path[0] || !tex_load) return false;
    if (!jce_impostor_meta_read(meta_path, &out->meta)) return false;
    out->atlas = tex_load(user, out->meta.atlas_path);
    out->valid = jce_texture_valid(out->atlas);
    return out->valid;
}

void jce_impostor_draw_instanced(JceRenderer *r, uint16_t view_id,
                                 const JceImpostorAtlas *atlas,
                                 const JceImpostorInstance *insts, uint32_t count,
                                 const float light_dir[3],
                                 const float light_color[4])
{
    if (!r || !atlas || !atlas->valid || !insts || count == 0) return;
    impostor_gfx_init(r);
    if (g_gfx.prog.idx == UINT16_MAX) return;

    /* Allocate the per-instance buffer: 2 vec4 (32 bytes) per card.
       i_data0 = (center.xyz, radius); i_data1 = (gridN, tint.rgb). */
    const uint16_t stride = 32;
    uint32_t avail = bgfx_get_avail_instance_data_buffer(count, stride);
    if (avail == 0) return;
    bgfx_instance_data_buffer_t idb;
    bgfx_alloc_instance_data_buffer(&idb, avail, stride);
    float *dst = (float *)idb.data;
    for (uint32_t i = 0; i < avail; ++i) {
        const JceImpostorInstance *in = &insts[i];
        dst[0] = in->center[0]; dst[1] = in->center[1]; dst[2] = in->center[2];
        dst[3] = in->radius;
        dst[4] = (float)atlas->meta.grid_n;
        dst[5] = in->tint[0]; dst[6] = in->tint[1]; dst[7] = in->tint[2];
        dst += 8;
    }

    /* Sun term uniforms. */
    float ld[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    if (light_dir) { ld[0] = light_dir[0]; ld[1] = light_dir[1]; ld[2] = light_dir[2]; }
    float lc[4] = { 0.85f, 0.85f, 0.85f, 0.25f };  /* rgb sun, w ambient */
    if (light_color) { lc[0]=light_color[0]; lc[1]=light_color[1]; lc[2]=light_color[2]; lc[3]=light_color[3]; }
    bgfx_set_uniform(g_gfx.u_lightDir, ld, 1);
    bgfx_set_uniform(g_gfx.u_lightColor, lc, 1);

    bgfx_texture_handle_t atex = { atlas->atlas.idx };
    bgfx_set_texture(0, g_gfx.u_atlas, atex, UINT32_MAX);

    bgfx_set_vertex_buffer(0, g_gfx.quad_vb, 0, 4);
    bgfx_set_index_buffer(g_gfx.quad_ib, 0, 6);
    bgfx_set_instance_data_buffer(&idb, 0, avail);
    /* Opaque alpha-tested cards: depth test+write, no blend, no backface cull
       (the billboard can face either way). */
    bgfx_set_state(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                   BGFX_STATE_WRITE_Z  | BGFX_STATE_DEPTH_TEST_LESS, 0);
    bgfx_submit(view_id, g_gfx.prog, 0, BGFX_DISCARD_ALL);
}

/* ── GPU bake state machine ────────────────────────────────────────── */

typedef struct {
    JceImpostorBakeStatus status;
    JceImpostorBakeDesc   desc;
    int                   grid_n;
    int                   cell_px;
    int                   atlas_px;       /* grid_n * cell_px */
    jce_vec3              center;
    float                 radius;

    bgfx_frame_buffer_handle_t fbo;
    bgfx_texture_handle_t       color;
    bgfx_texture_handle_t       depth;

    /* GPU->CPU readback via blit + bgfx_read_texture (the proven pick-pass
       pattern; the screenshot callback does not deliver for offscreen FBOs in
       this bgfx build).  staging is BLIT_DST|READ_BACK; pixels is the CPU dst. */
    bgfx_texture_handle_t       staging;     /* BLIT_DST | READ_BACK RGBA8 */
    uint8_t  *pixels;        /* RGBA8, atlas_px*atlas_px*4 (read_texture dst) */
    uint32_t  ready_frame;   /* bgfx frame index after which pixels are valid */
    bool      blit_done;     /* blit + read_texture issued */
} ImpostorBake;

static ImpostorBake g_bake;

static void bake_destroy_fbo(void)
{
    if (BGFX_HANDLE_IS_VALID(g_bake.fbo)) bgfx_destroy_frame_buffer(g_bake.fbo);
    if (BGFX_HANDLE_IS_VALID(g_bake.staging)) bgfx_destroy_texture(g_bake.staging);
    g_bake.fbo.idx = UINT16_MAX;
    g_bake.color.idx = UINT16_MAX;
    g_bake.depth.idx = UINT16_MAX;
    g_bake.staging.idx = UINT16_MAX;
}

bool jce_impostor_bake_submit(const JceImpostorBakeDesc *desc)
{
    if (!desc || !desc->model || !desc->renderer) {
        LOG_WARN(LOG_TAG, "bake submit: missing model/renderer");
        return false;
    }
    if (g_bake.status == JCE_IMPOSTOR_BAKE_RENDERING ||
        g_bake.status == JCE_IMPOSTOR_BAKE_READBACK) {
        LOG_WARN(LOG_TAG, "bake rejected: one already in flight");
        return false;
    }
    if (!desc->atlas_path_host[0] || !desc->meta_path_host[0]) {
        LOG_WARN(LOG_TAG, "bake submit: missing output paths");
        return false;
    }

    memset(&g_bake, 0, sizeof(g_bake));
    g_bake.desc = *desc;
    g_bake.fbo.idx = UINT16_MAX;
    g_bake.color.idx = UINT16_MAX;
    g_bake.depth.idx = UINT16_MAX;
    g_bake.staging.idx = UINT16_MAX;

    int gn = desc->grid_n;
    if (gn < 2) gn = 8;
    /* Single-frame bake uses one bgfx view per cell, so clamp to the view-budget
       grid (gn^2 views fit between BASE and the init-clear band). */
    if (gn > JCE_IMPOSTOR_BAKE_MAX_GRID) gn = JCE_IMPOSTOR_BAKE_MAX_GRID;
    int cp = desc->cell_px;
    if (cp < 32) cp = 256;
    if (cp > 512) cp = 512;
    g_bake.grid_n = gn;
    g_bake.cell_px = cp;
    g_bake.atlas_px = gn * cp;

    /* Model bounds → center + radius. */
    float mn[3], mx[3];
    if (!jce_model_get_aabb(desc->model, mn, mx)) {
        LOG_WARN(LOG_TAG, "bake submit: model has no bounds");
        g_bake.status = JCE_IMPOSTOR_BAKE_FAILED;
        return false;
    }
    g_bake.center = jce_v3((mn[0]+mx[0])*0.5f, (mn[1]+mx[1])*0.5f, (mn[2]+mx[2])*0.5f);
    float ex = (mx[0]-mn[0])*0.5f, ey = (mx[1]-mn[1])*0.5f, ez = (mx[2]-mn[2])*0.5f;
    g_bake.radius = sqrtf(ex*ex + ey*ey + ez*ez);
    if (g_bake.radius <= 0.0f) g_bake.radius = 1.0f;

    /* Atlas FBO (RGBA8 color + depth).  Clamp atlas size to a GPU-safe ceiling. */
    int apx = g_bake.atlas_px;
    if (apx > 4096) { apx = 4096; g_bake.cell_px = apx / gn; g_bake.atlas_px = g_bake.cell_px * gn; apx = g_bake.atlas_px; }

    g_bake.color = bgfx_create_texture_2d((uint16_t)apx, (uint16_t)apx, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
    g_bake.depth = bgfx_create_texture_2d((uint16_t)apx, (uint16_t)apx, false, 1,
        BGFX_TEXTURE_FORMAT_D24S8, BGFX_TEXTURE_RT, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(g_bake.color) || !BGFX_HANDLE_IS_VALID(g_bake.depth)) {
        LOG_ERROR(LOG_TAG, "bake: atlas RT alloc failed (%dx%d)", apx, apx);
        if (BGFX_HANDLE_IS_VALID(g_bake.color)) bgfx_destroy_texture(g_bake.color);
        if (BGFX_HANDLE_IS_VALID(g_bake.depth)) bgfx_destroy_texture(g_bake.depth);
        g_bake.status = JCE_IMPOSTOR_BAKE_FAILED;
        return false;
    }
    bgfx_attachment_t att[2];
    bgfx_attachment_init(&att[0], g_bake.color, BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    bgfx_attachment_init(&att[1], g_bake.depth, BGFX_ACCESS_WRITE, 0, 1, 0, BGFX_RESOLVE_NONE);
    g_bake.fbo = bgfx_create_frame_buffer_from_attachment(2, att, true);
    if (!BGFX_HANDLE_IS_VALID(g_bake.fbo)) {
        LOG_ERROR(LOG_TAG, "bake: atlas FBO create failed");
        bgfx_destroy_texture(g_bake.color);
        bgfx_destroy_texture(g_bake.depth);
        g_bake.status = JCE_IMPOSTOR_BAKE_FAILED;
        return false;
    }
    g_bake.color = bgfx_get_texture(g_bake.fbo, 0);

    /* CPU-readback staging texture (blit destination, read-back capable) +
       the CPU pixel buffer that bgfx_read_texture fills. */
    g_bake.staging = bgfx_create_texture_2d((uint16_t)apx, (uint16_t)apx, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA8,
        BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK |
        BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
    if (!BGFX_HANDLE_IS_VALID(g_bake.staging)) {
        LOG_ERROR(LOG_TAG, "bake: staging readback texture alloc failed");
        bake_destroy_fbo();
        g_bake.status = JCE_IMPOSTOR_BAKE_FAILED;
        return false;
    }
    g_bake.pixels = (uint8_t *)JCE_MALLOC((size_t)apx * apx * 4u);
    if (!g_bake.pixels) {
        LOG_ERROR(LOG_TAG, "bake: CPU pixel buffer alloc failed");
        bake_destroy_fbo();
        g_bake.status = JCE_IMPOSTOR_BAKE_FAILED;
        return false;
    }

    g_bake.status = JCE_IMPOSTOR_BAKE_RENDERING;
    LOG_INFO(LOG_TAG, "bake start: grid=%dx%d cell=%d atlas=%dpx r=%.2f",
             gn, gn, g_bake.cell_px, g_bake.atlas_px, g_bake.radius);
    return true;
}

/* Render all grid_n^2 octahedral views of the model into the atlas FBO.  Each
   cell is its OWN bgfx view (own rect into the shared FBO + own ortho camera),
   so view-level transform/rect apply correctly even for multi-primitive models
   (a single view + per-draw scissor would lose the scissor on prim 2+). */
static void bake_render_views(void)
{
    const int gn = g_bake.grid_n;
    const int cp = g_bake.cell_px;
    const float r = g_bake.radius;
    const jce_vec3 c = g_bake.center;
    const bool homog = false; /* match the offscreen/editor depth convention */

    /* Ortho projection that fits the bounds sphere; near/far span the sphere. */
    jce_mat4 proj = jce_m4_ortho(-r, r, -r, r, 0.01f, r * 4.0f + 2.0f, homog);
    jce_mat4 ident = jce_m4_identity();

    /* Capture LOD0 (base geometry) at full detail; clear any stale draw-state
       hooks left armed by the scene renderer's last color pass. */
    jce_model_set_draw_lod(0);
    jce_model_set_pre_submit_cb(NULL, NULL);
    jce_model_set_material_override(NULL);

    /* Full-atlas clear (transparent) on a view that owns the WHOLE FBO rect and
       sorts BEFORE every cell view (id < BASE).  bgfx_set_view_clear only
       affects the view's own rect, so the per-cell views (each with a cell-sized
       rect) cannot clear the whole atlas — this dedicated pass does. */
    {
        uint16_t cv = (uint16_t)(JCE_VIEW_IMPOSTOR_BAKE_BASE - 1);
        bgfx_set_view_frame_buffer(cv, g_bake.fbo);
        bgfx_set_view_rect(cv, 0, 0, (uint16_t)g_bake.atlas_px,
                           (uint16_t)g_bake.atlas_px);
        bgfx_set_view_clear(cv, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                            0x00000000, 1.0f, 0);
        bgfx_touch(cv);
    }

    for (int row = 0; row < gn; ++row) {
        for (int col = 0; col < gn; ++col) {
            int cell = row * gn + col;
            uint16_t v = (uint16_t)(JCE_VIEW_IMPOSTOR_BAKE_BASE + cell);

            /* Cell center UV → octahedral direction (the view from that angle). */
            float u = ((float)col + 0.5f) / (float)gn;
            float w = ((float)row + 0.5f) / (float)gn;
            jce_vec3 dir = jce_impostor_oct_decode(jce_v2(u, w));

            /* Camera placed along +dir looking back at the bounds center. */
            jce_vec3 eye = jce_v3(c.x + dir.x * (r * 2.0f),
                                  c.y + dir.y * (r * 2.0f),
                                  c.z + dir.z * (r * 2.0f));
            jce_vec3 up = (fabsf(dir.y) > 0.99f) ? jce_v3(0,0,1) : jce_v3(0,1,0);
            jce_mat4 view = jce_m4_look_at(eye, c, up);

            bgfx_set_view_frame_buffer(v, g_bake.fbo);
            bgfx_set_view_rect(v, (uint16_t)(col * cp), (uint16_t)(row * cp),
                               (uint16_t)cp, (uint16_t)cp);
            /* No per-cell clear: the dedicated full-atlas pass (id BASE-1) above
               already cleared color+depth across the whole atlas, and each cell
               view writes only its own (distinct) pixel region. */
            bgfx_set_view_clear(v, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
            bgfx_set_view_mode(v, BGFX_VIEW_MODE_SEQUENTIAL);
            bgfx_set_view_transform(v, view.raw[0], proj.raw[0]);
            bgfx_touch(v);

            jce_model_draw(g_bake.desc.model, g_bake.desc.renderer, v, &ident, NULL, 0);
        }
    }

    /* Blit the rendered atlas into the read-back staging texture, then request a
       CPU read.  Use a blit view that sorts AFTER every cell view so the atlas is
       fully rendered first.  bgfx_read_texture returns the frame index after
       which g_bake.pixels is valid (poll for it). */
    uint16_t blit_view = (uint16_t)(JCE_VIEW_IMPOSTOR_BAKE_BASE +
                                    JCE_IMPOSTOR_BAKE_MAX_GRID * JCE_IMPOSTOR_BAKE_MAX_GRID);
    bgfx_blit(blit_view, g_bake.staging, 0, 0, 0, 0,
              g_bake.color, 0, 0, 0, 0,
              (uint16_t)g_bake.atlas_px, (uint16_t)g_bake.atlas_px, 1);
    g_bake.ready_frame = bgfx_read_texture(g_bake.staging, g_bake.pixels, 0, 0);
    g_bake.blit_done = true;
}

/* Encode the read-back atlas pixels to a PNG (alpha preserved) via SDL_image. */
static bool bake_write_atlas_png(void)
{
    if (!g_bake.pixels || g_bake.atlas_px == 0) return false;
    SDL_Surface *surf = SDL_CreateSurfaceFrom((int)g_bake.atlas_px, (int)g_bake.atlas_px,
        SDL_PIXELFORMAT_RGBA32, g_bake.pixels, (int)(g_bake.atlas_px * 4u));
    if (!surf) { LOG_ERROR(LOG_TAG, "bake: surface wrap failed: %s", SDL_GetError()); return false; }
    bool ok = IMG_SavePNG(surf, g_bake.desc.atlas_path_host);
    SDL_DestroySurface(surf);
    if (!ok) LOG_ERROR(LOG_TAG, "bake: PNG write failed: %s (%s)",
                       g_bake.desc.atlas_path_host, SDL_GetError());
    return ok;
}

JceImpostorBakeStatus jce_impostor_bake_poll(void)
{
    switch (g_bake.status) {
    case JCE_IMPOSTOR_BAKE_RENDERING:
        /* Submit the atlas views + blit-to-staging + bgfx_read_texture THIS
           frame.  bgfx_read_texture returns the frame index after which the CPU
           pixels are valid; we poll for it in READBACK. */
        bake_render_views();
        g_bake.status = JCE_IMPOSTOR_BAKE_READBACK;
        return g_bake.status;

    case JCE_IMPOSTOR_BAKE_READBACK:
        /* Wait until bgfx has produced the read-back (current frame index has
           reached the frame bgfx_read_texture promised). */
        if (g_bake.desc.renderer) {
            uint32_t fi = jce_renderer_get_frame_index(g_bake.desc.renderer);
            if (fi < g_bake.ready_frame) return g_bake.status;
        }
        {
            bool ok = bake_write_atlas_png();
            if (ok) {
                JceImpostorMeta meta;
                memset(&meta, 0, sizeof meta);
                meta.grid_n  = g_bake.grid_n;
                meta.cell_px = g_bake.cell_px;
                meta.center[0] = g_bake.center.x;
                meta.center[1] = g_bake.center.y;
                meta.center[2] = g_bake.center.z;
                meta.radius  = g_bake.radius;
                snprintf(meta.atlas_path, sizeof meta.atlas_path, "%s",
                         g_bake.desc.atlas_path_rel[0] ? g_bake.desc.atlas_path_rel
                                                       : g_bake.desc.atlas_path_host);
                ok = jce_impostor_meta_write(g_bake.desc.meta_path_host, &meta);
            }
            if (g_bake.pixels) { JCE_FREE(g_bake.pixels); g_bake.pixels = NULL; }
            bake_destroy_fbo();
            g_bake.status = ok ? JCE_IMPOSTOR_BAKE_DONE : JCE_IMPOSTOR_BAKE_FAILED;
            if (ok) LOG_SUCCESS(LOG_TAG, "bake done: %s (+ %s)",
                                g_bake.desc.atlas_path_host, g_bake.desc.meta_path_host);
        }
        return g_bake.status;

    default:
        return g_bake.status;
    }
}

float jce_impostor_bake_progress(void)
{
    switch (g_bake.status) {
    case JCE_IMPOSTOR_BAKE_RENDERING: return 0.4f;
    case JCE_IMPOSTOR_BAKE_READBACK:  return 0.8f;
    case JCE_IMPOSTOR_BAKE_DONE:      return 1.0f;
    default:                          return 0.0f;
    }
}

void jce_impostor_shutdown(void)
{
    /* NEVER trust zero-initialized bgfx handles: idx==0 is a VALID handle
     * (someone ELSE's resource).  In a session that never ran an impostor
     * bake, g_bake's statics were all-zero, so this shutdown destroyed frame
     * buffer 0 + texture 0; likewise an un-inited g_gfx destroyed vertex/
     * index buffer 0, program 0 and THREE uniform 0s — silently corrupting
     * bgfx's handle refcounts in release ("Destroying already destroyed
     * uniform" with debug asserts) and, layout-dependent, the heap next to
     * bgfx's internal allocations (the benchmark exit-crash: ShaderRef name
     * freed with a stomped pointer inside bgfx::shutdown).  Gate every
     * destroy on the module's lazy-init latches. */
    if (g_bake.fbo.idx != 0 || g_bake.staging.idx != 0 || g_bake.pixels) {
        bake_destroy_fbo();
        if (g_bake.pixels) { JCE_FREE(g_bake.pixels); g_bake.pixels = NULL; }
    }
    if (g_gfx.init_tried) {
        if (BGFX_HANDLE_IS_VALID(g_gfx.quad_vb)) bgfx_destroy_vertex_buffer(g_gfx.quad_vb);
        if (BGFX_HANDLE_IS_VALID(g_gfx.quad_ib)) bgfx_destroy_index_buffer(g_gfx.quad_ib);
        if (BGFX_HANDLE_IS_VALID(g_gfx.prog))    bgfx_destroy_program(g_gfx.prog);
        if (BGFX_HANDLE_IS_VALID(g_gfx.u_atlas)) bgfx_destroy_uniform(g_gfx.u_atlas);
        if (BGFX_HANDLE_IS_VALID(g_gfx.u_lightDir)) bgfx_destroy_uniform(g_gfx.u_lightDir);
        if (BGFX_HANDLE_IS_VALID(g_gfx.u_lightColor)) bgfx_destroy_uniform(g_gfx.u_lightColor);
    }
    memset(&g_gfx, 0, sizeof g_gfx);
    g_gfx.prog.idx = UINT16_MAX;
}
