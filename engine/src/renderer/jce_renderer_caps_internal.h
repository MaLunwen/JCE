#ifndef JCE_RENDERER_CAPS_INTERNAL_H
#define JCE_RENDERER_CAPS_INTERNAL_H

#include <jce/renderer/jce_renderer_caps.h>

/* Renderer lifecycle bridge.
 *
 * THERE ARE TWO WAYS THE NEGOTIATED VERSION CAN ARRIVE, and the difference
 * matters because the first one is not always there:
 *
 *   1. capture_trace() — bgfx does not expose the version through any API, so
 *      conan/hooks/hook_bgfx_wasm_fix.py PATCHES renderer_gl.cpp and
 *      renderer_vk.cpp to BX_TRACE it, and jce_renderer.c forwards the tagged
 *      lines here.  This works only when the bgfx package in use was built
 *      with that patch: a package built before the hook existed emits nothing,
 *      the version is never observed, and nothing says so.
 *   2. publish_probe() — jce_renderer.c asks the driver directly, through a
 *      throwaway SDL context, before bgfx is initialised at all.  Independent
 *      of which bgfx binary is installed, and it runs BEFORE the attempt, so
 *      an unusable backend is skipped instead of being initialised and then
 *      rejected.
 *
 * Do not assume bgfx enforces its own floor.  It does not: measured
 * 2026-09-01, BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION appears ZERO times in
 * renderer_gl.cpp, `m_gles3` is a compile-time constant off Emscripten, and
 * BGFX_RENDERER_OPENGL_NAME ("OpenGL 3.3") is a compile-time string built from
 * the floor -- not a reading.  bgfx_init() succeeds on a context it cannot
 * serve and the shaders fail later. */
void jce_renderer_caps_api_begin_attempt(JceRendererBackend backend);
void jce_renderer_caps_api_capture_trace(const char *line);

/* Record a version read straight from the driver AND answer whether it clears
 * the build tier's floor.  `api` is a GL_VERSION-style string ("4.6.0 NVIDIA
 * ...", "OpenGL ES 3.2 ..."); `shading` is a GL_SHADING_LANGUAGE_VERSION-style
 * one ("4.60 NVIDIA") and may be NULL.  Publishing and deciding are one call
 * on purpose: split apart, the caller has to re-implement the comparison and
 * the two can disagree.
 *
 * Returns false only when a version WAS read and it is below the floor.  An
 * unreadable version returns true with a warning -- refusing a backend on a
 * driver that merely will not introduce itself is worse than trying it. */
bool jce_renderer_caps_api_publish_probe(JceRendererBackend backend,
                                         const char *api,
                                         const char *shading);

/* True when the active backend's observed version meets the build tier's
 * floor.  When the version could NOT be observed this returns true with a
 * warning, because refusing every backend on an unreadable driver is worse
 * than running one -- but it never claims the floor was checked. */
bool jce_renderer_caps_api_accept_active_backend(void);
void jce_renderer_caps_api_reset(void);

/* ================================================================== */
/* Vertex-stage texture fetch                                          */
/* ================================================================== */

/* INTERNAL, deliberately.  It was public for one build; nothing outside
 * engine/src calls it, and check_editor_consumption correctly reported
 * two more unconsumed symbols on the api_graphics umbrella the moment it
 * landed there.  A capability question the engine asks itself is not a
 * user-facing API.
 *
 * A CLEAR capability bit means "no" only if the backend sets that bit at
 * all.  bgfx's OpenGL renderer never sets BGFX_CAPS_FORMAT_TEXTURE_VERTEX --
 * for any format -- against two sites in D3D11 and one each in Vulkan and
 * D3D12.  Three separate features read it as a plain boolean and were
 * therefore OFF on every OpenGL machine regardless of hardware: FFT water
 * displacement, dynamic water ripples, and GPU crowd instancing.  Each
 * warned about a question the backend was never asked.
 *
 * The distinction is measurable rather than a hardcoded backend list: if no
 * format anywhere reports VERTEX, the field is unpopulated and its silence
 * is not an answer. */
typedef enum {
    JCE_GPU_VFETCH_NO = 0,      /* backend answers, and the answer is no  */
    JCE_GPU_VFETCH_YES,         /* backend answers yes                    */
    JCE_GPU_VFETCH_UNANSWERED   /* backend never populates the field      */
} JceGpuVertexFetch;

/* Formats the engine samples from a vertex shader. */
typedef enum {
    JCE_GPU_VFETCH_FMT_RGBA32F = 0,  /* FFT displacement, bone palette    */
    JCE_GPU_VFETCH_FMT_R32F,         /* water ripple height               */
    JCE_GPU_VFETCH_FMT_COUNT
} JceGpuVertexFetchFormat;

/* The raw three-valued answer.  Requires a live bgfx; returns
 * JCE_GPU_VFETCH_UNANSWERED when caps are unavailable. */
JceGpuVertexFetch jce_gpu_vertex_fetch_state(JceGpuVertexFetchFormat fmt);

/* The policy every call site wants: can the vertex stage sample `fmt`?
 * UNANSWERED resolves against the build floor -- desktop GL 3.1+ and
 * GLES 3.0+ both MANDATE vertex texture fetch and the JCE floor is exactly
 * those, so it proceeds.  Web is the exception and keeps its fallback:
 * float FILTERING in the vertex stage there needs OES_texture_float_linear,
 * and that is the platform the original guards were written for. */
bool jce_gpu_vertex_fetch_usable(JceGpuVertexFetchFormat fmt);


#endif /* JCE_RENDERER_CAPS_INTERNAL_H */
