/*
 * jce_forwardplus.h  Forward+ clustered lighting — GPU upload (ROUND A).
 *
 * Wraps jce_light_cluster (the CPU froxel build + per-froxel light
 * assignment) and PACKS its result into ONE GPU-portable RGBA32F data
 * texture plus a small uniform block, so a future fragment shader
 * (ROUND B's fs_pbr.sc) can replace the brute-force 16-light loop with a
 * per-froxel clustered loop — lifting the JCE_MAX_POINT_LIGHTS cap.
 *
 * ROUND A scope: build + upload only.  NOTHING in any shader reads this
 * texture/the uniforms yet, so rendering is byte-identical to the existing
 * brute-force path.  The packing math is split out as pure, headless,
 * bgfx-free functions (jce_forwardplus_pack producing the per-region arrays,
 * and jce_forwardplus_pack_combined concatenating them into the single
 * upload buffer) so it is unit-testable on a host with no GPU;
 * jce_forwardplus_update pushes the packed bytes to the GPU on a render
 * thread.
 *
 * ROUND B SLOT RESOLUTION (why ONE texture):  fs_pbr.sc already occupies
 * ALL of bgfx sampler stages 0..15 and WebGL2/GLES3 hardware caps fragment
 * samplers at 16, so the three separate cluster textures of an earlier
 * Round-A draft (grid/index/lights = 3 extra stages) are physically
 * impossible.  We therefore concatenate the three texel regions into ONE
 * RGBA32F texture sampled from a SINGLE stage (s_cluster); the shader adds
 * each region's base ROW offset (uploaded in u_clusterRegions) before its
 * existing in-region (e % W, e / W) addressing.
 *
 * Cross-backend constraints (JCE ships dx11 + spirv + glsl + GL ES/WASM):
 *   - NO compute shaders, NO SSBOs.  Light assignment is done CPU-side by
 *     jce_light_cluster; the shader only SAMPLES the resulting texture.
 *   - The texture is BGFX_TEXTURE_FORMAT_RGBA32F (exact float storage;
 *     32-bit-float sampling is in the GLES3/WebGL2 baseline JCE already
 *     targets — mirrors how jce_lighting_system packs uniforms, just
 *     promoted to a texture so the per-froxel index list can be arbitrarily
 *     long instead of capped at a uniform-array size).
 *   - Exactly ONE sampler slot is consumed (s_cluster), because fs_pbr.sc
 *     already owns stages 0..15 and WebGL2/GLES3 caps fragment samplers at
 *     16.  The three logical regions (grid / index / lights) live in ONE
 *     texture stacked by rows.
 *
 * ════════════════════════════════════════════════════════════════════════
 *  GPU LAYOUT CONTRACT  (bit-for-bit; ROUND B's fs_pbr.sc MUST mirror this)
 * ════════════════════════════════════════════════════════════════════════
 *
 * Grid dimensions are uploaded in u_clusterParams / u_clusterParams2 and the
 * three region base ROWS in u_clusterRegions (see below).  Let:
 *     CX = cells_x, CY = cells_y, CZ = slices_z          (froxel grid dims)
 *     NCELL = CX * CY * CZ                                (total froxels)
 *     MAXIDX = NCELL * max_per_cell                       (index-list cap)
 *     NPARAM = lightCount * JCE_FP_TEXELS_PER_LIGHT       (param texels)
 *     W = JCE_FP_TEX_WIDTH                                (fixed 256)
 *
 *  ── SINGLE COMBINED TEXTURE (s_cluster, ONE sampler stage) ──────────────
 *  All three element arrays are concatenated into ONE RGBA32F texture of
 *  fixed width W, stacked as three contiguous ROW-regions:
 *
 *      gridRows   = ceil(NCELL  / W)
 *      indexRows  = ceil(MAXIDX / W)
 *      lightsRows = ceil(max(NPARAM,1) / W)
 *
 *      region GRID    : rows [ gridBaseRow   , gridBaseRow   + gridRows  )
 *      region INDEX   : rows [ indexBaseRow  , indexBaseRow  + indexRows )
 *      region LIGHTS  : rows [ lightsBaseRow , lightsBaseRow + lightsRows)
 *
 *      gridBaseRow   = 0
 *      indexBaseRow  = gridRows
 *      lightsBaseRow = gridRows + indexRows
 *      totalRows     = gridRows + indexRows + lightsRows     (texture height)
 *
 *  These three base rows + W + totalRows are uploaded in u_clusterRegions
 *  (see Uniforms below).  WITHIN a region, an element e keeps its old
 *  row-major addressing; the ABSOLUTE texel the shader fetches is:
 *      tx       = e % W
 *      texelRow = regionBaseRow + (e / W)
 *  Point-clamp NEAREST sampling (no filtering, no mips).  Use texelFetch in
 *  GLSL/SPIRV; on backends without integer texelFetch, sample at
 *      uv = ((tx + 0.5) / W , (texelRow + 0.5) / totalRows)
 *  with POINT CLAMP — exact.
 *
 *  ── (i) GRID region  (base row = gridBaseRow = 0) ───────────────────────
 *  NCELL texels, one per froxel `c` (e = c):
 *      .r = offset   (float; exact integer) start index into the INDEX list
 *      .g = count    (float; exact integer) number of lights in this froxel
 *      .b = 0
 *      .a = 0
 *  Froxel index c is computed by the shader from the fragment (see "froxel
 *  index" below); within-region element e = c, absolute texel =
 *  ( c % W , gridBaseRow + c / W ).
 *
 *  ── (ii) LIGHT-INDEX region  (base row = indexBaseRow) ──────────────────
 *  MAXIDX texels.  Flat list; for froxel c the lights are at list elements
 *  [offset .. offset+count).  Element e holds:
 *      .r = lightIndex  (float; exact integer) index into the LIGHTS region
 *      .g = .b = .a = 0
 *  i.e. the shader loops k in [0,count): e = offset + k;
 *  li = texel( e % W , indexBaseRow + e / W ).r.
 *
 *  ── (iii) LIGHT-PARAMS region  (base row = lightsBaseRow) ───────────────
 *  NPARAM texels.  Exactly JCE_FP_TEXELS_PER_LIGHT = 4 texels per light.
 *  For light `li` the base element is b = li * 4:
 *      texel b+0 : .rgb = world-space position,        .a = range (radius)
 *      texel b+1 : .rgb = linear color,                .a = intensity
 *      texel b+2 : .r   = type (0 = point, 1 = spot),
 *                  .gba = spot forward direction (normalized; (0,0,0) point)
 *      texel b+3 : .r   = inner cone cos (spot; 1.0 for point),
 *                  .g   = outer cone cos (spot; -1.0 for point),
 *                  .b   = .a = 0   (reserved: shadow slot / flags in v2)
 *  Within-region element e = b + j (j in 0..3); absolute texel =
 *  ( e % W , lightsBaseRow + e / W ).
 *  Attenuation for both types is the brute-force formula already in
 *  fs_pbr.sc:  att = clamp(1 - d*d / range*range, 0, 1)^2 ; spot adds the
 *  cone smoothstep(outerCos, innerCos, dot(L, -dir)).  Keeping it identical
 *  guarantees ROUND B is a culling change, not a shading change.
 *
 *  ── froxel index from a fragment (ROUND B shader math) ──────────────────
 *  Given the fragment's screen UV in [0,1] (origin top-left; on GL flip V
 *  to match the framebuffer convention used elsewhere in fs_pbr.sc) and its
 *  POSITIVE view-space depth  vz = -viewPos.z  (or v_viewdepth, which the
 *  shader already has):
 *      tileX = clamp( floor(uv.x * CX), 0, CX-1 )
 *      tileY = clamp( floor(uv.y * CY), 0, CY-1 )
 *      slice = clamp( floor( log(vz / zNear) / log(zFar / zNear) * CZ ),
 *                     0, CZ-1 )                          // log-Z (see below)
 *      c = (slice * CY + tileY) * CX + tileX             // MUST match the
 *                                                        //  CPU build order
 *  This froxel ordering is the SAME one jce_light_cluster.c writes:
 *      cell = (s * cells_y + ty) * cells_x + tx          (jce_light_cluster.c)
 *  Any divergence here mis-lights — keep them identical.
 *
 *  ── log-Z slice formula (matches jce_light_cluster_slice_for_view_z) ────
 *  zNear/zFar are the camera planes uploaded in u_clusterParams2.zw.  For a
 *  positive view depth vz:
 *      vz <= zNear            -> slice 0
 *      vz >= zFar             -> slice CZ-1
 *      else  slice = floor( log(vz / zNear) / log(zFar / zNear) * CZ )
 *  (Identical to the CPU reference so a light's froxel on the GPU equals the
 *   froxel the CPU assigned it to.)
 *
 *  ── SAMPLER SLOT (ROUND B) ──────────────────────────────────────────────
 *  fs_pbr.sc already uses ALL of stages 0..15 and this bgfx build caps at
 *  BGFX_CONFIG_MAX_TEXTURE_SAMPLERS = 16 (WebGL2/GLES3 fragment-sampler hw
 *  limit), so the combined cluster texture must consume exactly ONE stage.
 *  The single sampler uniform is s_cluster.  ROUND A still does NOT
 *  bgfx_set_texture it to a stage (that + the shader read land in B-shader);
 *  the data is resident but unread, so output is byte-identical.  ROUND B
 *  frees a stage (fold IES off s_iesLut(14) when Forward+ is on, per the
 *  design doc) and binds s_cluster there.
 *
 *  ── Uniforms (FREE slots; do NOT collide with the brute-force path) ─────
 *  u_clusterParams  (vec4):  x = CX, y = CY, z = CZ, w = lightCount
 *  u_clusterParams2 (vec4):  x = max_per_cell, y = JCE_FP_TEX_WIDTH,
 *                            z = zNear, w = zFar
 *  u_clusterRegions (vec4):  x = gridBaseRow, y = indexBaseRow,
 *                            z = lightsBaseRow, w = totalRows (texture height)
 *  (All counts are exact integers stored as float, read with int() in GLSL.
 *   The shader reads region bases from u_clusterRegions, builds an absolute
 *   texelRow = regionBase + e/W, and samples s_cluster.)
 *
 * Layer: Renderer (Layer 3).
 */

