/*
 * jce_sr_camera_stack.c -- overlay cameras, drawn on top of a base camera.
 *
 * Unity's camera stacking.  One function, and it is here rather than in
 * jce_scene_renderer.c because it shares no state with that file: it reads a
 * JceSceneRenderConfig, sets four bgfx view properties, and calls the public
 * render entry.  The scene-side half -- which cameras are in the stack and in
 * what order -- is jce_scene_camera_resolve_stack in jce_scene_camera.c.
 *
 * The view IDS are the load-bearing part and they are decided elsewhere: the
 * base render's view-order pass names base+64..66 immediately after the colour
 * view, ahead of SSR, the project fullscreen chain, SSGI's composite and the
 * PostFX chain -- every one of which READS the colour an overlay writes into.
 * See JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET in <jce/renderer/jce_views.h>.
 */
#include "jce_sr_internal.h"

void jce_scene_renderer_render_camera_overlay(JceSceneRenderer *sr,
                                              JceScene *scene,
                                              const JceCamera *camera,
                                              uint16_t view_id_base,
                                              uint8_t overlay_index,
                                              float dt_sec,
                                              const JceSceneRenderConfig *config)
{
    if (!sr || !scene || !camera || !config) return;
    if (overlay_index >= JCE_VIEW_SR_CAMERA_OVERLAY_MAX) {
        /* Said once, not dropped in silence: a fourth overlay camera is a
         * thing someone authored, and a stack that quietly renders three of
         * four is the shape this audit exists to remove. */
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN(LOG_TAG,
                     "camera stack: at most %u overlay cameras render; "
                     "stack_index beyond that draws nothing",
                     (unsigned)JCE_VIEW_SR_CAMERA_OVERLAY_MAX);
        }
        return;
    }

    const uint16_t view = (uint16_t)(view_id_base +
                                     JCE_VIEW_SR_CAMERA_OVERLAY_OFFSET +
                                     overlay_index);

    uint32_t w = config->viewport_width;
    uint32_t h = config->viewport_height;
    if (w == 0u || h == 0u) { w = 1280u; h = 720u; }

    /* WHAT SURVIVES UNDERNEATH.  A skybox clear is refused rather than
     * honoured: a sky drawn by an overlay covers the base camera entirely,
     * which is not a stack -- it is a replacement.  Unity refuses the same
     * thing at the same place. */
    uint8_t mode = config->camera_clear_mode;
    if (mode == (uint8_t)JCE_CAMERA_CLEAR_SKYBOX) {
        static bool warned_sky = false;
        if (!warned_sky) {
            warned_sky = true;
            LOG_WARN(LOG_TAG,
                     "camera stack: an overlay camera cannot clear to the "
                     "skybox (it would cover the base camera); drawing it as "
                     "depth-only");
        }
        mode = (uint8_t)JCE_CAMERA_CLEAR_DEPTH_ONLY;
    }
    uint16_t clear_flags = 0u;
    if (mode == (uint8_t)JCE_CAMERA_CLEAR_COLOR)
        clear_flags = BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH;
    else if (mode == (uint8_t)JCE_CAMERA_CLEAR_DEPTH_ONLY)
        clear_flags = BGFX_CLEAR_DEPTH;
    /* JCE_CAMERA_CLEAR_NOTHING keeps 0: the overlay shares the base camera's
     * depth and can be occluded by it, which is the whole point of that mode. */

    bgfx_frame_buffer_handle_t fb = { config->scene_frame_buffer };
    bgfx_set_view_frame_buffer(view, fb);
    bgfx_set_view_rect(view, 0, 0, (uint16_t)w, (uint16_t)h);
    bgfx_set_view_clear(view, clear_flags, 0x000000ff, 1.0f, 0);

    const float aspect = (h > 0u) ? ((float)w / (float)h) : 1.0f;
    jce_mat4 v = jce_camera_view((JceCamera *)camera);
    jce_mat4 p = jce_camera_proj((JceCamera *)camera, aspect,
                                 bgfx_get_caps()->homogeneousDepth);
    bgfx_set_view_transform(view, JCE_M4_PTR(v), JCE_M4_PTR(p));

    /* The draw is the ORDINARY scene render, reduced.  Not a second
     * implementation of the mesh walk: the overlay has to agree with the base
     * camera about materials, lighting, skinning and instancing, and the only
     * way to guarantee that is to be the same code. */
    JceSceneRenderConfig cfg = *config;
    cfg.reduced_pass     = true;   /* see JceSceneRenderConfig::reduced_pass */
    cfg.draw_skybox      = false;  /* refused above; this is the enforcement */
    cfg.apply_postfx     = false;  /* the base camera's chain runs after us */
    cfg.draw_shadows     = false;  /* the base render's atlas is still bound */
    cfg.viewport_width   = w;
    cfg.viewport_height  = h;
    /* Its own viewport slot, so an overlay's previous-frame camera never
     * overwrites the base viewport's (JCE_SR_VIEWPORT_SLOTS names who owns
     * which).  Sharing one would be safe only for as long as reduced_pass
     * keeps TAA off, which is a property of another field. */
    cfg.viewport_id      = JCE_SR_VIEWPORT_SLOT_CAMERA_OVERLAY + overlay_index;
    cfg.on_after_sky     = NULL;
    cfg.on_after_sky_ud  = NULL;
    cfg.camera_overlay_count = 0u;  /* an overlay does not stack further */

    jce_scene_renderer_render(sr, scene, camera, view, dt_sec, &cfg);
}
