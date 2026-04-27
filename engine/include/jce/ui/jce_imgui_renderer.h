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
#include <jce/os/core/jce_pak_loader.h>

#ifdef __cplusplus
extern "C" {
#endif

JCE_API bool jce_imgui_renderer_init(const JcePakArchive *pak,
                                     uint8_t              view_id);

JCE_API void jce_imgui_renderer_shutdown(void);

JCE_API void jce_imgui_renderer_setup_view(uint16_t width, uint16_t height);

JCE_API void jce_imgui_renderer_draw(void);

JCE_API void jce_imgui_renderer_rebuild_fonts(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_IMGUI_RENDERER_H */