#ifndef JCE_FORWARDPLUS_H
#define JCE_FORWARDPLUS_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_light_cluster.h>
#include <jce/renderer/jce_lighting_system.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* JceRenderer is forward-declared by jce_lighting_system.h (included above). */

/* ── Layout constants (mirror these in ROUND B's shader) ───────────────── */

/* Fixed width of the grid / index / param data textures.  All three 1-D
 * arrays are tiled row-major into a 2D texture of this width.  256 keeps
 * even a 24x16x24 (9216-froxel) desktop grid to a 256x36 texture and a
 * generous index list bounded, well under any backend's max texture size. */
#define JCE_FP_TEX_WIDTH 256u

/* RGBA32F texels per light in the params texture (see contract above). */
#define JCE_FP_TEXELS_PER_LIGHT 4u

/* ── Packed CPU mirror of the GPU texture regions (headless, bgfx-free) ──
 *
 * jce_forwardplus_pack fills these per-region float arrays with EXACTLY the
 * bytes each region of the single combined GPU texture will contain.  The
 * unit test asserts on these.  Each array is RGBA32F: 4 floats per texel.
 * Sizes are reported back so the combiner/uploader can compute region row
 * counts and the total texture height. */
typedef struct {
    /* Grid texels: NCELL texels (offset,count,0,0).  rows = ceil(NCELL/W). */
    float   *grid;          /* length = grid_texels * 4 */
    uint32_t grid_texels;   /* = NCELL = CX*CY*CZ */

    /* Index list texels: one float index per used element.  We allocate the
     * full MAXIDX capacity so a froxel's [offset,offset+count) range is
     * always in-bounds; unused tail is zero. */
    float   *index;         /* length = index_texels * 4 */
    uint32_t index_texels;  /* = NCELL * max_per_cell (capacity) */
    uint32_t index_used;    /* = total assignments actually written */

    /* Light params texels: JCE_FP_TEXELS_PER_LIGHT per light. */
    float   *params;        /* length = param_texels * 4 */
    uint32_t param_texels;  /* = light_count * JCE_FP_TEXELS_PER_LIGHT */
    uint32_t light_count;

    /* Grid dims + camera planes echoed for the uniform upload / asserts. */
    uint32_t cells_x, cells_y, slices_z, max_per_cell;
    float    z_near, z_far;
} JceForwardPlusPacked;

