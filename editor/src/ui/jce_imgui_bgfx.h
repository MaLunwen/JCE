/*
 * jce_imgui_bgfx.h  ImGui renderer backend for bgfx.
 *
 * Renders ImGui draw data using bgfx transient buffers on JCE_VIEW_IMGUI.
 * Requires bgfx to be initialized before calling init.
 */

#ifndef JCE_IMGUI_BGFX_H
#define JCE_IMGUI_BGFX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/os/core/pak_loader.h>

/* Initialize the bgfx renderer backend for ImGui.
   Loads the imgui shader from PAK, creates font atlas texture,
   and sets up vertex layout.
   Must be called after bgfx_init() and ImGui::CreateContext(). */
bool jce_imgui_bgfx_init(const JcePakArchive *pak, uint8_t view_id);

/* Destroy all bgfx resources (shader, texture, uniform). */
void jce_imgui_bgfx_shutdown(void);

/* Set up the view for this frame (ortho projection, viewport). */
void jce_imgui_bgfx_setup_view(uint16_t width, uint16_t height);

/* Render ImGui draw data to bgfx.
   Call after ImGui::Render(). */
void jce_imgui_bgfx_render_draw_data(void);

/* Rebuild the font atlas texture (call after adding fonts). */
void jce_imgui_bgfx_rebuild_fonts(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_IMGUI_BGFX_H */
