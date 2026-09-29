/*
 * jce_editor_layout_internal.h — what the layout TU and the menu bar share.
 *
 * WHY THIS EXISTS.  jce_editor_layout.cpp was 3,461 lines, past the 3,000-line
 * cap and frozen by tools/lint/check_file_size.py, so every addition to it
 * had to be paid for by a removal.  draw_menu_bar() was 757 of those lines and
 * is what the size gate's own message asks for: "move the addition into a new
 * translation unit".
 *
 * It could not simply be cut out.  The menu bar OPENS modals whose bodies are
 * drawn by the layout TU, so ten flags are written in one file and read in the
 * other -- s_show_save_as is touched once in the menu bar and seven times
 * outside it.  Sharing them through this header is the boundary that made the
 * split possible, and it is deliberately the SMALLEST one that works: ten
 * flags, six helpers and the two enums their signatures need.  Nothing else in
 * either file became visible.
 *
 * The flags keep their s_ prefix on purpose.  Renaming them to drop it would
 * have touched ~80 call sites for no behavioural gain, and a refactor whose
 * diff is mostly renames is one nobody can review for behaviour.
 */

#ifndef JCE_EDITOR_LAYOUT_INTERNAL_H
#define JCE_EDITOR_LAYOUT_INTERNAL_H

#include <cstddef>   /* NULL, for request_gated_action's default argument */

/* Deferred until the user answers "save your changes?" -- a quit, or one of
 * the scene-swap actions. */
typedef enum {
    PGA_NONE = 0,
    PGA_QUIT,
    PGA_NEW_SCENE,
    PGA_OPEN_SCENE,
    PGA_OPEN_PROJECT,
    PGA_OPEN_RECENT_SCENE,
    PGA_OPEN_RECENT_PROJECT,
} PendingGatedAction;

typedef enum {
    SAVE_SCENE_RESULT_FAILED = 0,
    SAVE_SCENE_RESULT_OK,
    SAVE_SCENE_RESULT_NEEDS_PATH,
} SaveSceneResult;

/* Modal visibility, owned by jce_editor_layout.cpp.  The menu bar sets them;
 * the layout TU draws and clears them. */
extern bool s_show_about;
extern bool s_show_build;
extern bool s_show_bundles;
extern bool s_show_new_project;
extern bool s_show_open_bundle;
extern bool s_show_save_as;
extern bool s_show_welcome;

/* Toggles and one-shot requests the menu bar raises and the layout consumes. */
extern bool s_demo_lod_enabled;
extern int  s_layout_preset_pending;
extern bool s_reset_layout_requested;

/* Helpers defined in jce_editor_layout.cpp and called from the menu bar. */
void            cmd_pack_current_scene_(void);
void            cmd_toggle_demo_lod_(void);
void            pump_pending_panel_focus(void);
void            request_gated_action(PendingGatedAction act,
                                     const char *path = NULL);
SaveSceneResult save_scene_or_open_save_as(void);
bool            should_block_editor_interaction(void);

/* Defined in jce_editor_menu_bar.cpp, called from the layout's frame. */
void draw_menu_bar(void);

#endif /* JCE_EDITOR_LAYOUT_INTERNAL_H */