/* Allocate the packed arrays for the given grid + light count.  Returns
 * false on bad args / OOM.  Free with jce_forwardplus_packed_free. */
JCE_API bool jce_forwardplus_packed_alloc(JceForwardPlusPacked *out,
                                          uint32_t cells_x,
                                          uint32_t cells_y,
                                          uint32_t slices_z,
                                          uint32_t max_per_cell,
                                          uint32_t light_count);

JCE_API void jce_forwardplus_packed_free(JceForwardPlusPacked *p);

/* Per-light shading params, parallel to the JceLightProxy geometry array. */
typedef struct {
    jce_vec3 color;
    float    intensity;
    float    type;            /* 0 = point, 1 = spot */
    jce_vec3 spot_dir;        /* normalized spot forward; (0,0,0) for point */
    float    inner_cone_cos;  /* spot; 1.0 for point */
    float    outer_cone_cos;  /* spot; -1.0 for point */
} JceForwardPlusLightParam;

/* PURE PACKING (headless; no bgfx).  Flattens a finished light-cluster
 * build result + the light proxy params into the texel arrays of `out`
 * exactly as the GPU textures will hold them (see the layout contract).
 *
 *   result : from jce_light_cluster_get_result (post-build).
 *   lights : the SAME proxy array passed to jce_light_cluster_build, so a
 *            cluster index li maps to lights[li] (1:1, matching the cluster
 *            module's user_id == build-call array index convention).
 *   params : per-light shading params, parallel to `lights` (lights[li]
 *            gives geometry; params[li] gives color/intensity/type/cone).
 *
 * `out` must have been allocated with matching grid dims + light_count
 * (jce_forwardplus_packed_alloc).  z_near/z_far are stored into `out`.
 * Returns false on shape mismatch.  Deterministic; no globals, no RNG. */
