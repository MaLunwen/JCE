/*
 * api_graphics.h  Layer 3 — Graphics abstraction.
 *
 * Renderer, cameras, textures, meshes, models, materials, text,
 * primitives, lighting, shaders, PBR, particles, post-processing.
 * Built on bgfx for cross-backend rendering.
 */

#ifndef JCE_API_GRAPHICS_H
#define JCE_API_GRAPHICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/renderer/jce_camera.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_lighting.h>
#include <jce/renderer/jce_lighting_system.h>
#include <jce/renderer/jce_material.h>
#include <jce/renderer/jce_mesh.h>
#include <jce/renderer/jce_model.h>
#include <jce/renderer/jce_particles.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_shaders.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_views.h>



#ifdef __cplusplus
}
#endif
#endif /* JCE_API_GRAPHICS_H */
