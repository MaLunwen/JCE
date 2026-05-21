/*
 * jce_render_preview.h  Engine-owned helpers for in-editor preview
 *                       passes (material graph, asset thumbnails, ...).
 *
 * These helpers consolidate all PBR uniform/light/texture binding so
 * the editor can render a single, scene-consistent sphere/cube/plane
 * into an offscreen target with a couple of calls. They live in the
 * engine layer (not the editor) so the rendering path stays identical
 * to runtime — same shader programs, same sampler stages, same light
 * uniform layout. The editor only owns the offscreen target and the
 * scratch material; this helper does the actual draw.
 *
 * Layer: Graphics (Layer 3) — public API.
 */

#ifndef JCE_RENDER_PREVIEW_H
#define JCE_RENDER_PREVIEW_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_gfx_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRenderer     JceRenderer;
typedef struct JcePbrMaterial  JcePbrMaterial;

/* Render a unit sphere centred at the origin into the currently-bound
 * view's framebuffer, fully PBR-lit with a deterministic 1-directional
 * + ambient light environment and IBL explicitly disabled.
 *
 * Caller is responsible for setting up the view's framebuffer and
 * view/proj matrices BEFORE calling this — typically by invoking
 * jce_offscreen_target_prepare() on an editor-owned offscreen target.
 *
 * Parameters:
 *   r                 — renderer instance (must be valid).
 *   view_id           — bgfx view id the sphere will submit into.
 *                       Use JCE_VIEW_EDITOR_PREVIEW for the
 *                       material-graph preview pane.
 *   eye_pos[3]        — world-space camera position. Used to drive the
 *                       specular highlight direction (u_cameraPos).
 *                       Must match the view matrix passed to the
 *                       offscreen target.
 *   mat               — material to bind. NULL = default white PBR.
 *   program_override  — explicit fs/vs program to use instead of the
 *                       renderer's default PBR program. Pass
 *                       JCE_INVALID_SHADER to use the default PBR
 *                       program (same as runtime). The shader-graph
 *                       "Compile & Bind" path passes the runtime-
 *                       compiled program here.
 *
 * Returns false if internal resources (sphere mesh, uniforms, light
 * env) could not be lazily created — in that case the editor should
 * fall back to its ImGui-circles placeholder. */
JCE_API bool jce_render_preview_sphere(const JceRenderer      *r,
                                        uint16_t                view_id,
                                        const float             eye_pos[3],
                                        const JcePbrMaterial   *mat,
                                        JceShaderHandle         program_override);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_PREVIEW_H */
