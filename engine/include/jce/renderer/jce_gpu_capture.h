/*
 * jce_gpu_capture.h  Engine-side GPU frame-capture trigger (RenderDoc).
 *
 * Debug-only diagnostics hook shared by every JCE consumer (editor, games,
 * headless QA). When the process is launched under a GPU debugger that injects
 * the RenderDoc in-application API (e.g. `renderdoccmd capture <exe>`), setting
 * JCE_RDOC_FRAME=N asks RenderDoc to capture the full GPU frame once the
 * renderer reaches frame N. Inspect the resulting .rdc to see exact draw state /
 * bound resources on any backend (D3D11/D3D12/Vulkan/OpenGL).
 *
 * Compiled to no-ops unless the build enables JCE_RENDERDOC_ENABLED (forced OFF
 * for dist builds, mirroring Tracy), so shipped games carry zero RenderDoc code.
 * The RenderDoc module is located through the jce_library shared-object
 * abstraction; no platform headers leak into the renderer layer.
 */

#ifndef JCE_GPU_CAPTURE_H
#define JCE_GPU_CAPTURE_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Advance the capture frame counter and, if this is the armed target frame
 * (JCE_RDOC_FRAME=N), ask the injected RenderDoc API to capture it. Call once
 * per rendered frame, right before the backbuffer swap. No-op when the build
 * disables RenderDoc, when JCE_RDOC_FRAME is unset, or when no RenderDoc API is
 * injected. Fires at most once per process. */
JCE_API void JCE_CALL jce_gpu_capture_tick(void);

JCE_EXTERN_C_END

#endif /* JCE_GPU_CAPTURE_H */
