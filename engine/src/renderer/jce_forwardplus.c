/*
 * jce_forwardplus.c  Forward+ clustered lighting — GPU upload (ROUND A).
 *
 * See engine/include/jce/renderer/jce_forwardplus.h for the full GPU layout
 * contract.  ROUND A: build + pack + upload only; no shader reads the data
 * yet, so rendering is byte-identical to the brute-force path.
 *
 * The packing math (jce_forwardplus_pack and the alloc/free helpers) is pure
 * and bgfx-free so it can be unit-tested headlessly.  Only the stateful
 * JceForwardPlus wrapper touches bgfx.
 */

#include <jce/os/core/jce_log.h>
#include <jce/renderer/jce_forwardplus.h>

#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <string.h>
#include "renderer/jce_render_encoder.h"

#define LOG_TAG "forwardplus"

/* ================================================================== */
/* Pure packing helpers (NO bgfx — unit-testable headless)             */
/* ================================================================== */

static uint32_t ceil_div_u32(uint32_t a, uint32_t b)
{
    return b ? (a + b - 1u) / b : 0u;
}

bool jce_forwardplus_packed_alloc(JceForwardPlusPacked *out,
                                  uint32_t cells_x,
                                  uint32_t cells_y,
                                  uint32_t slices_z,
                                  uint32_t max_per_cell,
                                  uint32_t light_count)
{
    if (!out || !cells_x || !cells_y || !slices_z || !max_per_cell)
        return false;

    memset(out, 0, sizeof(*out));

    uint32_t ncell = cells_x * cells_y * slices_z;
    uint32_t maxidx = ncell * max_per_cell;
    uint32_t ptex = light_count * JCE_FP_TEXELS_PER_LIGHT;

    out->grid   = (float *)JCE_CALLOC((size_t)ncell * 4u, sizeof(float));
    out->index  = (float *)JCE_CALLOC((size_t)maxidx * 4u, sizeof(float));
    /* Allocate at least one texel for params so a zero-light frame still has
     * a valid (1x1) buffer to upload — avoids a degenerate 0-size texture. */
    uint32_t ptex_alloc = ptex ? ptex : 1u;
    out->params = (float *)JCE_CALLOC((size_t)ptex_alloc * 4u, sizeof(float));

    if (!out->grid || !out->index || !out->params) {
        jce_forwardplus_packed_free(out);
        return false;
    }

    out->grid_texels  = ncell;
    out->index_texels = maxidx;
    out->index_used   = 0;
    out->param_texels = ptex;
    out->light_count  = light_count;
    out->cells_x      = cells_x;
    out->cells_y      = cells_y;
    out->slices_z     = slices_z;
    out->max_per_cell = max_per_cell;
    out->z_near       = 0.0f;
    out->z_far        = 0.0f;
    return true;
}

void jce_forwardplus_packed_free(JceForwardPlusPacked *p)
{
    if (!p) return;
    JCE_FREE(p->grid);
    JCE_FREE(p->index);
    JCE_FREE(p->params);
    p->grid = p->index = p->params = NULL;
    p->grid_texels = p->index_texels = p->index_used = 0;
    p->param_texels = p->light_count = 0;
}

