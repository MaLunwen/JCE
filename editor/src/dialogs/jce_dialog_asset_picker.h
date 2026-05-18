/*
 * jce_dialog_asset_picker.h  Modal in-editor project-asset selector.
 *
 *   Opens a modal that lists assets from jce_assetdb_*, with a kind
 *   filter and a fuzzy-text search.  Returns the picked project-
 *   relative VFS path via the provided output buffer.
 *
 *   API is asynchronous in spirit (matches the native dialog helpers'
 *   contract) but it actually completes within a few frames since the
 *   modal is an ImGui window we draw ourselves.
 *
 *   Usage:
 *     static bool ready=false, cancel=false;
 *     if (clicked_browse)
 *         jce_editor_asset_picker_open("Pick texture", JCE_ASSET_KIND_TEXTURE,
 *                                      out_buf, sizeof(out_buf),
 *                                      &ready, &cancel);
 *     jce_editor_asset_picker_draw();  // call once per frame
 *     if (ready) { ... ready=false; }
 */

#ifndef JCE_DIALOG_ASSET_PICKER_H
#define JCE_DIALOG_ASSET_PICKER_H

#include <stddef.h>

/* asset_kind: 0 → any, otherwise JceAssetKind enum value. */
void jce_editor_asset_picker_open(const char *title,
                                  int asset_kind,
                                  char *out_buf, size_t out_size,
                                  bool *ready_flag,
                                  bool *cancelled_flag);

/* Draws the modal if currently active.  Call once per frame. */
void jce_editor_asset_picker_draw(void);

#endif /* JCE_DIALOG_ASSET_PICKER_H */
