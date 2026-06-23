/*
 * jce_editor_presets.h  Component preset save / apply / list.
 *
 * Presets are per-component-type binary blobs stored under
 *   <project_root>/presets/<component_name>/<preset_name>.preset
 * where <component_name> is the CANONICAL engine registry name
 * (jce_component_name: "MeshRenderer", "NavAgent", ...).  Pre-registry
 * presets were filed under the editor display name ("Mesh Renderer");
 * reads fall back to that directory so existing user presets keep loading.
 *
 * Files are tightly bound to the component struct layout and engine
 * version; mismatched sizes are rejected on load.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

struct JceScene;
typedef uint64_t JceEntity;

/* Save the named entity's component (identified by its dense registry
 * `comp_id`, see jce_component_registry.h) as a preset. */
bool jce_preset_save(int comp_id, const char *preset_name,
                     JceScene *scene, JceEntity e);

/* Apply a preset onto the named entity's component. Wraps the write in a
 * single undo transaction. Returns false if the file is missing,
 * malformed, or struct size mismatched. */
bool jce_preset_apply(int comp_id, const char *preset_name,
                      JceScene *scene, JceEntity e);

/* List preset names (filename without extension) available for a
 * component type (union of the canonical and legacy directories). */
std::vector<std::string> jce_preset_list(int comp_id);

/* Delete a preset file by name (from both directories when present). */
bool jce_preset_delete(int comp_id, const char *preset_name);