bool jce_forwardplus_pack(JceForwardPlusPacked            *out,
                          const JceLightClusterResult     *result,
                          const JceLightProxy             *lights,
                          const JceForwardPlusLightParam  *params,
                          uint32_t                         light_count,
                          float                            z_near,
                          float                            z_far)
{
    if (!out || !result) return false;
    if (light_count > 0 && (!lights || !params)) return false;

    /* Shape must match what was allocated. */
    if (out->cells_x != result->cells_x ||
        out->cells_y != result->cells_y ||
        out->slices_z != result->slices_z ||
        out->max_per_cell != result->max_per_cell ||
        out->light_count != light_count) {
        return false;
    }

    uint32_t ncell = result->cells_x * result->cells_y * result->slices_z;

    /* Reset to zero so unused tails are deterministic. */
    memset(out->grid,  0, (size_t)out->grid_texels  * 4u * sizeof(float));
    memset(out->index, 0, (size_t)out->index_texels * 4u * sizeof(float));
    if (out->param_texels)
        memset(out->params, 0, (size_t)out->param_texels * 4u * sizeof(float));

    /* ── Grid + index list ────────────────────────────────────────────
     * Each froxel `c` gets a contiguous run in the flat index list.  We
     * use the SAME packing the cluster module uses internally:
     *   element base for froxel c = c * max_per_cell
     * so the run for c is [c*max_per_cell .. c*max_per_cell + count).  The
     * grid texel stores (offset, count).  This keeps offsets monotone and
     * never overlapping, exactly as the GPU will read them. */
    uint32_t total_assign = 0;
    for (uint32_t c = 0; c < ncell; ++c) {
        uint32_t count  = result->cell_counts[c];
        if (count > result->max_per_cell) count = result->max_per_cell;
        uint32_t offset = c * result->max_per_cell;

        out->grid[c * 4u + 0u] = (float)offset;
        out->grid[c * 4u + 1u] = (float)count;
        out->grid[c * 4u + 2u] = 0.0f;
        out->grid[c * 4u + 3u] = 0.0f;

        const uint32_t *src = &result->cell_indices[(size_t)c * result->max_per_cell];
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t e = offset + k;
            out->index[e * 4u + 0u] = (float)src[k];   /* light index */
            out->index[e * 4u + 1u] = 0.0f;
            out->index[e * 4u + 2u] = 0.0f;
            out->index[e * 4u + 3u] = 0.0f;
            total_assign++;
        }
    }
    out->index_used = total_assign;

    /* ── Light params (4 texels per light) ────────────────────────────── */
    for (uint32_t li = 0; li < light_count; ++li) {
        const JceLightProxy            *g = &lights[li];
        const JceForwardPlusLightParam *p = &params[li];
        float *t = out->params + (size_t)li * JCE_FP_TEXELS_PER_LIGHT * 4u;

        /* texel 0: pos.xyz, range */
        t[0]  = g->position_ws.x;
        t[1]  = g->position_ws.y;
        t[2]  = g->position_ws.z;
        t[3]  = g->radius;
        /* texel 1: color.rgb, intensity */
        t[4]  = p->color.x;
        t[5]  = p->color.y;
        t[6]  = p->color.z;
        t[7]  = p->intensity;
        /* texel 2: type, spotDir.xyz */
        t[8]  = p->type;
        t[9]  = p->spot_dir.x;
        t[10] = p->spot_dir.y;
        t[11] = p->spot_dir.z;
        /* texel 3: innerCos, outerCos, maskLo, maskHi.
         *
         * The two lanes that were 0 now carry this light's rendering-layer
         * mask, which is what lets the clustered path honour layers at all:
         * the cluster is built once per frame with no receiver in sight, so
         * the mask has to travel WITH the light and be tested against the
         * receiver in the shader.
         *
         * SPLIT IN HALF because a 32-bit mask does not survive a float: 2^31
         * needs a 32-bit mantissa and there are 24.  Each half is an integer
         * below 65536, which is exact, and so is the floor(m / 2^b) the
         * shader's bit test does. */
        t[12] = p->inner_cone_cos;
        t[13] = p->outer_cone_cos;
        t[14] = (float)(p->layer_mask & 0xFFFFu);
        t[15] = (float)((p->layer_mask >> 16) & 0xFFFFu);
    }

    out->z_near = z_near;
    out->z_far  = z_far;
    return true;
}

/* ── Combine the three regions into ONE contiguous W-wide RGBA32F buffer ──
 * Layout: GRID rows first, then INDEX rows, then LIGHTS rows.  Each region
 * is laid out row-major at full W width; its rows are placed starting at the
 * region's base row.  Within a region, element e keeps its (e % W, e / W)
 * addressing, so absolute texel row = regionBaseRow + e/W.  The combined
 * buffer is zero-initialized so any padding texels (the tail of a region's
 * last partial row, and the never-written index capacity) are deterministic
 * zeros — bit-identical to what the separate-texture uploads produced. */
