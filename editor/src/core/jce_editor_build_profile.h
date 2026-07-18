/*
 * jce_editor_build_profile.h  Per-project build/run profile overlay.
 *
 * The build/run profile (external game exe + working dir + target name +
 * cmake presets + output path + run mode) is MACHINE-LOCAL but
 * per-PROJECT: switching projects in one session used to clobber the
 * previous project's profile (global-only storage), so project B would
 * try to launch project A's exe.  These helpers mirror the profile to the
 * per-project store (<root>/.jce/editor-state.json) so each project keeps
 * its own, while the global JceEditorConfig fields stay the live values
 * every existing reader already uses.
 *
 * Symmetric around set_current_project_root: snapshot the OUTGOING
 * project's profile before the root changes, restore (or seed) the
 * INCOMING project's after.
 */

#ifndef JCE_EDITOR_BUILD_PROFILE_H
#define JCE_EDITOR_BUILD_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot the current global build/run profile into the per-project
 * store.  Call while s_current_project_root still holds the project the
 * fields belong to (i.e. BEFORE switching roots).  No-op when no project
 * store is active. */
void jce_editor_build_profile_snapshot(void);

/* Overlay the (now-current) project's saved build/run profile onto the
 * global JceEditorConfig, or seed the project store from the current
 * global values on first sight.  Call AFTER the project store follows the
 * new root.  No-op when no project store is active. */
void jce_editor_build_profile_restore(void);

/* Asset-browser "Locations" favourites are the same class of state
 * (machine-local, per-project): project A's pinned folders must not show
 * while browsing project B.  Snapshot before a root switch, restore after.
 * First-sight seeding copies in ONLY the global favourites that live under
 * this project's root (a favourite pointing into another project is not
 * inherited). */
void jce_editor_favorites_snapshot(void);
void jce_editor_favorites_restore(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_BUILD_PROFILE_H */
