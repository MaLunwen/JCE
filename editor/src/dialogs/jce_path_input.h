/*
 * jce_path_input.h  Unified path-input widget for the editor.
 *
 *   InputText + Browse button in one call.  Eliminates manual-typed
 *   path errors and centralizes all native file/folder dialogs and
 *   the in-editor project-asset picker.
 *
 *   Two semantics:
 *     FileAbs / FolderAbs / SaveFileAbs → absolute paths via the
 *         host OS file dialog (open_file_dialog_async etc.).
 *     AssetVfs → project-relative VFS path via the in-editor
 *         Asset Picker modal (jce_dialog_asset_picker).
 *
 *   Return value: true if the buffer's contents changed THIS frame
 *   (either from typing or from a browse result that just landed).
 *
 *   The helper owns its own per-label static ready/cancel flags so
 *   callers do not have to wire up the async pump plumbing.
 */

#ifndef JCE_PATH_INPUT_H
#define JCE_PATH_INPUT_H

#include <stddef.h>

enum class JcePathKind {
    FileAbs,        // any existing file on disk
    FolderAbs,      // any existing directory on disk
    SaveFileAbs,    // save dialog (file may not exist)
    AssetVfs,       // project asset, returned as project-relative VFS path
};

struct JcePathInputOpts {
    /* Title shown in the OS dialog (or asset picker).  NULL → use label. */
    const char *title = nullptr;
    /* For *Abs: Qt-style filter "Bundles (*.jbundle);;All Files (*.*)".
       For AssetVfs: an int cast of JceAssetKind to filter the picker,
       passed as the integer in `asset_kind` instead. */
    const char *filter = nullptr;
    /* AssetVfs only: 0 = ANY kind, otherwise JceAssetKind value. */
    int asset_kind = 0;
    /* Width of the InputText (in pixels).  0 = stretch to available. */
    float width = 0.0f;
    /* If true, the Browse button is drawn BEFORE InputText instead of
       after.  Default after. */
    bool button_first = false;
};

/* Draws label + InputText + "..." Browse button.
   `buf` must be a stable pointer for at least one frame after the
   browse button is clicked (the dialog writes back asynchronously). */
bool jce_draw_path_input(const char *label,
                         char *buf, size_t buf_size,
                         JcePathKind kind,
                         const JcePathInputOpts *opts = nullptr);

/* Convenience overloads with common defaults. */
inline bool jce_draw_path_input_file(const char *label, char *buf, size_t sz,
                                     const char *filter = nullptr) {
    JcePathInputOpts o; o.filter = filter;
    return jce_draw_path_input(label, buf, sz, JcePathKind::FileAbs, &o);
}
inline bool jce_draw_path_input_folder(const char *label, char *buf, size_t sz) {
    return jce_draw_path_input(label, buf, sz, JcePathKind::FolderAbs);
}
inline bool jce_draw_path_input_save(const char *label, char *buf, size_t sz,
                                     const char *filter = nullptr) {
    JcePathInputOpts o; o.filter = filter;
    return jce_draw_path_input(label, buf, sz, JcePathKind::SaveFileAbs, &o);
}
inline bool jce_draw_path_input_asset(const char *label, char *buf, size_t sz,
                                      int asset_kind = 0) {
    JcePathInputOpts o; o.asset_kind = asset_kind;
    return jce_draw_path_input(label, buf, sz, JcePathKind::AssetVfs, &o);
}

#endif /* JCE_PATH_INPUT_H */