bool jce_forwardplus_pack_combined(JceForwardPlusCombined     *out,
                                   const JceForwardPlusPacked *src)
{
    if (!out || !src) return false;
    if (!src->grid || !src->index || !src->params) return false;

    memset(out, 0, sizeof(*out));

    const uint32_t W = JCE_FP_TEX_WIDTH;

    uint32_t grid_rows   = ceil_div_u32(src->grid_texels,  W);
    uint32_t index_rows  = ceil_div_u32(src->index_texels, W);
    /* Mirror fp_gpu_init: the lights region is allocated at least one texel
     * (so a zero-light frame still has a valid row) — at least one row. */
    uint32_t param_texels = src->param_texels ? src->param_texels : 1u;
    uint32_t lights_rows = ceil_div_u32(param_texels, W);
    if (grid_rows   == 0) grid_rows   = 1;
    if (index_rows  == 0) index_rows  = 1;
    if (lights_rows == 0) lights_rows = 1;

    uint32_t total_rows = grid_rows + index_rows + lights_rows;

    float *data = (float *)JCE_CALLOC((size_t)W * total_rows * 4u, sizeof(float));
    if (!data) return false;

    /* GRID region — base row 0. */
    if (src->grid_texels) {
        memcpy(data,
               src->grid,
               (size_t)src->grid_texels * 4u * sizeof(float));
    }
    /* INDEX region — base row = grid_rows. */
    uint32_t index_base_row = grid_rows;
    if (src->index_texels) {
        memcpy(data + (size_t)index_base_row * W * 4u,
               src->index,
               (size_t)src->index_texels * 4u * sizeof(float));
    }
    /* LIGHTS region — base row = grid_rows + index_rows. */
    uint32_t lights_base_row = grid_rows + index_rows;
    if (src->param_texels) {
        memcpy(data + (size_t)lights_base_row * W * 4u,
               src->params,
               (size_t)src->param_texels * 4u * sizeof(float));
    }

    out->data            = data;
    out->width           = W;
    out->total_rows      = total_rows;
    out->grid_base_row   = 0;
    out->index_base_row  = index_base_row;
    out->lights_base_row = lights_base_row;
    out->grid_rows       = grid_rows;
    out->index_rows      = index_rows;
    out->lights_rows     = lights_rows;
    return true;
}

void jce_forwardplus_combined_free(JceForwardPlusCombined *c)
{
    if (!c) return;
    JCE_FREE(c->data);
    memset(c, 0, sizeof(*c));
}

/* ================================================================== */
/* Stateful GPU wrapper (render-thread; bgfx)                          */
/* ================================================================== */

struct JceForwardPlus {
    bool             enabled;
    uint32_t         cells_x, cells_y, slices_z, max_per_cell, max_lights;

    JceLightCluster *cluster;
    JceForwardPlusPacked packed;
    JceForwardPlusCombined combined;   /* single-texture upload mirror */
    bool             packed_valid;

    /* Scratch proxy buffer (cluster build input) — sized to max_lights. */
    JceLightProxy   *scratch_proxies;

    /* GPU resources (lazy; created on first enabled update).  ONE combined
     * RGBA32F texture (grid ++ index ++ lights regions stacked by rows) so a
     * single sampler stage (s_cluster) covers all three — fs_pbr.sc already
     * owns stages 0..15 and WebGL2/GLES3 caps fragment samplers at 16. */
    bool                  gpu_init;
    uint16_t              tex_w, tex_h;     /* combined texture dims */
    bgfx_texture_handle_t tex_cluster;
    bgfx_uniform_handle_t s_cluster;
    bgfx_uniform_handle_t u_clusterParams;
    bgfx_uniform_handle_t u_clusterParams2;
    bgfx_uniform_handle_t u_clusterRegions;

    /* Last-frame uniform values, cached so jce_forwardplus_bind can replay
     * them on every PBR submit (bgfx clears uniform/texture-stage state
     * between submits).  Valid only when last_upload_ok is true. */
    float                 last_cp[4];
    float                 last_cp2[4];
    float                 last_cr[4];
    bool                  last_upload_ok;
};

