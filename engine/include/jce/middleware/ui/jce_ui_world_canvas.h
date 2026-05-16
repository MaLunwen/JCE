/*
 * jce_ui_world_canvas.h  World-space / camera-space canvas modes.
 *
 * Unity Canvas render-mode equivalents:
 *   SCREEN_SPACE_OVERLAY   — UI drawn on top, no depth
 *   SCREEN_SPACE_CAMERA    — UI drawn at a fixed distance from camera
 *   WORLD_SPACE            — UI placed in the scene like any 3D quad
 *
 * Holds the canvas's render-mode flag + the world transform used to
 * place its rect in 3D.  The B17 wire transforms UI quads through
 * this matrix before submission.
 *
 * Layer: middleware/ui (Layer 4) — public.
 */

#ifndef JCE_UI_WORLD_CANVAS_H
#define JCE_UI_WORLD_CANVAS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_WORLD_CANVAS_SCREEN_OVERLAY = 0,
    JCE_WORLD_CANVAS_SCREEN_CAMERA  = 1,
    JCE_WORLD_CANVAS_WORLD          = 2,
} JceWorldCanvasRenderMode;

typedef struct {
    JceWorldCanvasRenderMode mode;

    /* World-space settings (mode == WORLD). */
    float position[3];
    float rotation_euler_deg[3];
    /* UI pixel → world unit scale.  Default 0.01 = 100 px ⇒ 1 unit. */
    float pixel_scale;
    /* Reference resolution in pixels.  Layout uses this as the canvas
     * "design size"; actual render scales by pixel_scale. */
    float reference_w;
    float reference_h;

    /* Camera-space setting (mode == SCREEN_CAMERA). */
    float plane_distance;
    /* Optional camera entity id this canvas tracks (0 = main camera). */
    uint64_t camera_entity;

    /* Sort order across multiple canvases (higher draws on top). */
    int sort_order;
} JceWorldCanvasComponent;

/* Build the world-space TRS matrix used to place a WORLD canvas's
 * rect.  `out_mat4` is column-major 16 floats.  For non-WORLD modes
 * this fills identity. */
JCE_API void jce_canvas_build_world_matrix(const JceWorldCanvasComponent *c,
                                             float out_mat4[16]);

/* Hit-test: project a world-space ray onto the canvas plane and
 * return the resulting (u, v) in [0,1] coordinates.  Returns false
 * if the ray misses the plane or the canvas is not in WORLD mode. */
JCE_API bool jce_canvas_world_pick(const JceWorldCanvasComponent *c,
                                     const float ray_origin[3],
                                     const float ray_dir[3],
                                     float *out_u,
                                     float *out_v);

JCE_EXTERN_C_END

#endif /* JCE_UI_WORLD_CANVAS_H */