JCE_API bool jce_forwardplus_pack(JceForwardPlusPacked            *out,
                                  const JceLightClusterResult     *result,
                                  const JceLightProxy             *lights,
                                  const JceForwardPlusLightParam  *params,
                                  uint32_t                         light_count,
                                  float                            z_near,
                                  float                            z_far);

/* ── Combined single-texture layout (headless; no bgfx) ──────────────────
 *
 * The three regions (grid / index / lights) are stacked into ONE RGBA32F
 * texture of fixed width W = JCE_FP_TEX_WIDTH.  This describes the region
 * row geometry + the single contiguous upload buffer the GPU wrapper pushes
 * with one bgfx_update_texture_2d.  Within a region, element e still maps to
 * (e % W) column; its ABSOLUTE row is regionBaseRow + (e / W). */
typedef struct {
    float   *data;            /* W * total_rows texels, RGBA32F (4 floats ea.) */
    uint32_t width;           /* = JCE_FP_TEX_WIDTH */
    uint32_t total_rows;      /* = grid_rows + index_rows + lights_rows */

    uint32_t grid_base_row;   /* = 0 */
    uint32_t index_base_row;  /* = grid_rows */
    uint32_t lights_base_row; /* = grid_rows + index_rows */

    uint32_t grid_rows;       /* = ceil(grid_texels   / W) */
    uint32_t index_rows;      /* = ceil(index_texels  / W) */
    uint32_t lights_rows;     /* = ceil(max(param_texels,1) / W) */
} JceForwardPlusCombined;

/* PURE COMBINE (headless; no bgfx).  Concatenates the per-region arrays of a
 * packed mirror into ONE contiguous W-wide RGBA32F buffer laid out as three
 * stacked row-regions (grid, then index, then lights), and reports the region
 * base rows + total dims.  `src` must be a populated JceForwardPlusPacked
 * (from jce_forwardplus_pack).  Allocates `out->data`; free with
 * jce_forwardplus_combined_free.  Deterministic; no globals, no RNG.
 * Returns false on bad args / OOM. */
