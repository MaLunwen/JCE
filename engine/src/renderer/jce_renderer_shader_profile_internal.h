/* Shared build-policy/actual-backend selection; not a hardware capability claim. */
#ifndef JCE_RENDERER_SHADER_PROFILE_INTERNAL_H
#define JCE_RENDERER_SHADER_PROFILE_INTERNAL_H
#include <jce/renderer/jce_renderer_caps.h>
#include <bgfx/c99/bgfx.h>

static inline bool jce_renderer_shader_profile_compatibility(
    const JceRendererApiInfo *api, bgfx_renderer_type_t backend)
{
    const JceRendererBackend actual = backend == BGFX_RENDERER_TYPE_OPENGL
        ? JCE_BACKEND_OPENGL : backend == BGFX_RENDERER_TYPE_OPENGLES
        ? JCE_BACKEND_OPENGLES : JCE_BACKEND_AUTO;
    return api && jce_graphics_api_is_compatibility_gl_es(api->build_tier, actual);
}
#endif
