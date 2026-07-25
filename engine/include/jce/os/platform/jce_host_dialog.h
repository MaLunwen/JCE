/*
 * jce_host_dialog.h  Native file/folder picker dialogs (async).
 *
 * Wraps SDL3's SDL_ShowOpenFileDialog / SDL_ShowOpenFolderDialog /
 * SDL_ShowSaveFileDialog so editor/game code never includes Win32
 * <shlobj.h> / Cocoa NSOpenPanel / GTK FileChooser directly.
 *
 * THREADING: callback thread is backend-defined.  Some SDL3 desktop
 * backends invoke it from a native dialog worker thread, so consumers must
 * marshal results to their owning thread before touching UI, config, ECS, or
 * other non-thread-safe state.  Do NOT block waiting for it.
 *
 * Layer: Platform (Layer 1 — depends on jce_core, uses SDL3 internally).
 */

#ifndef JCE_HOST_DIALOG_H
#define JCE_HOST_DIALOG_H


#include <jce/os/core/jce_defs.h>
JCE_EXTERN_C_BEGIN

/* Forward declare to avoid pulling SDL into every consumer. */
struct SDL_Window;
typedef struct JceWindow JceWindow;

/* Register the application's primary SDL_Window as the parent of all
   subsequent native dialogs.  This is required for stability on Windows:
   SDL3's IFileDialog implementation can crash on the dialog worker
   thread when no parent HWND is supplied (heap/COM teardown races).
   Pass NULL to clear (e.g. on shutdown).  Safe to call from any thread,
   but in practice only the main thread should set this. */
JCE_API void jce_host_dialog_set_parent_window(struct SDL_Window *window);

/* Convenience: same as set_parent_window but takes a JceWindow.  The
   engine knows how to extract the underlying SDL_Window. */
JCE_API void jce_host_dialog_set_parent_jce_window(JceWindow *window);

typedef enum {
    JCE_DIALOG_OK        = 0,
    JCE_DIALOG_CANCELLED = 1,
    JCE_DIALOG_ERROR     = 2
} JceDialogResult;

/* Single-path callback.
   - On JCE_DIALOG_OK:        path is non-NULL and valid for the call.
   - On JCE_DIALOG_CANCELLED: path is NULL.
   - On JCE_DIALOG_ERROR:     path is NULL (use SDL_GetError if needed). */
typedef void (*JceDialogPathCallback)(void *user,
                                      JceDialogResult result,
                                      const char *path);

/* Open a folder picker.
   - title:        dialog title (may be NULL for system default).
   - default_path: starting directory (may be NULL or "").
   - cb:           result callback (must be non-NULL).
   - user:         opaque pointer passed back to cb. */
JCE_API void jce_host_dialog_pick_folder(const char *title,
                                 const char *default_path,
                                 JceDialogPathCallback cb,
                                 void *user);

/* Open a file picker.
   - filters: optional Qt-style filter string e.g.
       "Scenes (*.scn);;All Files (*.*)"
       Pass NULL for no filtering.  Parsed into SDL_DialogFileFilter[]. */
JCE_API void jce_host_dialog_pick_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user);

/* Show a save-file dialog.  Same filter syntax as pick_file. */
JCE_API void jce_host_dialog_save_file(const char *title,
                               const char *default_path,
                               const char *filters,
                               JceDialogPathCallback cb,
                               void *user);

JCE_EXTERN_C_END

#endif /* JCE_HOST_DIALOG_H */