JCE_API bool jce_forwardplus_pack_combined(JceForwardPlusCombined     *out,
                                           const JceForwardPlusPacked *src);

JCE_API void jce_forwardplus_combined_free(JceForwardPlusCombined *c);

/* ── Stateful GPU wrapper (render-thread; uses bgfx) ─────────────────────
 *
 * Owns a JceLightCluster + the packed CPU mirror + ONE dynamic combined GPU
 * texture + the uniform handles.  Toggleable: while disabled, no GPU
 * resources are created and upload is a no-op, so the brute-force path is
 * untouched.  All functions are NULL-safe. */
typedef struct JceForwardPlus JceForwardPlus;

JCE_API JceForwardPlus *jce_forwardplus_create(uint32_t cells_x,
                                               uint32_t cells_y,
                                               uint32_t slices_z,
                                               uint32_t max_per_cell,
                                               uint32_t max_lights);
JCE_API void jce_forwardplus_destroy(JceForwardPlus *fp);

/* Enable/disable the per-frame cluster build + upload.  Default: disabled
 * (ROUND A keeps the data DORMANT).  When disabled, _update is a no-op. */
JCE_API void jce_forwardplus_set_enabled(JceForwardPlus *fp, bool enabled);
JCE_API bool jce_forwardplus_is_enabled(const JceForwardPlus *fp);

/* Per-frame: build the cluster for the given camera basis from the supplied
 * light geometry (proxies) + shading params, pack, and upload the textures +
 * uniforms.  No-op when disabled.
 *
 * The caller (scene renderer) already constructs the per-light data when it
 * fills the JceLightEnv, so this module takes EXPLICIT parallel arrays rather
 * than reaching into the opaque JceLightEnv — keeping it decoupled (no
 * cross-layer reach into private struct fields).  `lights[i]` (geometry) and
 * `params[i]` (shading) describe the same light i, 1:1.
 *
 *   inv_view/proj : camera world-from-view and view-to-clip matrices.
 *   z_near/z_far  : positive camera planes (0 < near < far).
 * Safe to call after jce_light_env_apply on the render thread. */
JCE_API void jce_forwardplus_update(JceForwardPlus                 *fp,
                                    const JceLightProxy            *lights,
                                    const JceForwardPlusLightParam *params,
                                    uint32_t                        light_count,
                                    const jce_mat4                 *inv_view,
                                    const jce_mat4                 *proj,
                                    float                           z_near,
                                    float                           z_far,
                                    const JceRenderer              *r);

/* Read-only access to the last packed mirror (for tests / debug overlays).
 * Returns NULL when disabled or never updated. */
JCE_API const JceForwardPlusPacked *
jce_forwardplus_get_packed(const JceForwardPlus *fp);

/* ── ROUND B per-draw binding ────────────────────────────────────────────
 * Bind the SINGLE combined cluster texture on sampler stage 14 (s_cluster)
 * plus the three cluster uniforms (u_clusterParams / u_clusterParams2 /
 * u_clusterRegions) for the NEXT PBR submit.  bgfx clears per-draw texture
 * stage + uniform state between submits, so this must be called once per
 * submit that uses the JCE_FORWARDPLUS fs_pbr variant (mirrors how the scene
 * renderer re-binds IBL / lights per submit).
 *
 * No-op (binds nothing) when disabled or before the first successful update,
 * so the DEFAULT brute-force program never sees stage 14 touched here — its
 * s_iesLut binding is left to the existing cookie/IES path.  NULL-safe.
 *
 * IMPORTANT: only call this when the forward+ variant PROGRAM is the one being
 * submitted.  Binding s_cluster on stage 14 while the default program (which
 * has s_iesLut there) is submitted would mis-bind the IES sampler. */
JCE_API void jce_forwardplus_bind(JceForwardPlus *fp);

JCE_EXTERN_C_END

#endif /* JCE_FORWARDPLUS_H */
