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
 *   (from typing, from a browse result that just landed, or from an
 *   asset dropped onto the field).
 *
 *   The helper owns its own per-label static ready/cancel flags so
 *   callers do not have to wire up the async pump plumbing.
 *
 *   DRAG-DROP IS OWNED BY THIS WIDGET.  It installs the
 *   JCE_DND_ASSET_PATH target on the text field itself and normalises
 *   the dropped path (project-relative for AssetVfs, verbatim for the
 *   *Abs kinds).  Call sites must NOT bolt an extra accept_asset_drop()
 *   on afterwards: by then ImGui's "last item" is the trailing browse /
 *   clear button, so the extra target lands on a button, duplicates the
 *   handling, and mutates the buffer without the widget's return value
 *   reporting the change.  There is deliberately no opt-out flag — every
 *   path field in the editor should accept asset drops, and no call site
 *   has ever wanted otherwise.
 *
 *   Call sites that need EXTRA work on drop (e.g. the MeshRenderer's
 *   "dropping a model also imports its material") cannot express that
 *   here yet — see the note in jce_path_input.cpp.
 */

#ifndef JCE_PATH_INPUT_H
#define JCE_PATH_INPUT_H

#include <jce/middleware/scene/jce_scene.h>
#include <stdio.h>
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

    /* Optional: receives the RAW dropped path when this frame's edit came from
       a drag-and-drop, and is left untouched otherwise (so test it for '\0').
       The widget consumes the JCE_DND_ASSET_PATH payload itself, so a call
       site that must react to a drop with more than "store the path" — e.g.
       MeshRenderer importing a dropped model's materials, or reloading a
       dropped .mat.json — cannot see the payload and, crucially, cannot get
       the ABSOLUTE host path an importer needs (`buf` holds the relativized
       form).  Point this at a scratch buffer to get it.
       Do NOT add a second BeginDragDropTarget at the call site: by then
       ImGui's last item is the trailing browse/clear button, so the target
       lands on a button and the edit bypasses the widget's return value. */
    char  *dropped_raw = nullptr;
    size_t dropped_raw_size = 0;
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

/* As above, but also reports the RAW (absolute) path when the edit came from a
   drag-and-drop.  `dropped_raw` is left untouched otherwise, so zero it first
   and test dropped_raw[0].  Use this when the drop must trigger more than
   storing the path — see JcePathInputOpts::dropped_raw. */
inline bool jce_draw_path_input_asset_dnd(const char *label, char *buf, size_t sz,
                                          char *dropped_raw, size_t dropped_raw_sz,
                                          int asset_kind = 0) {
    JcePathInputOpts o;
    o.asset_kind        = asset_kind;
    o.dropped_raw       = dropped_raw;
    o.dropped_raw_size  = dropped_raw_sz;
    return jce_draw_path_input(label, buf, sz, JcePathKind::AssetVfs, &o);
}

/* Interned-field forms.
 *
 * Component asset paths are interned pointers (see jce_str_intern.h), so the
 * in-place editors above cannot be pointed at them: the string is shared with
 * every other component naming the same asset. These copy the field into a
 * scratch buffer, run the normal widget, and re-intern only when the user
 * actually changed something -- an untouched field keeps its pointer and adds
 * nothing to the pool. */
inline bool jce_draw_path_input_asset_interned(const char *label,
                                               JceScene *scene,
                                               const char **field,
                                               int asset_kind = 0) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", (field && *field) ? *field : "");
    if (!jce_draw_path_input_asset(label, buf, sizeof(buf), asset_kind))
        return false;
    if (field) *field = jce_scene_intern(scene, buf);
    return true;
}

inline bool jce_draw_path_input_asset_dnd_interned(const char *label,
                                                   JceScene *scene,
                                                   const char **field,
                                                   char *dropped_raw,
                                                   size_t dropped_raw_sz,
                                                   int asset_kind = 0) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", (field && *field) ? *field : "");
    if (!jce_draw_path_input_asset_dnd(label, buf, sizeof(buf),
                                       dropped_raw, dropped_raw_sz, asset_kind))
        return false;
    if (field) *field = jce_scene_intern(scene, buf);
    return true;
}

#endif /* JCE_PATH_INPUT_H */
