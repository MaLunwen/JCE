/*
 * jce_hotkeys.h  Central editor hotkey registry.
 *
 * Replaces hardcoded ImGui::IsKeyPressed checks scattered across panels
 * with a single id-based lookup so:
 *   - Settings dialog can list every keybinding.
 *   - Users can rebind chords (persists to hotkeys.json).
 *   - Help/tooltips can render the current chord text.
 *
 * Migration is incremental — call sites switch to jce_hotkey_pressed()
 * one at a time; legacy IsKeyPressed code keeps working.
 */
#ifndef JCE_HOTKEYS_H
#define JCE_HOTKEYS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_HK_FILE_SAVE = 0,
    JCE_HK_FILE_SAVE_AS,
    JCE_HK_FILE_OPEN,
    JCE_HK_FILE_NEW,

    JCE_HK_EDIT_UNDO,
    JCE_HK_EDIT_REDO,
    JCE_HK_EDIT_CUT,
    JCE_HK_EDIT_COPY,
    JCE_HK_EDIT_PASTE,
    JCE_HK_EDIT_DUPLICATE,
    JCE_HK_EDIT_DELETE,
    JCE_HK_EDIT_RENAME,
    JCE_HK_EDIT_SELECT_ALL,
    JCE_HK_EDIT_FIND,

    JCE_HK_GIZMO_TRANSLATE,
    JCE_HK_GIZMO_ROTATE,
    JCE_HK_GIZMO_SCALE,
    JCE_HK_GIZMO_NONE,
    JCE_HK_GIZMO_TOGGLE_SPACE,
    JCE_HK_GIZMO_TOGGLE_SNAP,

    JCE_HK_VIEW_FRAME_SELECTED,
    JCE_HK_VIEW_FRAME_ALL,
    JCE_HK_VIEW_FOCUS_PERSP,
    JCE_HK_VIEW_FOCUS_TOP,
    JCE_HK_VIEW_FOCUS_FRONT,
    JCE_HK_VIEW_FOCUS_RIGHT,
    JCE_HK_VIEW_TOGGLE_GRID,
    JCE_HK_VIEW_TOGGLE_GIZMOS,

    JCE_HK_PLAY_TOGGLE,
    JCE_HK_PLAY_STEP,
    JCE_HK_PLAY_PAUSE,

    JCE_HK_UI_COMMAND_PALETTE,
    JCE_HK_UI_FIND_IN_HIERARCHY,
    JCE_HK_UI_FIND_IN_ASSETS,
    JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW,

    JCE_HK_COUNT
} JceHotkeyId;

/* ImGui modifier bitfield (matches ImGuiMod_*). */
enum JceHotkeyMod {
    JCE_HKM_NONE  = 0,
    JCE_HKM_CTRL  = 1 << 0,
    JCE_HKM_SHIFT = 1 << 1,
    JCE_HKM_ALT   = 1 << 2,
    JCE_HKM_SUPER = 1 << 3
};

typedef struct {
    int      key;       /* ImGuiKey_* */
    uint8_t  mods;      /* JceHotkeyMod bitfield */
} JceHotkeyChord;

/* Lifecycle. Call once at editor start; load attempts to read hotkeys.json. */
void                jce_hotkeys_init(void);
void                jce_hotkeys_shutdown(void);
bool                jce_hotkeys_load(void);
bool                jce_hotkeys_save(void);
void                jce_hotkeys_reset_all(void);

/* Query / mutation. */
const char         *jce_hotkey_name(JceHotkeyId id);          /* "File / Save" */
const char         *jce_hotkey_id_string(JceHotkeyId id);     /* "file.save"   */
JceHotkeyChord      jce_hotkey_get(JceHotkeyId id);
JceHotkeyChord      jce_hotkey_get_default(JceHotkeyId id);
void                jce_hotkey_set(JceHotkeyId id, JceHotkeyChord chord);

/* Per-frame query (ImGui-context required). True only on the first frame
 * the chord is satisfied (rising edge), regardless of whether ImGui has
 * keyboard focus on a text input — call sites can opt-in/opt-out. */
bool                jce_hotkey_pressed(JceHotkeyId id);

/* Renders a human-readable chord, e.g. "Ctrl+Shift+Z". `out` must be
 * at least 64 bytes. Returns out for convenience. */
char               *jce_hotkey_chord_label(JceHotkeyChord chord,
                                            char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_HOTKEYS_H */
