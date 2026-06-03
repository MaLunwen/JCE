/*
 * jce_views.h  bgfx view ID assignments.
 *
 * bgfx renders views in ID order. Lower IDs render first.
 * This header defines the canonical view layout for JCE.
 */

#ifndef JCE_VIEWS_H
#define JCE_VIEWS_H

/* -- View IDs ------------------------------------------------------- */

/* Main 3D scene (perspective camera, depth test, lit meshes). */
#define JCE_VIEW_MAIN_3D    0

/* Debug overlay (bgfx debug text, profiler). */
#define JCE_VIEW_DEBUG       2

/* Editor scene viewport render target (off-screen scene panel). */
#define JCE_VIEW_EDITOR_SCENE 3

/* Shadow map depth passes (one per shadow-casting light). */
#define JCE_VIEW_SHADOW_BASE 10
#define JCE_VIEW_SHADOW_0    10
#define JCE_VIEW_SHADOW_1    11
#define JCE_VIEW_SHADOW_2    12
#define JCE_VIEW_SHADOW_3    13
#define JCE_VIEW_SHADOW_4    14

/* Reserved range for post-processing (Phase 4). */
#define JCE_VIEW_POST_BASE   20

/* Editor gizmo / grid / selection overlay — drawn AFTER PostFX so it
 * doesn't get tone-mapped or bloomed. Targets the postfx output FBO. */
#define JCE_VIEW_EDITOR_OVERLAY 50

/* Editor material-graph live preview (offscreen sphere). Sits between
 * the editor's main scene viewport (3) and the post-fx / overlay
 * range, so the preview can be drawn while editor scene rendering is
 * still in progress without view-ordering surprises. */
#define JCE_VIEW_EDITOR_PREVIEW 60

/* Hidden editor object-ID picking pass.  Separate FBO/readback target,
 * after regular scene + preview views and before UI. */
#define JCE_VIEW_EDITOR_PICK 70
/* One-pixel GPU blit into a readback staging texture. */
#define JCE_VIEW_EDITOR_PICK_READBACK 71

/* ImGui editor overlay (renders before UI overlay so HUD sits on top). */
#define JCE_VIEW_IMGUI       250

/* 2D UI / RmlUi overlay (orthographic, no depth test — sprites, text,
 * debug HUD). Placed AFTER ImGui so the engine HUD is visible on top
 * of editor panels. */
#define JCE_VIEW_UI          254

/* Range of transient view ids reserved exclusively for clearing
 * freshly-created render-target textures. NOT USED any more —
 * view-id-based ordering doesn't work for this purpose because bgfx
 * runs all views per frame and the renderer's own writes to the FBO
 * happen via lower view ids that run BEFORE our clear. Kept as
 * reserved range; clears are now done via direct CPU-zero upload at
 * texture creation time (see jce_clear_freshly_created_fbo). */
#define JCE_VIEW_INIT_CLEAR_BASE  232
#define JCE_VIEW_INIT_CLEAR_COUNT 16
#define JCE_VIEW_INIT_CLEAR       JCE_VIEW_INIT_CLEAR_BASE  /* legacy alias */

#ifdef __cplusplus
extern "C" {
#endif

/* No-op stub kept for ABI; see jce_views.c. The right way to scrub
 * fresh GPU textures is to upload zero memory at create time, which
 * each subsystem now does directly via bgfx_create_texture_2d's _mem
 * parameter when needed. */
void jce_clear_freshly_created_fbo(unsigned short fb_idx,
                                   unsigned short width,
                                   unsigned short height);

/* Returns a bgfx_memory_t of `size_bytes` filled with zeros (or NULL
 * on alloc failure). Pass to bgfx_create_texture_2d's _mem parameter
 * so the new texture's GPU storage starts as solid black instead of
 * leftover uninitialised VRAM ("rainbow garbage"). bgfx releases the
 * memory once the upload is enqueued. */
struct bgfx_memory;
const struct bgfx_memory *jce_zero_init_mem(unsigned int size_bytes);

#ifdef __cplusplus
}
#endif

#endif /* JCE_VIEWS_H */