JceForwardPlus *jce_forwardplus_create(uint32_t cells_x,
                                       uint32_t cells_y,
                                       uint32_t slices_z,
                                       uint32_t max_per_cell,
                                       uint32_t max_lights)
{
    if (!cells_x || !cells_y || !slices_z || !max_per_cell || !max_lights)
        return NULL;

    JceForwardPlus *fp = (JceForwardPlus *)JCE_CALLOC(1, sizeof(*fp));
    if (!fp) return NULL;

    fp->cells_x      = cells_x;
    fp->cells_y      = cells_y;
    fp->slices_z     = slices_z;
    fp->max_per_cell = max_per_cell;
    fp->max_lights   = max_lights;
    fp->enabled      = false;   /* ROUND A: dormant by default. */

    JceLightClusterDesc d = { cells_x, cells_y, slices_z, max_lights, max_per_cell };
    fp->cluster = jce_light_cluster_create(&d);
    fp->scratch_proxies =
        (JceLightProxy *)JCE_CALLOC(max_lights, sizeof(JceLightProxy));

    if (!fp->cluster || !fp->scratch_proxies ||
        !jce_forwardplus_packed_alloc(&fp->packed, cells_x, cells_y,
                                      slices_z, max_per_cell, max_lights)) {
        jce_forwardplus_destroy(fp);
        return NULL;
    }

    /* Mark all bgfx handles invalid until lazy GPU init. */
    fp->tex_cluster.idx      = UINT16_MAX;
    fp->s_cluster.idx        = UINT16_MAX;
    fp->u_clusterParams.idx  = UINT16_MAX;
    fp->u_clusterParams2.idx = UINT16_MAX;
    fp->u_clusterRegions.idx = UINT16_MAX;

    return fp;
}

void jce_forwardplus_destroy(JceForwardPlus *fp)
{
    if (!fp) return;

    if (fp->gpu_init) {
        if (BGFX_HANDLE_IS_VALID(fp->tex_cluster))      bgfx_destroy_texture(fp->tex_cluster);
        if (BGFX_HANDLE_IS_VALID(fp->s_cluster))        bgfx_destroy_uniform(fp->s_cluster);
        if (BGFX_HANDLE_IS_VALID(fp->u_clusterParams))  bgfx_destroy_uniform(fp->u_clusterParams);
        if (BGFX_HANDLE_IS_VALID(fp->u_clusterParams2)) bgfx_destroy_uniform(fp->u_clusterParams2);
        if (BGFX_HANDLE_IS_VALID(fp->u_clusterRegions)) bgfx_destroy_uniform(fp->u_clusterRegions);
    }

    jce_forwardplus_combined_free(&fp->combined);
    jce_forwardplus_packed_free(&fp->packed);
    jce_light_cluster_destroy(fp->cluster);
    JCE_FREE(fp->scratch_proxies);
    JCE_FREE(fp);
}

void jce_forwardplus_set_enabled(JceForwardPlus *fp, bool enabled)
{
    if (fp) fp->enabled = enabled;
}

bool jce_forwardplus_is_enabled(const JceForwardPlus *fp)
{
    return fp ? fp->enabled : false;
}

const JceForwardPlusPacked *jce_forwardplus_get_packed(const JceForwardPlus *fp)
{
    if (!fp || !fp->packed_valid) return NULL;
    return &fp->packed;
}

/* Lazily create the ONE combined GPU texture + uniforms.  The texture is
 * MUTABLE (NULL mem, BGFX_TEXTURE_NONE) so it can be refreshed each frame
 * with bgfx_update_texture_2d.  Sized for the WORST case (max_lights) so its
 * height never has to grow: the grid + index regions are constant size (they
 * depend only on the froxel grid / index capacity), and the lights region is
 * sized to max_lights here; per-frame uploads cover only the rows actually
 * used for that frame's light count (u_clusterRegions tells the shader the
 * exact region base rows + total height).  Returns false if creation failed. */
