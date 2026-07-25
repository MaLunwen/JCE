/*
 * jce_gi_probes.c  GI L1 — dynamic irradiance probe grid implementation.
 *
 * See jce_gi_probes.h.  GPU side: cs_gi_gather writes an rgba32f atlas
 * (x = sx + sz*GX, y = sy*GIG_ROWS + row; rows 0..8 SH9 rgb, row 9 meta
 * {weight, cell.xyz}, row 10 sky-visibility (L2)).  CPU side: blitted to a
 * READ_BACK
 * staging texture and bgfx_read_texture'd on a rolling 2-slot ring; decoded
 * probes serve trilinear SH9 queries with per-probe validity (a probe whose
 * stored world cell doesn't match the queried cell counts as empty — the
 * toroidal grid may have scrolled between kick and readback).
 */

#include <jce/renderer/jce_gi_probes.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"
#include "renderer/jce_shader_load.h"   /* shared backend suffix */

#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_shaders.h>   /* embedded engine pak fallback */

#include <bgfx/c99/bgfx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "jce_gi_probes"

/* Grid: 12 x 6 x 12 probes, 4 m spacing -> 48 x 24 x 48 m around the
 * camera.  Atlas 144 x 60 rgba32f = 138 KB; readback of that size per frame
 * is well inside the WINCAP-proven budget. */
#define GIG_X        12
#define GIG_Y        6
#define GIG_Z        12
#define GIG_COUNT    (GIG_X * GIG_Y * GIG_Z)
#define GIG_SPACING  4.0f
#define GIG_ROWS     11    /* 9 SH + meta + sky-visibility (GI L2) */
#define GIG_AW       (GIG_X * GIG_Z)
#define GIG_AH       (GIG_Y * GIG_ROWS)
#define GIG_SAMPLES  16.0f
#define GIG_RADIUS_PX 96.0f
#define GIG_GATHER_D (3.0f * GIG_SPACING)
#define GIG_ALPHA    0.08f     /* temporal hysteresis */
#define GIG_SKY      0.35f     /* sky sample weight   */
#define GIG_RING     2         /* readback staging ring */
#define GIG_LATENCY  3         /* consume a kick this many updates later */

typedef struct {
    float sh[9][3];
    float weight;
    float cell[3];
    bool  valid;
} GigProbe;

struct JceGiProbes {
    bgfx_program_handle_t  program;
    bgfx_texture_handle_t  atlas;
    bgfx_texture_handle_t  staging[GIG_RING];
    uint8_t               *pixels[GIG_RING];   /* GIG_AW*GIG_AH*16 bytes */
    int                    kicks[GIG_RING];    /* update index of the kick, -1 idle */
    int                    update_ix;

    bgfx_uniform_handle_t  u_grid, u_dims, u_cam, u_screen, u_misc, u_vp, u_ivp;
    bgfx_uniform_handle_t  u_sky;
    bgfx_uniform_handle_t  u_sun, u_suncol, u_csmvp;
    bgfx_uniform_handle_t  s_csm;
    /* GI L3: PRIVATE copy of the sampled CSM cascade.  The gather must
     * never bind the LIVE cascade (it is that frame's depth attachment —
     * binding it to compute disturbs the shadow chain on D3D11, measured
     * as a scene-wide darkening).  We blit the cascade AFTER the dispatch
     * each frame and sample the copy (previous frame's shadows) with the
     * previous frame's light VP — the same one-frame-late phase as the
     * color/depth/VP inputs. */
    bgfx_texture_handle_t  csm_copy;
    uint16_t               csm_copy_size;
    uint32_t               csm_copy_fmt;   /* bgfx_texture_format_t of csm_copy */
    float                  csm_prev_vp[16];
    bool                   csm_prev_valid;
    bgfx_uniform_handle_t  s_color, s_depth;

    GigProbe               probes[GIG_COUNT];  /* decoded CPU copy */
    bool                   have_data;
    uint32_t               frame;
};

/* Shader loading — jce_gpu_scene.c pattern (engine-pak fallback rule).
 * Kept local rather than routed through jce_shader_load_from_pak(): a missing
 * GI shader is a WARN here (the probe grid is optional), not an ERROR. */
