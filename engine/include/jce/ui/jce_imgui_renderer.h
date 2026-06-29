/*
 * jce_imgui_renderer.h  ImGui renderer backend (bgfx-backed).
 *
 * Renders ImGui draw data via the JCE renderer on the JCE_VIEW_IMGUI view.
 * Must be initialized after the renderer + ImGui::CreateContext().
 *
 * Loads the imgui shader from the supplied PAK archive.
 */

#ifndef JCE_IMGUI_RENDERER_H
#define JCE_IMGUI_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <jce/os/core/jce_defs.h>
#include <jce/resource/jce_pak_loader.h>

#ifdef __cplusplus
extern "C" {
#endif

JCE_API bool jce_imgui_renderer_init(const JcePakArchive *pak,
                                     uint8_t              view_id);

JCE_API void jce_imgui_renderer_shutdown(void);

JCE_API void jce_imgui_renderer_setup_view(uint16_t width, uint16_t height);

JCE_API void jce_imgui_renderer_draw(void);

JCE_API void jce_imgui_renderer_rebuild_fonts(void);

/* Request a whole-window screenshot to `path` (.png).  The next frame renders
 * the ImGui view into an offscreen FBO and reads it back (the backbuffer cannot
 * be screen-shot reliably on D3D flip-model swap chains — it comes back black).
 * Asynchronous: the PNG is written a couple of frames later by the renderer's
 * read-back poll.  One capture in flight at a time. */
JCE_API void jce_imgui_renderer_request_capture(const char *path);

/* Enable/disable whole-window video recording.  While on, the UI is rendered a
 * second time into an offscreen FBO and read back into the renderer's capture
 * sink each frame (the backbuffer cannot be captured on D3D flip-model swap
 * chains).  Pair with jce_renderer_set_capture_imgui_mode(true). */
JCE_API void jce_imgui_renderer_set_recording(bool on);

#ifdef __cplusplus
}
#endif

#endif /* JCE_IMGUI_RENDERER_H */