static bool fp_gpu_init(JceForwardPlus *fp)
{
    if (fp->gpu_init) return true;

    const uint16_t W = (uint16_t)JCE_FP_TEX_WIDTH;

    /* Worst-case region rows.  grid/index capacity is light-count-independent;
     * lights region is sized to the max light pool so the texture height is a
     * fixed upper bound.  (fp->packed was allocated for max_lights, so
     * grid_texels = NCELL and index_texels = MAXIDX are already the caps.) */
    uint32_t grid_rows   = ceil_div_u32(fp->packed.grid_texels,  JCE_FP_TEX_WIDTH);
    uint32_t index_rows  = ceil_div_u32(fp->packed.index_texels, JCE_FP_TEX_WIDTH);
    uint32_t max_param   = fp->max_lights * JCE_FP_TEXELS_PER_LIGHT;
    uint32_t lights_rows = ceil_div_u32(max_param ? max_param : 1u, JCE_FP_TEX_WIDTH);
    if (grid_rows   == 0) grid_rows   = 1;
    if (index_rows  == 0) index_rows  = 1;
    if (lights_rows == 0) lights_rows = 1;

    uint32_t total_rows = grid_rows + index_rows + lights_rows;

    fp->tex_w = W;
    fp->tex_h = (uint16_t)total_rows;
    if (fp->tex_h == 0) fp->tex_h = 1;

    const uint64_t flags = BGFX_TEXTURE_NONE
                         | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
                         | BGFX_SAMPLER_MIP_POINT
                         | BGFX_SAMPLER_U_CLAMP   | BGFX_SAMPLER_V_CLAMP;

    fp->tex_cluster = bgfx_create_texture_2d(fp->tex_w, fp->tex_h, false, 1,
                                             BGFX_TEXTURE_FORMAT_RGBA32F, flags, NULL, 0);

    fp->s_cluster        = bgfx_create_uniform("s_cluster",        BGFX_UNIFORM_TYPE_SAMPLER, 1);
    fp->u_clusterParams  = bgfx_create_uniform("u_clusterParams",  BGFX_UNIFORM_TYPE_VEC4,    1);
    fp->u_clusterParams2 = bgfx_create_uniform("u_clusterParams2", BGFX_UNIFORM_TYPE_VEC4,    1);
    fp->u_clusterRegions = bgfx_create_uniform("u_clusterRegions", BGFX_UNIFORM_TYPE_VEC4,    1);

    if (!BGFX_HANDLE_IS_VALID(fp->tex_cluster)) {
        LOG_WARN(LOG_TAG, "cluster data texture failed to create — disabling");
        return false;
    }

    fp->gpu_init = true;
    LOG_DEBUG(LOG_TAG, "Forward+ GPU resources created (combined %ux%u: "
              "grid rows %u, index rows %u, lights rows %u)",
              fp->tex_w, fp->tex_h, grid_rows, index_rows, lights_rows);
    return true;
}

/* Upload a combined RGBA32F buffer (grid ++ index ++ lights regions) into the
 * top of the mutable combined texture.  Only the rows the combined buffer
 * actually uses are written; rows beyond combined->total_rows are never
 * sampled (u_clusterRegions.w == total_rows bounds the lights region). */
static void fp_upload_combined(bgfx_texture_handle_t tex, uint16_t tex_w,
                               uint16_t tex_h,
                               const JceForwardPlusCombined *combined)
{
    if (!BGFX_HANDLE_IS_VALID(tex) || !combined || !combined->data) return;

    uint16_t rows = (uint16_t)combined->total_rows;
    if (rows > tex_h) rows = tex_h;   /* clamp to the texture footprint */
    if (rows == 0) return;

    uint32_t bytes = (uint32_t)tex_w * (uint32_t)rows * 4u * (uint32_t)sizeof(float);
    const bgfx_memory_t *mem = bgfx_alloc(bytes);
    memcpy(mem->data, combined->data, bytes);
    /* Pitch = row stride in bytes = width * 4 channels * 4 bytes (RGBA32F).
     * width is JCE_FP_TEX_WIDTH (256) so 256*16 = 4096 fits a uint16_t. */
    uint16_t pitch = (uint16_t)((uint32_t)tex_w * 4u * (uint32_t)sizeof(float));
    bgfx_update_texture_2d(tex, 0, 0, 0, 0, tex_w, rows, mem, pitch);
}

