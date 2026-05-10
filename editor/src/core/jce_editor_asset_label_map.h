/*
 * jce_editor_asset_label_map.h  Editor-side asset → label-mask map.
 *
 * Holds (asset_path → uint64 label_bits) authored in the editor, plus
 * load/save to <project>/.jce/asset_labels.json.  The runtime registry
 * picks up these labels on asset load (a future hookup).  This module
 * just owns the data + a lookup API for the editor UI.
 */

#ifndef JCE_EDITOR_ASSET_LABEL_MAP_H
#define JCE_EDITOR_ASSET_LABEL_MAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get/set label bits for an asset path.  Setting 0 clears the entry. */
uint64_t jce_editor_asset_labels_get(const char *asset_path);
bool     jce_editor_asset_labels_set(const char *asset_path, uint64_t bits);

/* Persistence — saves under <project>/.jce/asset_labels.json. */
bool     jce_editor_asset_labels_save(void);
bool     jce_editor_asset_labels_load(void);

uint32_t jce_editor_asset_labels_entry_count(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_ASSET_LABEL_MAP_H */