static bgfx_program_handle_t gig_load(const JcePakArchive *pak, const char *name)
{
    bgfx_program_handle_t invalid = { UINT16_MAX };
    const char *sfx = jce_shader_backend_suffix();
    if (!sfx) return invalid;
    char path[256];
    snprintf(path, sizeof path, "shaders/%s_%s.bin", name, sfx);
    const JcePakAsset *asset = pak ? jce_pak_find(pak, path) : NULL;
    if (!asset) {
        const JcePakArchive *fb = jce_shaders_embedded_engine_pak();
        if (fb && fb != pak) asset = jce_pak_find(fb, path);
    }
    if (!asset) { LOG_WARN(LOG_TAG, "shader not in pak: %s", path); return invalid; }
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return invalid;
    if (jce_pak_decompress(asset, buf, (size_t)asset->original_size) == 0) {
        JCE_FREE(buf); return invalid;
    }
    const bgfx_memory_t *mem = bgfx_copy(buf, (uint32_t)asset->original_size);
    JCE_FREE(buf);
    bgfx_shader_handle_t cs = bgfx_create_shader(mem);
    if (cs.idx == UINT16_MAX) return invalid;
    return bgfx_create_compute_program(cs, true);
}

JceGiProbes *jce_gi_probes_create(const JcePakArchive *pak)
{
    const bgfx_caps_t *caps = bgfx_get_caps();
    if (!caps || !(caps->supported & BGFX_CAPS_COMPUTE)) return NULL;

    JceGiProbes *gi = (JceGiProbes *)JCE_CALLOC(1, sizeof *gi);
    if (!gi) return NULL;
    gi->program.idx  = UINT16_MAX;
    gi->atlas.idx    = UINT16_MAX;
    gi->csm_copy.idx = UINT16_MAX;
    for (int i = 0; i < GIG_RING; ++i) {
        gi->staging[i].idx = UINT16_MAX;
        gi->kicks[i] = -1;
    }

    /* Uniforms BEFORE the program (GL uniform-order contract). */
    gi->u_grid   = bgfx_create_uniform("u_gig_grid",   BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_dims   = bgfx_create_uniform("u_gig_dims",   BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_cam    = bgfx_create_uniform("u_gig_cam",    BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_screen = bgfx_create_uniform("u_gig_screen", BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_misc   = bgfx_create_uniform("u_gig_misc",   BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_vp     = bgfx_create_uniform("u_gig_vp",     BGFX_UNIFORM_TYPE_MAT4, 1);
    gi->u_ivp    = bgfx_create_uniform("u_gig_ivp",    BGFX_UNIFORM_TYPE_MAT4, 1);
    gi->u_sky    = bgfx_create_uniform("u_gig_sky",    BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_sun    = bgfx_create_uniform("u_gig_sun",    BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_suncol = bgfx_create_uniform("u_gig_suncol", BGFX_UNIFORM_TYPE_VEC4, 1);
    gi->u_csmvp  = bgfx_create_uniform("u_gig_csm",    BGFX_UNIFORM_TYPE_MAT4, 1);
    gi->s_csm    = bgfx_create_uniform("s_gigCsm",     BGFX_UNIFORM_TYPE_SAMPLER, 1);
    gi->s_color  = bgfx_create_uniform("s_gigColor",   BGFX_UNIFORM_TYPE_SAMPLER, 1);
    gi->s_depth  = bgfx_create_uniform("s_gigDepth",   BGFX_UNIFORM_TYPE_SAMPLER, 1);

    gi->program = gig_load(pak, "cs_gi_gather");
    if (gi->program.idx == UINT16_MAX) {
        jce_gi_probes_destroy(gi);
        return NULL;
    }

    gi->atlas = bgfx_create_texture_2d(GIG_AW, GIG_AH, false, 1,
        BGFX_TEXTURE_FORMAT_RGBA32F, BGFX_TEXTURE_COMPUTE_WRITE, NULL, 0);
    for (int i = 0; i < GIG_RING; ++i) {
        gi->staging[i] = bgfx_create_texture_2d(GIG_AW, GIG_AH, false, 1,
            BGFX_TEXTURE_FORMAT_RGBA32F,
            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK, NULL, 0);
        gi->pixels[i] = (uint8_t *)JCE_CALLOC(1, (size_t)GIG_AW * GIG_AH * 16u);
    }
    if (gi->atlas.idx == UINT16_MAX ||
        gi->staging[0].idx == UINT16_MAX || gi->staging[1].idx == UINT16_MAX ||
        !gi->pixels[0] || !gi->pixels[1]) {
        jce_gi_probes_destroy(gi);
        return NULL;
    }
    LOG_INFO(LOG_TAG, "dynamic GI probes online: %dx%dx%d grid, %.0fm spacing "
             "(SH9 via the baked-GI funnel)", GIG_X, GIG_Y, GIG_Z, GIG_SPACING);
    return gi;
}

void jce_gi_probes_destroy(JceGiProbes *gi)
{
    if (!gi) return;
    if (gi->program.idx != UINT16_MAX) bgfx_destroy_program(gi->program);
    if (gi->atlas.idx   != UINT16_MAX) bgfx_destroy_texture(gi->atlas);
    for (int i = 0; i < GIG_RING; ++i) {
        if (gi->staging[i].idx != UINT16_MAX) bgfx_destroy_texture(gi->staging[i]);
        if (gi->pixels[i]) JCE_FREE(gi->pixels[i]);
    }
    if (gi->u_grid.idx   != UINT16_MAX) bgfx_destroy_uniform(gi->u_grid);
    if (gi->u_dims.idx   != UINT16_MAX) bgfx_destroy_uniform(gi->u_dims);
    if (gi->u_cam.idx    != UINT16_MAX) bgfx_destroy_uniform(gi->u_cam);
    if (gi->u_screen.idx != UINT16_MAX) bgfx_destroy_uniform(gi->u_screen);
    if (gi->u_misc.idx   != UINT16_MAX) bgfx_destroy_uniform(gi->u_misc);
    if (gi->u_vp.idx     != UINT16_MAX) bgfx_destroy_uniform(gi->u_vp);
    if (gi->u_ivp.idx    != UINT16_MAX) bgfx_destroy_uniform(gi->u_ivp);
    if (gi->u_sky.idx    != UINT16_MAX) bgfx_destroy_uniform(gi->u_sky);
    if (gi->u_sun.idx    != UINT16_MAX) bgfx_destroy_uniform(gi->u_sun);
    if (gi->u_suncol.idx != UINT16_MAX) bgfx_destroy_uniform(gi->u_suncol);
    if (gi->u_csmvp.idx  != UINT16_MAX) bgfx_destroy_uniform(gi->u_csmvp);
    if (gi->s_csm.idx    != UINT16_MAX) bgfx_destroy_uniform(gi->s_csm);
    if (gi->csm_copy.idx != UINT16_MAX) bgfx_destroy_texture(gi->csm_copy);
    if (gi->s_color.idx  != UINT16_MAX) bgfx_destroy_uniform(gi->s_color);
    if (gi->s_depth.idx  != UINT16_MAX) bgfx_destroy_uniform(gi->s_depth);
    JCE_FREE(gi);
}

static void gig_origin_cell(jce_vec3 cam, float out_cell[3])
{
    out_cell[0] = floorf(cam.x / GIG_SPACING) - GIG_X / 2;
    out_cell[1] = floorf(cam.y / GIG_SPACING) - GIG_Y / 2;
    out_cell[2] = floorf(cam.z / GIG_SPACING) - GIG_Z / 2;
}

static void gig_decode(JceGiProbes *gi, const uint8_t *px)
{
    const float *img = (const float *)px;
    for (int sy = 0; sy < GIG_Y; ++sy) {
        for (int sz = 0; sz < GIG_Z; ++sz) {
            for (int sx = 0; sx < GIG_X; ++sx) {
                GigProbe *pr = &gi->probes[sx + sz * GIG_X +
                                           sy * GIG_X * GIG_Z];
                int ax = sx + sz * GIG_X;
                const float *meta =
                    img + ((size_t)(sy * GIG_ROWS + 9) * GIG_AW + ax) * 4u;
                pr->weight  = meta[0];
                pr->cell[0] = meta[1];
                pr->cell[1] = meta[2];
                pr->cell[2] = meta[3];
                pr->valid   = pr->weight > 0.02f;
                for (int c = 0; c < 9; ++c) {
                    const float *t =
                        img + ((size_t)(sy * GIG_ROWS + c) * GIG_AW + ax) * 4u;
                    pr->sh[c][0] = t[0];
                    pr->sh[c][1] = t[1];
                    pr->sh[c][2] = t[2];
                }
            }
        }
    }
    gi->have_data = true;
}

void jce_gi_probes_update(JceGiProbes *gi,
                          uint16_t compute_view, uint16_t blit_view,
                          uint16_t color_tex, uint16_t depth_tex,
                          const float prev_vp[16], jce_vec3 cam_pos,
                          uint32_t vp_w, uint32_t vp_h, bool gl_ndc,
                          jce_vec3 sky_color, float sky_amount,
                          uint16_t sun_csm_tex, uint16_t sun_csm_size,
                          uint32_t sun_csm_fmt, const float *sun_csm_vp,
                          jce_vec3 sun_dir, jce_vec3 sun_color,
                          float sun_amount)
{
    if (!gi || color_tex == UINT16_MAX || depth_tex == UINT16_MAX || !prev_vp)
        return;

    /* Consume the ring slot whose kick is old enough (data guaranteed). */
    for (int i = 0; i < GIG_RING; ++i) {
        if (gi->kicks[i] >= 0 &&
            gi->update_ix - gi->kicks[i] >= GIG_LATENCY) {
            gig_decode(gi, gi->pixels[i]);
            gi->kicks[i] = -1;
        }
    }

    /* Dispatch the gather. */
    jce_mat4 vp;
    memcpy(vp.raw, prev_vp, sizeof vp.raw);
    jce_mat4 ivp = jce_m4_inverse(&vp);

    float cell[3];
    gig_origin_cell(cam_pos, cell);
    float grid4[4]   = { cell[0], cell[1], cell[2], GIG_SPACING };
    float dims4[4]   = { (float)GIG_X, (float)GIG_Y, (float)GIG_Z,
                         (float)GIG_COUNT };
    float cam4[4]    = { cam_pos.x, cam_pos.y, cam_pos.z, GIG_ALPHA };
    float screen4[4] = { (float)vp_w, (float)vp_h, GIG_SAMPLES, GIG_RADIUS_PX };
    float misc4[4]   = { (float)(gi->frame++ & 1023u), GIG_SKY, GIG_GATHER_D,
                         gl_ndc ? 1.0f : 0.0f };
    bgfx_set_uniform(gi->u_grid,   grid4,   1);
    bgfx_set_uniform(gi->u_dims,   dims4,   1);
    bgfx_set_uniform(gi->u_cam,    cam4,    1);
    bgfx_set_uniform(gi->u_screen, screen4, 1);
    bgfx_set_uniform(gi->u_misc,   misc4,   1);
    bgfx_set_uniform(gi->u_vp,  vp.raw[0],  1);
    bgfx_set_uniform(gi->u_ivp, ivp.raw[0], 1);
    /* JCE_GI_SKY overrides the sky-floor amount (0 disables the L2 term). */
    static float s_sky_env = -2.0f;
    if (s_sky_env < -1.5f) {
        const char *v = getenv("JCE_GI_SKY");
        s_sky_env = (v && v[0]) ? (float)atof(v) : -1.0f;
    }
    float sky4[4] = { sky_color.x, sky_color.y, sky_color.z,
                      (s_sky_env >= 0.0f) ? s_sky_env : sky_amount };
    bgfx_set_uniform(gi->u_sky, sky4, 1);

    /* GI L3 sun-bounce inputs (JCE_GI_SUN overrides; 0 disables). */
    static float s_sun_env = -2.0f;
    if (s_sun_env < -1.5f) {
        const char *v = getenv("JCE_GI_SUN");
        s_sun_env = (v && v[0]) ? (float)atof(v) : -1.0f;
    }
    float sun_amt = (s_sun_env >= 0.0f) ? s_sun_env : sun_amount;
    bool sun_in = (sun_csm_tex != UINT16_MAX) && sun_csm_vp && sun_amt > 0.0f &&
                  sun_csm_size > 0;
    /* Sample the PRIVATE COPY from last frame (never the live cascade). */
    bool sun_ready = sun_in && gi->csm_prev_valid &&
                     gi->csm_copy.idx != UINT16_MAX;
    /* GI L4 multi-bounce gain (JCE_GI_BOUNCE; 0 = off).  Rides u_gig_suncol.w
     * (the sun-colour vec4's spare lane — no new uniform).  Clamp < 1 so the
     * normalised neighbour feedback fixed point SH=gather/(1-gain) converges. */
    static float s_bounce_env = -2.0f;
    if (s_bounce_env < -1.5f) {
        const char *v = getenv("JCE_GI_BOUNCE");
        s_bounce_env = (v && v[0]) ? (float)atof(v) : 0.0f;   /* opt-in */
        if (s_bounce_env < 0.0f) s_bounce_env = 0.0f;
        if (s_bounce_env > 0.9f) s_bounce_env = 0.9f;
    }
    float sun4[4]  = { sun_dir.x, sun_dir.y, sun_dir.z,
                       sun_ready ? sun_amt : 0.0f };
    float sunc4[4] = { sun_color.x, sun_color.y, sun_color.z, s_bounce_env };
    bgfx_set_uniform(gi->u_sun,    sun4,  1);
    bgfx_set_uniform(gi->u_suncol, sunc4, 1);
    if (sun_ready) {
        bgfx_set_uniform(gi->u_csmvp, gi->csm_prev_vp, 1);
        bgfx_set_texture(3, gi->s_csm, gi->csm_copy,
                         BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                         BGFX_SAMPLER_MIP_POINT);
    }
    bgfx_texture_handle_t ct = { color_tex }, dt = { depth_tex };
    bgfx_set_texture(0, gi->s_color, ct, UINT32_MAX);
    bgfx_set_texture(1, gi->s_depth, dt,
                     BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
                     BGFX_SAMPLER_MIP_POINT);
    bgfx_set_image(2, gi->atlas, 0, BGFX_ACCESS_READWRITE,
                   BGFX_TEXTURE_FORMAT_RGBA32F);
    bgfx_dispatch(compute_view, gi->program,
                  (GIG_COUNT + 63u) / 64u, 1, 1, BGFX_DISCARD_ALL);

    /* GI L3: refresh the private cascade copy for NEXT frame (the blit
     * view runs after this frame's shadow pass, so the copy holds this
     * frame's shadows; the paired VP is saved alongside).  Lazily (re)size
     * to the live cascade — bgfx blit requires equal formats/sizes. */
    if (sun_in) {
        /* The private copy must EXACTLY match the live cascade: bgfx_blit does
         * NOT clamp or convert — a size or format mismatch is an ILLEGAL copy
         * (on D3D12 CopyTextureRegion out-of-bounds / out-of-family removes
         * the device with DXGI_ERROR_INVALID_CALL; D3D11/GL/VK silently drop
         * it).  The caller passes the cascade's real size + depth format
         * (tier-dependent: 512..4096, D16/D24S8/D32F), and the copy is
         * recreated whenever either changes (quality switch at runtime). */
        if (gi->csm_copy.idx != UINT16_MAX &&
            (gi->csm_copy_size != sun_csm_size ||
             gi->csm_copy_fmt  != sun_csm_fmt)) {
            bgfx_destroy_texture(gi->csm_copy);
            gi->csm_copy.idx = UINT16_MAX;
            gi->csm_prev_valid = false;
        }
        if (gi->csm_copy.idx == UINT16_MAX) {
            gi->csm_copy = bgfx_create_texture_2d(sun_csm_size, sun_csm_size,
                false, 1,
                (bgfx_texture_format_t)sun_csm_fmt,
                BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_MIN_POINT |
                BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT |
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, NULL, 0);
            gi->csm_copy_size = sun_csm_size;
            gi->csm_copy_fmt  = sun_csm_fmt;
        }
        if (gi->csm_copy.idx != UINT16_MAX) {
            bgfx_texture_handle_t live = { sun_csm_tex };
            bgfx_blit(blit_view, gi->csm_copy, 0, 0, 0, 0,
                      live, 0, 0, 0, 0, sun_csm_size, sun_csm_size, 1);
            memcpy(gi->csm_prev_vp, sun_csm_vp, sizeof gi->csm_prev_vp);
            gi->csm_prev_valid = true;
        }
    } else {
        gi->csm_prev_valid = false;
    }

    /* Kick a rolling readback into a free ring slot.  bgfx read_texture
     * MAPs the staging texture immediately on D3D11 — a hard pipeline sync
     * (~10ms under load), unavoidable at the bgfx API level — so the CPU
     * snapshot refreshes on a long cadence (default every 16 updates,
     * JCE_GI_READBACK overrides; the GPU-side hysteresis keeps integrating
     * every frame regardless, the CPU just samples the running average). */
    static int s_cadence = -1;
    if (s_cadence < 0) {
        const char *v = getenv("JCE_GI_READBACK");
        s_cadence = (v && v[0]) ? atoi(v) : 16;
        if (s_cadence < 1) s_cadence = 1;
    }
    for (int i = 0; (gi->update_ix % s_cadence) == 0 && i < GIG_RING; ++i) {
        if (gi->kicks[i] < 0) {
            bgfx_blit(blit_view, gi->staging[i], 0, 0, 0, 0,
                      gi->atlas, 0, 0, 0, 0, GIG_AW, GIG_AH, 1);
            bgfx_read_texture(gi->staging[i], gi->pixels[i], 0, 0);
            gi->kicks[i] = gi->update_ix;
            break;
        }
    }
    gi->update_ix++;
}

float jce_gi_probes_sample_sh9(const JceGiProbes *gi, jce_vec3 pos,
                               float out_sh9[9][3])
{
    if (out_sh9) memset(out_sh9, 0, sizeof(float) * 27);
    if (!gi || !gi->have_data || !out_sh9) return 0.0f;

    /* Cell-space trilinear across the 8 surrounding probes (probe centres
     * sit at cell+0.5, so sampling in "pos/spacing - 0.5" space).  Each
     * corner contributes only when its slot really holds that world cell
     * (toroidal scroll may have retired it). */
    float fx = pos.x / GIG_SPACING - 0.5f;
    float fy = pos.y / GIG_SPACING - 0.5f;
    float fz = pos.z / GIG_SPACING - 0.5f;
    float bx = floorf(fx), by = floorf(fy), bz = floorf(fz);
    float tx = fx - bx, ty = fy - by, tz = fz - bz;

    float wsum = 0.0f;
    for (int i = 0; i < 8; ++i) {
        float cx = bx + (float)(i & 1);
        float cy = by + (float)((i >> 1) & 1);
        float cz = bz + (float)((i >> 2) & 1);
        int sx = ((int)cx % GIG_X + GIG_X) % GIG_X;
        int sy = ((int)cy % GIG_Y + GIG_Y) % GIG_Y;
        int sz = ((int)cz % GIG_Z + GIG_Z) % GIG_Z;
        const GigProbe *pr = &gi->probes[sx + sz * GIG_X + sy * GIG_X * GIG_Z];
        if (!pr->valid) continue;
        if (fabsf(pr->cell[0] - cx) > 0.5f || fabsf(pr->cell[1] - cy) > 0.5f ||
            fabsf(pr->cell[2] - cz) > 0.5f) continue;
        float w = ((i & 1) ? tx : 1.0f - tx) *
                  (((i >> 1) & 1) ? ty : 1.0f - ty) *
                  (((i >> 2) & 1) ? tz : 1.0f - tz);
        w *= pr->weight;
        if (w <= 0.0f) continue;
        for (int c = 0; c < 9; ++c) {
            out_sh9[c][0] += pr->sh[c][0] * w;
            out_sh9[c][1] += pr->sh[c][1] * w;
            out_sh9[c][2] += pr->sh[c][2] * w;
        }
        wsum += w;
    }
    if (wsum > 1.0e-4f) {
        float inv = 1.0f / wsum;
        for (int c = 0; c < 9; ++c) {
            out_sh9[c][0] *= inv;
            out_sh9[c][1] *= inv;
            out_sh9[c][2] *= inv;
        }
    }
    return wsum > 1.0f ? 1.0f : wsum;
}