void jce_forwardplus_update(JceForwardPlus                 *fp,
                            const JceLightProxy            *lights,
                            const JceForwardPlusLightParam *params,
                            uint32_t                        light_count,
                            const jce_mat4                 *inv_view,
                            const jce_mat4                 *proj,
                            float                           z_near,
                            float                           z_far,
                            const JceRenderer              *r)
{
    if (!fp || !fp->enabled) return;
    if (!inv_view || !proj) return;
    if (!(z_near > 0.0f) || !(z_far > z_near)) return;

    if (light_count > fp->max_lights) light_count = fp->max_lights;

    /* Build the cluster. */
    jce_light_cluster_set_camera(fp->cluster, inv_view, proj, z_near, z_far);
    if (!jce_light_cluster_build(fp->cluster, lights, light_count)) return;
    JceLightClusterResult res = jce_light_cluster_get_result(fp->cluster);

    /* Pack into the CPU mirror.  Our packed buffer was allocated for
     * max_lights; re-stamp its light_count to match the actual count so the
     * shape check in jce_forwardplus_pack passes. */
    fp->packed.light_count = light_count;
    fp->packed.param_texels = light_count * JCE_FP_TEXELS_PER_LIGHT;
    if (!jce_forwardplus_pack(&fp->packed, &res, lights, params,
                              light_count, z_near, z_far)) {
        return;
    }
    fp->packed_valid = true;

    /* Combine the three regions into ONE upload buffer (single texture). */
    jce_forwardplus_combined_free(&fp->combined);
    if (!jce_forwardplus_pack_combined(&fp->combined, &fp->packed)) return;

    if (!fp_gpu_init(fp)) { fp->enabled = false; return; }

    /* Upload the single combined texture. */
    fp_upload_combined(fp->tex_cluster, fp->tex_w, fp->tex_h, &fp->combined);

    /* Uniforms (see header contract).  Uploaded but DORMANT — no shader reads
     * u_clusterParams* / u_clusterRegions in ROUND A, so they have zero effect
     * on output.  bgfx zero-clears unset uniforms between draws, so setting
     * these here cannot perturb any existing uniform the brute-force path
     * consumes. */
    float cp[4]  = { (float)fp->cells_x, (float)fp->cells_y,
                     (float)fp->slices_z, (float)light_count };
    float cp2[4] = { (float)fp->max_per_cell, (float)JCE_FP_TEX_WIDTH,
                     z_near, z_far };
    float cr[4]  = { (float)fp->combined.grid_base_row,
                     (float)fp->combined.index_base_row,
                     (float)fp->combined.lights_base_row,
                     (float)fp->combined.total_rows };
    jce_enc_set_uniform(fp->u_clusterParams,  cp,  1);
    jce_enc_set_uniform(fp->u_clusterParams2, cp2, 1);
    jce_enc_set_uniform(fp->u_clusterRegions, cr,  1);

    /* Cache the uniform values so jce_forwardplus_bind can replay them on
     * every PBR submit this frame (bgfx clears uniform/texture-stage state
     * between submits, so the bind must carry the full cluster state per
     * draw).  The bgfx_set_uniform calls above are harmless extras; the
     * authoritative per-draw set happens in jce_forwardplus_bind. */
    memcpy(fp->last_cp,  cp,  sizeof(cp));
    memcpy(fp->last_cp2, cp2, sizeof(cp2));
    memcpy(fp->last_cr,  cr,  sizeof(cr));
    fp->last_upload_ok = true;

    /* ── ROUND B: the SINGLE combined cluster texture is RESIDENT (uploaded
     * via bgfx_update_texture_2d above).  The per-draw STAGE binding (stage 14
     * = s_cluster) is issued by jce_forwardplus_bind, called from the scene
     * renderer's per-submit bind callback ONLY when the JCE_FORWARDPLUS fs_pbr
     * variant program is the one being submitted.  Binding here would be lost
     * (bgfx clears stage state every submit), so we don't bind in _update. */

    (void)r;
}

void jce_forwardplus_bind(JceForwardPlus *fp)
{
    if (!fp || !fp->enabled || !fp->gpu_init || !fp->last_upload_ok) return;
    if (!BGFX_HANDLE_IS_VALID(fp->tex_cluster) ||
        !BGFX_HANDLE_IS_VALID(fp->s_cluster)) {
        return;
    }

    /* Stage 14 = s_cluster (the forward+ variant freed it from s_iesLut).
     * Cookie (13) and local shadow (15) are untouched here. */
    jce_enc_set_texture(14, fp->s_cluster, fp->tex_cluster, UINT32_MAX);
    jce_enc_set_uniform(fp->u_clusterParams,  fp->last_cp,  1);
    jce_enc_set_uniform(fp->u_clusterParams2, fp->last_cp2, 1);
    jce_enc_set_uniform(fp->u_clusterRegions, fp->last_cr,  1);
}
