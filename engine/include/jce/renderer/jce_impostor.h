/*
 * jce_impostor.h  Octahedral impostors — terminal-LOD billboard cards.
 *
 * Beyond the last discrete mesh LOD, a distant prop/tree renders as a single
 * camera-facing CARD that samples a pre-baked OCTAHEDRAL ATLAS: an offline GPU
 * pass renders the mesh from grid_n × grid_n viewpoints arranged on an
 * octahedral-mapped sphere into one atlas texture (albedo + coverage/alpha).
 * At runtime the fragment shader maps the current view direction to the nearest
 * atlas cell and samples it, so a whole forest of far trees costs a handful of
 * instanced quads.  (RDR2 / Genshin / UE technique.)
 *
 * Two halves:
 *   BAKE  (editor / tools, GPU): jce_impostor_bake_submit + _poll render the
 *         model into the atlas across one bgfx frame, read it back, and write a
 *         PNG atlas + .impostor.json metadata sidecar next to the source asset.
 *   DRAW  (runtime, every renderer/LOD path): jce_impostor_atlas_load resolves
 *         the cooked atlas + metadata; jce_impostor_draw_instanced submits all
 *         resident far-cards as ONE instanced draw per atlas.
 *
 * Octahedral mapping (full sphere) — must match fs_impostor.sc oct_encode:
 *   dir /= (|x|+|y|+|z|); oct = dir.xz; if dir.y<0 fold across the edges;
 *   uv = oct*0.5+0.5.
 *
 * Layer: Graphics (Layer 3).
 */
#ifndef JCE_IMPOSTOR_H
#define JCE_IMPOSTOR_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceModel    JceModel;
typedef struct JceRenderer JceRenderer;

#define JCE_IMPOSTOR_MAX_GRID  16   /* grid_n upper clamp (16x16 = 256 views) */

/* ── Octahedral direction math (shared CPU/GPU convention) ─────────── */

/* Encode a unit direction onto the [0,1]^2 octahedral atlas. */
JCE_API jce_vec2 jce_impostor_oct_encode(jce_vec3 dir);
/* Decode an atlas UV [0,1]^2 back to a unit direction (used by the bake). */
JCE_API jce_vec3 jce_impostor_oct_decode(jce_vec2 uv);

/* ── Persistent metadata (the .impostor.json sidecar) ──────────────── */

typedef struct {
    int      grid_n;        /* views per side (grid_n x grid_n cells)        */
    int      cell_px;       /* pixels per atlas cell side                    */
    float    center[3];     /* model-local bounds center (billboard anchor)  */
    float    radius;        /* model bounds radius (billboard half-size)     */
    char     atlas_path[256]; /* project-relative path to the baked PNG atlas */
} JceImpostorMeta;

/* Write / read the metadata sidecar (host filesystem, absolute or cooked
 * paths).  Returns false on I/O or parse failure. */
JCE_API bool jce_impostor_meta_write(const char *json_path,
                                     const JceImpostorMeta *meta);
JCE_API bool jce_impostor_meta_read(const char *json_path,
                                    JceImpostorMeta *out_meta);

/* ── GPU bake (editor / tools; render thread) ──────────────────────── */

typedef struct {
    JceModel    *model;        /* required: the LOD0 model to bake          */
    JceRenderer *renderer;     /* required: active renderer (for programs)  */
    int          grid_n;       /* views per side (clamped 2..MAX)           */
    int          cell_px;      /* pixels per cell (clamped 32..512)         */
    char         atlas_path_host[512]; /* where to write the PNG atlas      */
    char         meta_path_host[512];  /* where to write the .impostor.json */
    char         atlas_path_rel[256];  /* project-relative atlas path (meta)*/
} JceImpostorBakeDesc;

typedef enum {
    JCE_IMPOSTOR_BAKE_IDLE = 0,
    JCE_IMPOSTOR_BAKE_RENDERING, /* atlas views submitted; awaiting frame   */
    JCE_IMPOSTOR_BAKE_READBACK,  /* screenshot requested; awaiting pixels    */
    JCE_IMPOSTOR_BAKE_DONE,
    JCE_IMPOSTOR_BAKE_FAILED,
} JceImpostorBakeStatus;

/* Begin a bake.  At most one in flight (returns false if busy or desc bad).
 * The bake is frame-driven: call jce_impostor_bake_poll() once per rendered
 * frame (after the renderer's bgfx_frame) until status is DONE/FAILED. */
JCE_API bool                  jce_impostor_bake_submit(const JceImpostorBakeDesc *desc);
JCE_API JceImpostorBakeStatus jce_impostor_bake_poll(void);
JCE_API float                 jce_impostor_bake_progress(void);

/* ── Runtime atlas (loaded from the cooked PNG + metadata) ─────────── */

typedef struct {
    JceTexture       atlas;   /* the baked RGBA atlas texture               */
    JceImpostorMeta  meta;
    bool             valid;
} JceImpostorAtlas;

/* Load an atlas + metadata for runtime drawing.  `meta_path` is the cooked /
 * resolvable .impostor.json; the atlas texture is resolved via `tex_load`
 * (the renderer's texture resolve, so the cooked PNG goes through the normal
 * texture cache).  Returns false (out->valid=false) when either is missing. */
typedef JceTexture (*JceImpostorTexLoadFn)(void *user, const char *rel_path);
JCE_API bool jce_impostor_atlas_load(const char *meta_path,
                                     JceImpostorTexLoadFn tex_load, void *user,
                                     JceImpostorAtlas *out);

/* Per-instance card record: world center, billboard radius, baseColor tint. */
typedef struct {
    float center[3];
    float radius;
    float tint[3];   /* baseColor modulation (linear) */
} JceImpostorInstance;

/* Submit `count` far-cards that share `atlas` as ONE instanced draw on
 * `view_id`.  Billboards face the camera; the FS samples the octahedral cell
 * for each card's view direction.  `light_dir`/`light_color` drive the flat sun
 * term (pass the scene's main directional light; NULL = neutral).  No-op when
 * the atlas is invalid or the impostor program is unavailable. */
JCE_API void jce_impostor_draw_instanced(JceRenderer *r, uint16_t view_id,
                                         const JceImpostorAtlas *atlas,
                                         const JceImpostorInstance *insts,
                                         uint32_t count,
                                         const float light_dir[3],
                                         const float light_color[4]);

/* Release GPU resources held by the impostor module (program, quad VB, uniforms,
 * any in-flight bake FBO).  Called at renderer shutdown. */
JCE_API void jce_impostor_shutdown(void);

JCE_EXTERN_C_END

#endif /* JCE_IMPOSTOR_H */
