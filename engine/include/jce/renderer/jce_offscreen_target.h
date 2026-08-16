/*
 * jce_offscreen_target.h  Engine-owned editor viewport bridge.
 *
 * Encapsulates off-screen render target lifecycle and view setup used by
 * the editor scene viewport, so panel-side code does not manage bgfx FBOs.
 */

#ifndef JCE_OFFSCREEN_TARGET_H
#define JCE_OFFSCREEN_TARGET_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer JceRenderer;
typedef struct JceOffscreenTarget JceOffscreenTarget;

/* Create/destroy an editor render bridge.
 * view_id=0 uses JCE_VIEW_EDITOR_SCENE. */
JCE_API JceOffscreenTarget *jce_offscreen_target_create(JceRenderer *renderer,
                                                       uint16_t view_id);
JCE_API void jce_offscreen_target_destroy(JceOffscreenTarget *bridge);

/* Ensure render target exists at width/height and configure the view.
 * Returns false if target allocation or setup fails. */
JCE_API bool jce_offscreen_target_prepare(JceOffscreenTarget *bridge,
                                      uint32_t width,
                                      uint32_t height,
                                      const float *view16,
                                      const float *proj16,
                                      uint32_t clear_rgba,
                                      const char *view_name);

/* Bind another ordered view to the bridge's existing framebuffer without
 * clearing it.  This is used for passes that must retain both scene color and
 * depth while executing after an intermediate full-screen stage. */
JCE_API bool jce_offscreen_target_prepare_overlay_view(
    JceOffscreenTarget *bridge, uint16_t view_id,
    const float *view16, const float *proj16, const char *view_name);

/* Query bridge-owned texture/view handles for panel integration. */
JCE_API uint16_t jce_offscreen_target_get_color_texture(
    const JceOffscreenTarget *bridge);
JCE_API uint16_t jce_offscreen_target_get_depth_texture(
    const JceOffscreenTarget *bridge);
JCE_API uint16_t jce_offscreen_target_get_view_id(const JceOffscreenTarget *bridge);

/* Raw frame-buffer handle index of the offscreen target (or UINT16_MAX
 * when no target has been allocated yet). Used by post-passes that need
 * to bind the same destination FBO from a different view-id (e.g. the
 * volumetric-fog composite, which must run AFTER fog render in view
 * ordering and therefore cannot share the offscreen view-id). */
JCE_API uint16_t jce_offscreen_target_get_frame_buffer(const JceOffscreenTarget *bridge);

/* True when the color target is an HDR (RGBA16F) format. The editor uses this
 * to keep the tonemap pass always-on (the HDR bridge must be tonemapped to LDR
 * before display). False when the RGBA8 fallback was used. */
JCE_API bool jce_offscreen_target_is_hdr(const JceOffscreenTarget *bridge);

/* Composite an arbitrary color texture over the bridge's framebuffer as an
 * opaque fullscreen quad on `view_id` (pick a view that executes AFTER the
 * pass that produced the texture).  Used by the editor Game View to fold the
 * post-fx output back into the bridge so overlay passes (canvas UI) land
 * after tone mapping — the shipped runtime's scene→postfx→UI order.
 * `flip_v` mirrors the source vertically: pass
 * jce_renderer_origin_bottom_left() when the source is a post-fx style RT —
 * on bottom-left-origin backends (OpenGL) its sampling orientation is
 * inverted relative to the bridge/canvas convention, and an unflipped copy
 * lands the 3D scene upside-down under the panel's display flip (empirically
 * verified: GL editor Game View, scene inverted while the UI stayed upright). */
JCE_API void jce_offscreen_target_composite_texture(
    JceOffscreenTarget *bridge, uint16_t view_id, uint16_t texture_idx,
    uint16_t width, uint16_t height, bool flip_v);

JCE_EXTERN_C_END

#endif /* JCE_OFFSCREEN_TARGET_H */
