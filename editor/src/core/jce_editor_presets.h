/*
 * jce_editor_presets.h  Component preset save / apply / list.
 *
 * Presets are per-component-type binary blobs stored under
 *   <project_root>/presets/<flag_name>/<preset_name>.preset
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

/* Save the named entity's component (identified by `flag`) as a preset. */
bool jce_preset_save(uint64_t flag, const char *preset_name,
                     JceScene *scene, JceEntity e);

/* Apply a preset onto the named entity's component. Wraps the write in a
 * single undo transaction. Returns false if the file is missing,
 * malformed, or struct size mismatched. */
bool jce_preset_apply(uint64_t flag, const char *preset_name,
                      JceScene *scene, JceEntity e);

/* List preset names (filename without extension) available for a flag. */
std::vector<std::string> jce_preset_list(uint64_t flag);

/* Delete a preset file by name. */
bool jce_preset_delete(uint64_t flag, const char *preset_name);
