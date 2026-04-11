/*
 * jce_editor_style.h  Unified editor UI style and font loading.
 */

#ifndef JCE_EDITOR_STYLE_H
#define JCE_EDITOR_STYLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/core/pak_loader.h>

/* Theme indices. */
#define JCE_THEME_DARK   0
#define JCE_THEME_LIGHT  1
#define JCE_THEME_SSMS   2
#define JCE_THEME_COUNT  3

/* Apply the JCE dark editor theme to ImGui. */
void jce_editor_setup_style(void);

/* Apply a theme by index (0=Dark, 1=Light, 2=SSMS). */
void jce_editor_apply_theme(int theme_idx);

/* Get current theme index. */
int  jce_editor_get_theme(void);

/* Load JCE.ttf from PAK and set it as the default font.
   Must be called before jce_imgui_bgfx_rebuild_fonts(). */
bool jce_editor_load_fonts(const PakArchive *pak, float size_pixels);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_STYLE_H */
