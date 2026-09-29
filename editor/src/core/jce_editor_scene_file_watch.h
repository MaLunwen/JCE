/*
 * jce_editor_scene_file_watch.h — notice when the open scene changes on disk.
 *
 * WHY THIS EXISTS.  An agent working through the Automation API writes a scene
 * into the project inside a changeset and then asks a human to look at it
 * before the changeset is committed (REQ-SCN-04).  Until this file, the human
 * had to know to reopen the scene by hand: the editor read the file once, at
 * open, and never looked again.  A preview nobody is told to refresh is a
 * preview of the previous answer, and approving it approves something that was
 * never seen.
 *
 * WHAT IT WILL NOT DO.  It does not reload over unsaved work.  The editor's
 * edits live in jce_editor_history and the agent's live in a changeset; those
 * are two provenance systems and nothing has decided which owns undo.  When
 * both have something to say about the same file this warns and stands still,
 * because the two alternatives are worse in ways that are hard to see:
 * reloading silently deletes human edits that no undo stack can return, and
 * ignoring silently shows a stale scene that the human is about to approve.
 *
 * SELF-WRITES ARE NOT CHANGES.  Saving the scene moves its mtime, so the save
 * path re-stamps through jce_editor_scene_file_watch_restamp() -- otherwise
 * the editor reloads the file it just wrote, dropping the selection for no
 * reason anybody watching could explain.
 */
#ifndef JCE_EDITOR_SCENE_FILE_WATCH_H
#define JCE_EDITOR_SCENE_FILE_WATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Call once per editor frame.  Cheap: one mtime stat every
 * JCE_SCENE_WATCH_INTERVAL_FRAMES frames, and nothing at all while a scene
 * load is in flight or the current scene has no file behind it. */
void jce_editor_scene_file_watch_poll(void);

/* Adopt what is on disk NOW as the baseline, without reloading.  Called after
 * the editor itself loads or saves the scene. */
void jce_editor_scene_file_watch_restamp(void);

/* True when the file changed on disk and the reload was WITHHELD because the
 * editor has unsaved edits.  The layout shows this; it is the one state in
 * which what the human sees is not what the file says. */
bool jce_editor_scene_file_watch_is_stale(void);

/* Reload from disk, discarding unsaved editor edits.  What the human chooses
 * when the warning above is showing. */
void jce_editor_scene_file_watch_reload_now(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_SCENE_FILE_WATCH_H */
