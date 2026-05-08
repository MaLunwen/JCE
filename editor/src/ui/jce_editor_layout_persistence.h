/*
 * jce_editor_layout_persistence.h  Save / load named editor layouts.
 *
 * Stores ImGui layout INI snapshots under <project>/.jce/layouts/<name>.ini
 * so each project keeps its own per-user-named workspaces.  Mirrors
 * Unity's "Save Layout" / "Load Layout" / "Delete Layout" menu.
 *
 * The active ImGui state (window positions / dock node tree / panel
 * visibility) is what gets serialised — when saved, the file is a
 * complete imgui.ini.  When loaded, the entire layout is replaced
 * (matching Unity's semantics).
 */

#ifndef JCE_EDITOR_LAYOUT_PERSISTENCE_H
#define JCE_EDITOR_LAYOUT_PERSISTENCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persist the current ImGui layout to disk under `name` (no extension;
 * caller-supplied identifier).  Returns true on success. */
bool jce_editor_layout_save(const char *name);

/* Load a previously-saved layout by name.  Applied immediately —
 * window/dock layout is reset to the saved snapshot.  Returns true on
 * success. */
bool jce_editor_layout_load(const char *name);

/* Delete a saved layout.  Returns true if the file was removed (or did
 * not exist). */
bool jce_editor_layout_delete(const char *name);

/* Enumerate all saved layouts.  out_names points to caller-owned
 * char[max_count][64].  Returns the count actually written. */
uint32_t jce_editor_layout_list(char (*out_names)[64], uint32_t max_count);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_LAYOUT_PERSISTENCE_H */
