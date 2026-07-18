/*
 * jce_editor_json_reveal.h  Jump from live editor objects to their JSON
 * source text in the Code Viewer.
 *
 * Backs the Hierarchy right-click "View in Scene JSON" / "View Referenced
 * JSON" actions and the JCE_DBG_VIEWJSON headless QA hook; kept public so
 * future entry points (inspector header, search results) reuse the same
 * locate + open + focus path.
 */

#ifndef JCE_EDITOR_JSON_REVEAL_H
#define JCE_EDITOR_JSON_REVEAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Locate `entity_id`'s block in the CURRENT scene's saved JSON and open the
 * Code Viewer there (line highlighted + scrolled into view).  Located by
 * entity NAME (ids remap on load), disambiguated by live id when names
 * collide.  Returns false when there is no scene file (unsaved / bundle)
 * or the entity block was not found — the file still opens at line 1 in
 * the not-found case. */
bool jce_editor_reveal_entity_in_scene_json(uint32_t entity_id);

/* Open `asset_path` (asset-relative or absolute) as raw text in the Code
 * Viewer at 1-based `line` (<= 0 = top) and focus the panel. */
void jce_editor_reveal_json_source(const char *asset_path, int line);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_JSON_REVEAL_H */
