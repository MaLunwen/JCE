/*
 * jce_editor_style.h  Unified editor UI style and font loading.
 */

#ifndef JCE_EDITOR_STYLE_H
#define JCE_EDITOR_STYLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <jce/resource/jce_pak_loader.h>

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

/* Load fonts and set the default. Resolution priority for each font:
     1. user override path (if non-NULL and file exists)
     2. system font (e.g. Ink Free / KaiTi installed on the host OS)
   No fonts are bundled — if both lookups fail, the editor falls back to
   ImGui's built-in proggy font for that role.
   Override paths may be NULL or empty.
   Must be called before jce_imgui_renderer_rebuild_fonts(). */
bool jce_editor_load_fonts(const JcePakArchive *pak, float size_pixels,
                           const char *en_override,
                           const char *zh_override);

/* Discoverable font entry returned by jce_editor_enumerate_fonts(). */
typedef struct {
    char display_name[128]; /* e.g. "Ink Free", "Microsoft YaHei", "msgothic.ttc" */
    char path[1024];        /* absolute filesystem path                          */
} JceFontEntry;

/* Enumerate font files available on the host system by scanning a small
   set of well-known per-platform directories. Picks up *.ttf / *.otf /
   *.ttc files. Pure runtime detection (jce_platform_name + getenv);
   no #ifdef branches.

   Writes up to `max_entries` records into `out` and returns the count
   actually written. Sorted alphabetically by display_name. */
int  jce_editor_enumerate_fonts(JceFontEntry *out, int max_entries);

/* Defer a font reload to the next frame (call from inside an ImGui frame
   to avoid corrupting the active draw list / atlas). Strings are copied. */
void jce_editor_request_font_reload(float size_pixels,
                                    const char *en_override,
                                    const char *zh_override);

/* Editor main loop calls this BEFORE ImGui::NewFrame() — applies any
   pending reload requested via jce_editor_request_font_reload(). */
void jce_editor_apply_pending_font_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_STYLE_H */
