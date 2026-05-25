/*
 * jce_panel_toolbar.cpp  Top-level editor Toolbar (Unity-style).
 *
 * Hosts:
 *   - Transform tools (Q/W/E/R)              -> jce_state_set_gizmo_mode
 *   - Pivot / Center toggle (Z)              -> jce_state_set_gizmo_pivot
 *   - Local / Global toggle (X)              -> jce_state_set_gizmo_space
 *   - Play / Pause / Step                    -> jce_state_play/pause/stop/step
 *
 * Hotkeys are processed globally here (no focus requirement) so they work
 * from any panel.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_hotkeys.h"

#include <jce/tools/jce_imgui.hpp>

/* ── Hotkey processing (global, focus-agnostic) ────────────────────── */

static void process_toolbar_hotkeys(void)
{
    /* Skip when an ImGui text widget owns the keyboard, otherwise typing
     * "w" into a text field would switch the gizmo. */
    if (ImGui::GetIO().WantTextInput) return;

    if (jce_hotkey_pressed(JCE_HK_GIZMO_NONE))      jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_TRANSLATE)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_ROTATE))    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    if (jce_hotkey_pressed(JCE_HK_GIZMO_SCALE))     jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

    if (jce_hotkey_pressed(JCE_HK_GIZMO_TOGGLE_SPACE)) {
        JceGizmoSpace gs = jce_state_get_gizmo_space();
        jce_state_set_gizmo_space(gs == JCE_GIZMO_LOCAL ? JCE_GIZMO_WORLD : JCE_GIZMO_LOCAL);
    }
    if (jce_hotkey_pressed(JCE_HK_GIZMO_TOGGLE_PIVOT)) {
        JceGizmoPivot gp = jce_state_get_gizmo_pivot();
        jce_state_set_gizmo_pivot(gp == JCE_GIZMO_PIVOT ? JCE_GIZMO_CENTER : JCE_GIZMO_PIVOT);
    }

    if (jce_hotkey_pressed(JCE_HK_PLAY_TOGGLE)) {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_STOPPED) jce_state_play();
        else                        jce_state_stop();
    }
    if (jce_hotkey_pressed(JCE_HK_PLAY_PAUSE)) {
        JcePlayState ps = jce_state_get_play_state();
        if (ps == JCE_PLAY_PLAYING) jce_state_pause();
        else if (ps == JCE_PLAY_PAUSED) jce_state_play();
    }
    if (jce_hotkey_pressed(JCE_HK_PLAY_STEP)) {
        jce_state_step(1.0f / 60.0f);
    }
}

/* ── Content drawing helpers (REMOVED) ────────────────────────────────
   draw_transform_tools / draw_pivot_space / draw_play_controls used to
   render the top toolbar's widgets. The bar was removed (duplicated
   Scene-viewport gizmo controls and menu-bar Play/Stop), so these
   helpers are gone too. Hotkeys still work — see process_toolbar_hotkeys
   above and jce_editor_panel_toolbar_inline() below. */

/* draw_toolbar_content() removed — see jce_editor_panel_toolbar_inline()
   below. The top toolbar's visible widgets (Transform/Pivot/Space and
   Play/Pause/Step) duplicated Scene-viewport's inline toolbar and the
   menu-bar's right-aligned Play/Stop, so the bar itself is gone. Only
   the global Q/W/E/R/X/Z hotkeys survive (processed every frame). */

/* ── Inline hotkey pump ───────────────────────────────────────────── */

/* Drains the global gizmo hotkeys every frame. Drawn nothing — exists
   so Q/W/E/R/X/Z keep working from any panel even though the bar UI
   was removed (and the Window > Toolbar menu item with it). */
void jce_editor_panel_toolbar_inline(void)
{
    process_toolbar_hotkeys();
}

/* Legacy standalone wrapper — no longer called from layout, kept as a
   no-op so any out-of-tree caller stays linkable. */
void jce_editor_panel_toolbar(void)
{
    /* intentionally empty */
}
