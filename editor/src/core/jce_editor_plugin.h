/*
 * jce_editor_plugin.h  Compile-time / static plugin panel registry.
 *
 * Lightweight extension surface for adding custom editor panels without
 * modifying the hardcoded panel enum.  Game code (or a future dlopen
 * loader) calls jce_editor_plugin_register_panel() once at editor
 * startup; the main loop then iterates registered panels alongside the
 * built-in ones.
 *
 * This first cut is compile-time only: panels live in the same binary
 * as the editor and register from a known init function.  Dlopen-style
 * dynamic loading is deferred — same registry table will satisfy that
 * later (just call register from the .so's constructor).
 *
 * Naming model: each plugin panel has a stable string id (e.g.
 * "studio.metrics") which doubles as the menu item label fallback if
 * `display_name` is null and as the persistence key for visibility.
 *
 * Layer: editor (UI shell).
 */

#ifndef JCE_EDITOR_PLUGIN_H
#define JCE_EDITOR_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*JceEditorPluginPanelFn)(void);

typedef struct {
    const char            *id;             /* required, non-empty, unique */
    const char            *display_name;   /* falls back to id when NULL */
    JceEditorPluginPanelFn content_fn;     /* required */
    bool                   default_visible;
    /* Reserved for future use — set to 0 today. */
    uint32_t               flags;
} JceEditorPluginPanel;

/* Register a plugin panel.  Returns false if the registry is full or
 * the id collides with an existing entry.  Subsequent registrations
 * with the same id replace the existing entry (hot-reload friendly). */
bool jce_editor_plugin_register_panel(const JceEditorPluginPanel *desc);

/* Unregister a panel by id.  Returns true if removed. */
bool jce_editor_plugin_unregister_panel(const char *id);

/* Iterate.  Use jce_editor_plugin_panel_count + jce_editor_plugin_panel_at
 * to render every panel without exposing internal storage. */
uint32_t                    jce_editor_plugin_panel_count(void);
const JceEditorPluginPanel *jce_editor_plugin_panel_at(uint32_t idx);

/* Visibility helpers — visibility is persisted alongside the
 * built-in panels.  Returns NULL if id is unknown. */
bool *jce_editor_plugin_panel_visible_ptr(const char *id);

/* Render every registered plugin panel as a top-level ImGui Window
 * (only the ones whose visibility flag is true).  Call once per frame
 * from the editor main loop after the built-in panels have drawn. */
void jce_editor_plugin_render_all(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PLUGIN_H */
