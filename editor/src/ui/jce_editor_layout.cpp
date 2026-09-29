/*
 * jce_editor_layout.cpp  DockSpace layout with menu bar.
 *
 * Uses ImGui DockSpace and DockBuilder for a Unity-like editor layout:
 *   Left   (15%) — Hierarchy
 *   Center (60%) — Scene View / Game View (tabbed)
 *   Right  (25%) — Inspector / File Viewer (tabbed)
 *   Bottom (25%) — Console / Timeline / Asset Browser (tabbed)
 *
 * All panels are visible by default. Reset Default Layout re-enables
 * all panels and restores the dock arrangement.
 *
 * First-time panel pose: jce_editor_panel_default_pose() is invoked from
 * the central draw_panel_windows() dispatcher before each panel's Begin.
 * Using ImGuiCond_FirstUseEver, this gives never-before-seen windows a
 * centered ~60%-of-viewport pose instead of ImGui's default (60,60)/(32,32)
 * blob, while leaving docked panels and any persisted imgui.ini entry
 * fully untouched.
 *
 * Menu bar matches reference EditorUI.java:
 *   File | Edit | GameObject | Window | Help | [centered Play controls]
 */

#include "jce_editor_layout.h"
#include "jce_editor_layout_internal.h"

#include "dialogs/jce_editor_dialogs.h"
#include "dialogs/jce_dialog_asset_picker.h"
#include "core/jce_editor.h"
#include "core/jce_editor_scene_file_watch.h"
#include "jce_editor_colors.h"
#include "jce_editor_layout_scene_commands.h"
#include "jce_editor_welcome_policy.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_project.h"
#include "jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_game_input_bridge.h"
#include "core/jce_editor_config.h"
extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_timer.h>
#include <jce/application/jce_screenshot.h>
}
#include "scene/jce_editor_scene_render.h"   /* jce_editor_get_renderer */
#include "core/jce_editor_recorder.h"        /* F9 VP9/WebM recorder */
#include "core/jce_editor_toast.h"
#include "core/jce_editor_assert_bridge.h"
#include "core/jce_hotkeys.h"
#include "scene/jce_editor_game_render.h"   /* jce_editor_game_render_is_mouse_captured */
#include "core/jce_workspace.h"
#include "panels/jce_panel_common.h"         /* multi-select duplicate / delete */
#include "panels/jce_panel_preferences.h"
#include "viewers/jce_file_viewer.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include "scene/jce_editor_scene_render.h"

#include <jce/middleware/scene/jce_lod.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <cstdlib>   /* getenv (JCE_DBG_FOCUS_SCENE QA hook) */
#include <jce/tools/jce_imgui.hpp>
#include <jce/tools/jce_imgui_internal.h>
#include <stdio.h>
#include <string.h>

/* ── Dialog state ─────────────────────────────────────────────────── */

bool s_show_about       = false;
bool s_demo_lod_enabled = false;
static JceLodGroup s_demo_lod_group = {};
bool s_show_new_project = false;
static bool s_show_open_project = false;
static bool s_show_open_scene  = false;
bool s_show_open_bundle = false;
bool s_show_save_as     = false;
static bool s_show_unsaved     = false;
bool s_show_build       = false;
bool s_show_bundles     = false;
static bool s_show_proj_settings = false;
static bool s_show_preferences = false;
bool s_show_welcome     = false;
static bool s_welcome_startup_resolved = false;
static int  s_unsaved_result   = 0;
static bool s_quit_after_save_as = false;
static bool s_quit_confirmed = false;

/* Deferred dock-tab focus shared by menu commands and dialog shims.
 * We select the dock tab directly instead of calling SetWindowFocus()
 * from inside a menu popup, which would close the Window menu before the
 * user can toggle/focus more panels. */
static char s_pending_dock_tab[128] = {0};
static int  s_pending_dock_tab_ttl = 0;

extern "C" void jce_editor_panel_request_focus(const char *stable_window_id)
{
    if (!stable_window_id || !stable_window_id[0])
        return;
    snprintf(s_pending_dock_tab, sizeof(s_pending_dock_tab), "%s",
             stable_window_id);
    s_pending_dock_tab_ttl = 30;
}

void pump_pending_panel_focus(void)
{
    if (!s_pending_dock_tab[0])
        return;

    ImGuiWindow *w = ImGui::FindWindowByName(s_pending_dock_tab);
    bool done = false;
    if (w && w->DockNode && w->DockNode->TabBar) {
        ImGuiID tab_id = w->TabId ? w->TabId : w->ID;
        w->DockNode->TabBar->NextSelectedTabId = tab_id;
        done = true;
    }
    if (--s_pending_dock_tab_ttl <= 0 && !done) {
        /* Fallback for floating windows: traditional focus after the
         * menu interaction has had several frames to complete. */
        if (w)
            ImGui::SetWindowFocus(s_pending_dock_tab);
        done = true;
    }
    if (done) {
        s_pending_dock_tab[0] = '\0';
        s_pending_dock_tab_ttl = 0;
    }
}

/* ── Unsaved-changes gate ─────────────────────────────────────────────
 * New Scene / Open Scene / Open Project / Open Recent all discard the
 * current in-memory scene.  Each routes through request_gated_action(),
 * which (when the scene is dirty) defers the action behind the SAME
 * Save / Don't-Save / Cancel modal that quit already uses, and only
 * runs the action once the user confirms (Save succeeded, or Don't Save).
 *
 * The dialog drives the shared s_show_unsaved / s_unsaved_result state
 * machine; s_pending_action tells the post-dialog handler whether this
 * was a quit (PGA_QUIT, keeps the existing save→save-as→quit deferral)
 * or one of the scene-swap actions. */

static PendingGatedAction s_pending_action = PGA_NONE;
static char s_pending_path[1024] = {0};       /* payload for OPEN_RECENT_* */
static bool s_run_pending_after_save = false;  /* Save chosen → run once saved */

/* Perform the deferred scene-swap action.  Quit is handled separately by
 * the existing quit state machine and never reaches here. */
static void run_gated_action(PendingGatedAction act, const char *path)
{
    switch (act) {
    case PGA_NEW_SCENE:
        (void)jce_editor_layout_run_new_scene_command();
        break;
    case PGA_OPEN_SCENE:
        s_show_open_scene = true;
        break;
    case PGA_OPEN_PROJECT:
        s_show_open_project = true;
        break;
    case PGA_OPEN_RECENT_SCENE:
        if (path && path[0])
            (void)jce_state_load_scene_file(path);
        break;
    case PGA_OPEN_RECENT_PROJECT:
        if (path && path[0])
            jce_editor_dialog_open_project_set_path(path);
        s_show_open_project = true;
        break;
    default:
        break;
    }
}

/* Gate a scene-discarding action behind the unsaved-changes modal.
 * If the scene is clean (or a gate/modal is already in flight) the
 * action runs immediately; otherwise it is stashed and the modal opens. */
void request_gated_action(PendingGatedAction act, const char *path)
{
    if (act == PGA_NONE) return;

    /* A confirm flow (gate or quit) is already in progress — ignore the
     * new request rather than stacking modals. */
    if (s_show_unsaved || s_pending_action != PGA_NONE)
        return;

    if (!jce_state_is_scene_modified()) {
        run_gated_action(act, path);
        return;
    }

    s_pending_action = act;
    s_pending_path[0] = '\0';
    if (path && path[0])
        snprintf(s_pending_path, sizeof(s_pending_path), "%s", path);
    s_unsaved_result = 0;
    s_show_unsaved   = true;
}

/* ── Docking state ────────────────────────────────────────────────── */

static bool s_layout_initialized = false;
static int  s_deferred_focus_frames = 0;
bool s_reset_layout_requested = false;
/* Layout preset to apply on next reset. 0=Default, 1=Wide, 2=Animation, 3=TwoByTwo. */
int  s_layout_preset_pending = 0;
static bool s_focus_scene_view = false;
static bool s_focus_game_view = false;
/* Game viewport maximised over the whole editor window.
 *
 * Deliberately NOT persisted: booting the editor into a state where one
 * panel covers everything, with no visible chrome explaining why, is a
 * worse first frame than re-pressing F10.
 *
 * The maximised view is a SEPARATE window id (###game_view_maximized) that
 * hosts the same content, and the docked ###game_view is skipped for that
 * frame. Re-posing the docked window itself would detach it from its dock
 * node and ImGui would write the detachment into imgui.ini -- the user's
 * layout would not survive the round trip. */
/* ONE slot, not one bool per viewport. Two independent flags would let both
 * viewports claim the work area in the same frame, and whichever drew second
 * would win silently -- and each content function drives its own render target,
 * so both would also render. A single slot makes the modes mutually exclusive
 * by construction rather than by a rule someone has to remember.
 *
 * JCE_PANEL_COUNT means "none maximised". */
static JceEditorPanel s_maximized_panel = JCE_PANEL_COUNT;

bool jce_editor_panel_is_maximized(JceEditorPanel kind)
{
    return s_maximized_panel == kind;
}

void jce_editor_panel_set_maximized(JceEditorPanel kind, bool on)
{
    if (on) {
        s_maximized_panel = kind;
        if (kind >= 0 && kind < JCE_PANEL_COUNT)
            *jce_editor_panel_visible_ptr(kind) = true;
    } else if (s_maximized_panel == kind) {
        s_maximized_panel = JCE_PANEL_COUNT;
    }
}

JceEditorPanel jce_editor_panel_maximized(void) { return s_maximized_panel; }

/* Kept so existing call sites and the Game View toolbar read the same way. */
bool jce_editor_game_view_maximized(void)
{
    return jce_editor_panel_is_maximized(JCE_PANEL_GAME_VIEW);
}

void jce_editor_game_view_set_maximized(bool on)
{
    jce_editor_panel_set_maximized(JCE_PANEL_GAME_VIEW, on);
}
static bool s_focus_inspector = false;
static bool s_focus_file_viewer = false;
void cmd_toggle_demo_lod_(void);  /* fwd-decl: defined further down */

static bool should_draw_dialog_dimmer(void)
{
    return s_show_new_project
        || s_show_open_project
        || s_show_open_scene
        || s_show_save_as
        || s_show_unsaved
    || jce_editor_assets_delete_dialog_open()
    || jce_editor_inspector_delete_dialog_open();
}

bool should_block_editor_interaction(void)
{
    return s_show_about || s_show_preferences || s_show_proj_settings
        || s_show_build || should_draw_dialog_dimmer();
}

static bool has_known_startup_project(void)
{
    JceEditorConfig cfg;
    if (!jce_editor_config_load(&cfg))
        return false;

    if (cfg.last_project[0] != '\0')
        return true;
    for (int i = 0; i < cfg.recent_count; i++) {
        if (cfg.recent_projects[i][0] != '\0')
            return true;
    }
    return false;
}

static void resolve_startup_welcome_once(void)
{
    if (s_welcome_startup_resolved)
        return;
    s_welcome_startup_resolved = true;

    const JceProject *project = jce_editor_project_get();
    bool has_project = project && project->project_root
                    && project->project_root[0] != '\0';
    s_show_welcome = jce_editor_welcome_should_open_on_startup(
        jce_editor_prefs_startup_behavior(),
        has_project,
        has_known_startup_project());
}

static void draw_dialog_dimmer(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImGui::Begin("##DialogDimmer", NULL, flags);
    /* Invisible button fills the dimmer to absorb all mouse clicks. */
    ImGui::InvisibleButton("##dimmer_block", vp->Size);
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

/* Full-viewport "Loading scene…" overlay shown while a frame-sliced scene
 * open is in flight.  It dims the editor and absorbs all input (an invisible
 * button covering the viewport) so the user cannot operate on a scene that is
 * still being populated, then draws a centered label + progress bar. */
static void draw_scene_loading_overlay(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImGui::Begin("##SceneLoadingOverlay", NULL, flags);
    /* Absorb all mouse input so panels behind cannot be clicked. */
    ImGui::InvisibleButton("##scene_loading_block", vp->Size);

    const float       progress = jce_state_scene_load_progress();
    const char       *label    = jce_editor_i18n("scene.loading");
    if (!label || !label[0]) label = "Loading scene\xE2\x80\xA6"; /* fallback */

    const float bar_w = ImClamp(vp->Size.x * 0.35f, 200.0f, 520.0f);
    char pct[16];
    snprintf(pct, sizeof(pct), "%d%%", (int)(progress * 100.0f + 0.5f));

    ImVec2 label_sz = ImGui::CalcTextSize(label);
    float  cx = vp->Pos.x + vp->Size.x * 0.5f;
    float  cy = vp->Pos.y + vp->Size.y * 0.5f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(ImVec2(cx - label_sz.x * 0.5f, cy - 36.0f), text_col, label);

    /* Manual progress bar (draw-list based so it doesn't depend on cursor
     * layout inside a zero-padding window). */
    ImVec2 bar_a(cx - bar_w * 0.5f, cy - 4.0f);
    ImVec2 bar_b(cx + bar_w * 0.5f, cy + 16.0f);
    dl->AddRectFilled(bar_a, bar_b, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
    ImVec2 fill_b(bar_a.x + (bar_b.x - bar_a.x) * progress, bar_b.y);
    dl->AddRectFilled(bar_a, fill_b,
                      ImGui::GetColorU32(ImGuiCol_PlotHistogram), 4.0f);
    ImVec2 pct_sz = ImGui::CalcTextSize(pct);
    dl->AddText(ImVec2(cx - pct_sz.x * 0.5f, cy + 22.0f), text_col, pct);

    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}


SaveSceneResult save_scene_or_open_save_as(void)
{
    const char *scene_path = jce_state_get_current_scene_path();
    if (!scene_path || scene_path[0] == '\0') {
        s_show_save_as = true;
        jce_editor_console_log_level(JCE_CONSOLE_INFO,
            "No current scene file path. Use Save As to choose where to save.");
        return SAVE_SCENE_RESULT_NEEDS_PATH;
    }

    if (!jce_state_save_scene_file(scene_path)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Failed to save scene: %s", scene_path);
        return SAVE_SCENE_RESULT_FAILED;
    }

    jce_editor_console_log("Saved scene: %s", scene_path);
    jce_toast_success(jce_editor_i18n("toast.sceneSaved"), scene_path);
    return SAVE_SCENE_RESULT_OK;
}

static bool redo_hotkey_pressed(void)
{
    return jce_hotkey_pressed(JCE_HK_EDIT_REDO)
        || jce_hotkey_pressed(JCE_HK_EDIT_REDO_ALT);
}

static bool delete_hotkey_pressed(void)
{
    return jce_hotkey_pressed(JCE_HK_EDIT_DELETE)
        || jce_hotkey_pressed(JCE_HK_EDIT_DELETE_ALT);
}

static void select_all_scene_entities(void)
{
    int total = jce_state_get_entity_count();
    bool first = true;
    for (int i = 0; i < total; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id))
            continue;
        jce_state_select_entity(id, !first);
        first = false;
    }
    if (!first)
        jce_editor_inspector_request_sync();
}

static void copy_scene_selection(bool cut)
{
    int sel_count = 0;
    const uint32_t *sel = jce_state_get_selection(&sel_count);
    uint32_t focused = jce_state_get_focused();
    if (sel_count > 0) {
        jce_state_copy_entities(sel, sel_count, cut);
    } else if (focused != 0) {
        if (cut)
            jce_state_copy_entities(&focused, 1, true);
        else
            jce_state_copy_entity(focused);
    }
}

static void paste_scene_clipboard(void)
{
    uint32_t focused = jce_state_get_focused();
    uint32_t parent = jce_state_entity_exists(focused) ? focused : 0;
    uint32_t new_ids[JCE_MAX_SELECTED];
    int n = jce_state_paste_entities(parent, new_ids, JCE_MAX_SELECTED);
    if (n <= 0 && jce_state_has_copied()) {
        uint32_t nid = jce_state_paste_entity(parent);
        if (nid != 0) {
            new_ids[0] = nid;
            n = 1;
        }
    }
    if (n > 0) {
        jce_state_select_entity(new_ids[0], false);
        for (int i = 1; i < n; i++)
            jce_state_select_entity(new_ids[i], true);
        jce_editor_inspector_request_sync();
    }
}

/* Duplicate / delete of the whole selection live in jce_panel_common.h
 * (jce_panel_duplicate_selection / jce_panel_delete_selection) so the menu
 * bar, the Hierarchy and the Scene View all run the same orchestration. */

void cmd_toggle_demo_lod_(void);
void cmd_pack_current_scene_(void);
static void cmd_screenshot_(void);
static void cmd_record_toggle_(void);
static void cmd_copy_repro_(void);

static void handle_global_edit_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    /* Fullscreen via central registry (default F11). */
    if (jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW)) {
        jce_editor_toggle_fullscreen();
        return;
    }

    /* Screenshot (F12): capture the whole editor window to a PNG. */
    if (jce_hotkey_pressed(JCE_HK_UI_SCREENSHOT)) {
        cmd_screenshot_();
        return;
    }

    /* Record (F9): toggle continuous backbuffer capture (Phase 0: PNG frames). */
    if (jce_hotkey_pressed(JCE_HK_UI_RECORD)) {
        cmd_record_toggle_();
        return;
    }

    /* Copy repro command (Ctrl+Shift+C): put the exact envshot invocation for
     * WHAT IS ON SCREEN RIGHT NOW on the clipboard, and print it to the log.
     *
     * A screenshot shows a defect and hides the one thing needed to measure it.
     * Visual defects in this engine have been chased at cameras the reporter
     * never used -- one such hunt consumed a whole session and ended in "I
     * cannot reproduce it", which is a statement about the investigator's
     * camera and not about the bug. This turns "it looks wrong here" into a
     * command anyone can run.
     *
     * Both clipboard AND log: the clipboard is what a person wants, the log is
     * what survives the clipboard being overwritten by whatever they paste
     * next. */
    if (jce_hotkey_pressed(JCE_HK_UI_COPY_REPRO)) {
        cmd_copy_repro_();
        return;
    }

    /* Maximise the Game viewport (F10). Handled in this early block, before the
     * WantTextInput and game-input gates below, because the moment a user most
     * wants to un-maximise is while they are driving the game camera with the
     * mouse captured -- a handler placed after those gates would be dead
     * exactly then. */
    if (jce_hotkey_pressed(JCE_HK_UI_TOGGLE_GAME_MAXIMIZE)) {
        if (s_maximized_panel != JCE_PANEL_COUNT)
            s_maximized_panel = JCE_PANEL_COUNT;
        else
            jce_editor_panel_set_maximized(JCE_PANEL_GAME_VIEW, true);
        return;
    }

    /* Avoid stealing shortcuts while typing in a text field. */
    if (io.WantTextInput)
        return;

    /* Suppress editor shortcuts whenever the Game View is actively DRIVING input
       — i.e. the user clicked into it to control its free-fly / WASD free-cam (or
       the player / CharacterController), OR the OS mouse is captured. This holds
       REGARDLESS of Play state: the Game View free-cam can be driven in plain
       EDIT mode (play STOPPED) too, and there a WASD-driving user pressing
       Ctrl+S used to silently save / Ctrl+N reset the scene because the gate
       only fired during Play. jce_editor_game_view_is_input_active() is the
       persistent "user wants to drive the Game View" intent (set only on a
       click-into the hovered viewport, cleared on ESC / Stop / Alt), so it is
       true exactly when the user is driving the Game View — never on mere hover.
       In edit mode NOT driving the Game View, in the Scene View, and in every
       other panel (Hierarchy Ctrl+A / Ctrl+Z, …), shortcuts work normally. The
       UI keys above (F9/F11/F12) are handled earlier so they stay live. */
    if (jce_editor_game_view_is_input_active() ||
        jce_editor_game_render_is_mouse_captured())
        return;

    /* Modal dialogs must block background state changes. */
    if (should_block_editor_interaction())
        return;

    if (jce_hotkey_pressed(JCE_HK_FILE_SAVE_AS)) {
        s_show_save_as = true;
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_FILE_SAVE)) {
        save_scene_or_open_save_as();
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_FILE_OPEN)) {
        request_gated_action(PGA_OPEN_SCENE);
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_FILE_NEW)) {
        request_gated_action(PGA_NEW_SCENE);
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_SHADERS_RELOAD)) {
        jce_editor_reload_shaders();
        return;
    }

    if (jce_hotkey_pressed(JCE_HK_EDIT_UNDO)) {
        if (jce_state_can_undo())
            jce_state_undo();
        return;
    }
    if (redo_hotkey_pressed()) {
        if (jce_state_can_redo())
            jce_state_redo();
        return;
    }

    if (jce_hotkey_pressed(JCE_HK_EDIT_COPY)) {
        copy_scene_selection(false);
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_CUT)) {
        copy_scene_selection(true);
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_PASTE)) {
        paste_scene_clipboard();
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
        jce_panel_duplicate_selection();
        return;
    }
    if (delete_hotkey_pressed()) {
        jce_panel_delete_selection();
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_SELECT_ALL)) {
        select_all_scene_entities();
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_FIND)
        || jce_hotkey_pressed(JCE_HK_UI_FIND_IN_HIERARCHY)) {
        bool *v = jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH);
        if (v) *v = true;
        return;
    }

    /* Panel toggle hotkeys — flip Window-menu visibility flags. */
    struct PT { JceHotkeyId hk; JceEditorPanel panel; };
    static const PT toggles[] = {
        { JCE_HK_PANEL_CONSOLE,   JCE_PANEL_CONSOLE   },
        { JCE_HK_PANEL_PROFILER,  JCE_PANEL_PROFILER  },
        { JCE_HK_PANEL_HIERARCHY, JCE_PANEL_HIERARCHY },
        { JCE_HK_PANEL_INSPECTOR, JCE_PANEL_INSPECTOR },
        { JCE_HK_PANEL_ASSETS,    JCE_PANEL_ASSETS    },
        { JCE_HK_PANEL_SEARCH,    JCE_PANEL_SEARCH    },
        { JCE_HK_PANEL_PROJECT_SETTINGS, JCE_PANEL_PROJECT_SETTINGS },
        { JCE_HK_EDIT_PREFERENCES,       JCE_PANEL_USER_PREFERENCES },
    };
    for (const auto &t : toggles) {
        if (jce_hotkey_pressed(t.hk)) {
            bool *v = jce_editor_panel_visible_ptr(t.panel);
            if (v) *v = !*v;
        }
    }

    /* F12 = screenshot (handled above via JCE_HK_UI_SCREENSHOT).  F9 is left
       reserved for the upcoming screen-recording feature — do not bind editor
       actions to it.  (Demo-LOD toggle stays reachable from the Debug menu.) */

    /* Workspace switching (Ctrl+F1..F7). Mapped 1:1 to the first seven
       workspaces (Default, Modeling, Rigging, Animation, FX, Rendering,
       UV Editing). Sculpting has no default chord but is reachable from
       the menu-bar dropdown. */
    {
        struct WSKey { JceHotkeyId hk; JceWorkspaceId ws; };
        static const WSKey ws_keys[] = {
            { JCE_HK_WORKSPACE_1, JCE_WORKSPACE_DEFAULT    },
            { JCE_HK_WORKSPACE_2, JCE_WORKSPACE_MODELING   },
            { JCE_HK_WORKSPACE_3, JCE_WORKSPACE_RIGGING    },
            { JCE_HK_WORKSPACE_4, JCE_WORKSPACE_ANIMATION  },
            { JCE_HK_WORKSPACE_5, JCE_WORKSPACE_FX         },
            { JCE_HK_WORKSPACE_6, JCE_WORKSPACE_RENDERING  },
            { JCE_HK_WORKSPACE_7, JCE_WORKSPACE_UV_EDITING },
        };
        for (const auto &k : ws_keys) {
            if (jce_hotkey_pressed(k.hk)) {
                jce_workspace_set_active(k.ws);
                break;
            }
        }
    }

    if (jce_hotkey_pressed(JCE_HK_FILE_BUILD_SETTINGS)) {
        s_show_build = true;
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_FILE_PACK_CURRENT_SCENE)) {
        cmd_pack_current_scene_();
        return;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMMAND PALETTE (Ctrl+P)
 *  Fuzzy-search list of editor actions. Open with Ctrl+P, type to filter,
 *  Up/Down to move, Enter to run, Esc to close.
 * ══════════════════════════════════════════════════════════════════════ */

struct PaletteCmd {
    const char *id;            /* stable lower-case id, used for fuzzy match */
    const char *label;         /* shown text */
    const char *category;      /* "File" / "Edit" / "Window" / ... */
    void (*run)(void);
};

static bool  s_palette_open = false;
static char  s_palette_query[128] = {0};
static int   s_palette_sel = 0;
static bool  s_palette_focus_query = false;

/* Action helpers -- thin wrappers around existing code paths. */
static void cmd_undo_(void)              { if (jce_state_can_undo()) jce_state_undo(); }
static void cmd_redo_(void)              { if (jce_state_can_redo()) jce_state_redo(); }
static void cmd_new_scene_(void)         { request_gated_action(PGA_NEW_SCENE); }
static void cmd_open_scene_(void)        { request_gated_action(PGA_OPEN_SCENE); }
static void cmd_save_scene_(void)        { save_scene_or_open_save_as(); }
static void cmd_save_scene_as_(void)     { s_show_save_as = true; }
static void cmd_new_project_(void)       { s_show_new_project = true; }
static void cmd_open_project_(void)      { request_gated_action(PGA_OPEN_PROJECT); }

/* Public entry points for the Welcome screen. */
extern "C" void jce_editor_layout_request_new_project(void)
    { s_show_new_project = true; }
extern "C" void jce_editor_layout_request_open_project(void)
    { request_gated_action(PGA_OPEN_PROJECT); }
static void cmd_build_settings_(void)    { s_show_build = true; }
static void cmd_build_bundles_(void)     { s_show_bundles = true; }
void cmd_pack_current_scene_(void) {
    if (jce_editor_dialog_bundles_open_for_current_scene())
        s_show_bundles = true;
}
static void cmd_open_bundle_(void)       { s_show_open_bundle = true; }
static void cmd_settings_(void)          { s_show_preferences = true; }
static void cmd_proj_settings_(void)     { s_show_proj_settings = true; }
static void cmd_about_(void)             { s_show_about = true; }
static void cmd_quit_(void)              { jce_editor_layout_request_quit(); }
static void cmd_toggle_fullscreen_(void) { jce_editor_toggle_fullscreen(); }

/* F12 — capture the editor window to .jce/screenshots/jce_screenshot_<ts>.png.
 * The capture is deferred one frame and written by the renderer's bgfx
 * screen_shot callback; the toast confirms the request + destination.
 * Uses engine wrappers only (no platform-specific calls): the host-FS
 * directory helper (mkdir -p) and the engine's local-time formatter, which
 * hide the localtime_r/localtime_s fork. */
static void cmd_screenshot_(void)
{
    char dir[1024];
    jce_editor_dotjce_path("screenshots", dir, sizeof(dir));
    jce_fs_host_create_directory(dir);

    char stamp[32];
    jce_time_format_local(jce_time_now_epoch_seconds(),
                          "%Y-%m-%d_%H-%M-%S", stamp, sizeof(stamp));

    char path[1100];
    snprintf(path, sizeof(path),
             "%s/jce_screenshot_%s.png", dir, stamp);

    /* WHOLE editor window (incl. UI) via the backbuffer — the v-0.9.x behaviour
     * the user confirmed correct.  Interactive F12 presents in the foreground, so
     * bgfx_request_screen_shot completes and captures the FULL window, not just
     * the scene viewport.  (The scene-FBO variant that replaced this was meant for
     * headless capture but regressed the user-facing F12 into a viewport-only shot.) */
    bool ok = jce_screenshot_save(path, JCE_SCREENSHOT_PNG);
    if (ok)
        jce_toast_info(jce_editor_i18n("toast.screenshot"), path);
    else
        jce_toast_error("%s", jce_editor_i18n("toast.screenshotFailed"));
}

/* F9 — toggle screen recording to a VP9 .webm (backbuffer -> VP9 -> WebM on a
 * worker thread). Audio (Opus) is a planned follow-up. */
/* Build the envshot line that reproduces the current scene view. */
static void cmd_copy_repro_(void)
{
    float tgt[3] = { 0, 0, 0 }, dist = 0.0f, pitch = 0.0f, yaw = 0.0f;
    jce_editor_scene_get_orbit(tgt, &dist, &pitch, &yaw);

    const char *scene = jce_state_get_current_scene_path();
    char line[1024];
    /* `--view bay` is a placeholder every field of which the overrides below
     * replace; the tool needs a named view to start from. Naming that here is
     * cheaper than a reader discovering it by getting a different picture.
     *
     * The target's Y and the distance come out as a trailing COMMENT rather
     * than as flags, because envshot's named views own those two and there is
     * no override for them. Printing them anyway lets the reader see whether
     * the placeholder's y/dist are close enough, instead of silently getting a
     * different framing. */
    snprintf(line, sizeof line,
             "python tools/envshot.py capture --name repro "
             "--scene \"%s\" --view bay --dx %.3f --dz %.3f "
             "--pitch %.2f --yaw %.2f   # target y=%.3f dist=%.3f",
             (scene && scene[0]) ? scene : "<unsaved scene>",
             (double)tgt[0], (double)tgt[2], (double)pitch, (double)yaw,
             (double)tgt[1], (double)dist);

    ImGui::SetClipboardText(line);
    /* A toast, not a log line: this is pressed while looking at a defect, and
     * the person needs to know it worked without leaving the view they are
     * reporting. The command itself is on the clipboard; the toast only has to
     * confirm that. */
    jce_toast_info(jce_editor_i18n("toast.copyRepro"), line);
}

static void cmd_record_toggle_(void)
{
    if (jce_editor_recorder_is_active()) {
        uint32_t frames  = jce_editor_recorder_frame_count();
        uint32_t dropped = jce_editor_recorder_dropped();
        jce_editor_recorder_stop();
        jce_toast_info(jce_editor_i18n("toast.recordingStopped"),
                       frames, dropped);
        return;
    }

    JceRenderer *r = jce_editor_get_renderer();
    if (!r) { jce_toast_error("%s", jce_editor_i18n("toast.recordNoRenderer")); return; }

    char dir[1024];
    jce_editor_dotjce_path("recordings", dir, sizeof(dir));
    jce_fs_host_create_directory(dir);

    char stamp[32];
    jce_time_format_local(jce_time_now_epoch_seconds(),
                          "%Y-%m-%d_%H-%M-%S", stamp, sizeof(stamp));
    char path[1100];
    snprintf(path, sizeof(path), "%s/rec_%s.mkv", dir, stamp);

    if (jce_editor_recorder_start(r, path))
        jce_toast_info(jce_editor_i18n("toast.recordingStarted"), path);
    else
        jce_toast_error("%s", jce_editor_i18n("toast.recordStartFailed"));
}

static void cmd_create_(const char *name) {
    uint32_t id = jce_state_create_entity(name, 0);
    if (id) jce_state_select_entity(id, false);
}
static void cmd_create_empty_(void) { cmd_create_("New Entity"); }
static void cmd_create_cube_(void)  { cmd_create_("Cube"); }
static void cmd_create_sphere_(void){ cmd_create_("Sphere"); }
static void cmd_create_plane_(void) { cmd_create_("Plane"); }
static void cmd_create_camera_(void){ cmd_create_("Camera"); }
static void cmd_create_dirlight_(void){ cmd_create_("Directional Light"); }
static void cmd_create_pointlight_(void){ cmd_create_("Point Light"); }

static void cmd_view_top_(void)   { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP); }
static void cmd_view_front_(void) { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT); }
static void cmd_view_right_(void) { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT); }
static void cmd_view_reset_(void) { jce_editor_scene_camera_reset(); }

static void cmd_layout_default_(void)  { s_layout_preset_pending = 0; s_reset_layout_requested = true; }
static void cmd_layout_wide_(void)     { s_layout_preset_pending = 1; s_reset_layout_requested = true; }
static void cmd_layout_anim_(void)     { s_layout_preset_pending = 2; s_reset_layout_requested = true; }
static void cmd_layout_2x2_(void)      { s_layout_preset_pending = 3; s_reset_layout_requested = true; }

/* Public shim used by jce_workspace_set_active(). Clamps to the valid
   preset range (0..9) and schedules the same deferred reset path the
   Window menu uses. */
extern "C" void jce_editor_layout_request_preset(int preset_idx)
{
    if (preset_idx < 0) preset_idx = 0;
    if (preset_idx > 9) preset_idx = 9;
    s_layout_preset_pending = preset_idx;
    s_reset_layout_requested = true;
}

static void cmd_toggle_panel_(JceEditorPanel p) {
    bool *v = jce_editor_panel_visible_ptr(p);
    if (v) *v = !*v;
}
static void cmd_show_hierarchy_(void)         { cmd_toggle_panel_(JCE_PANEL_HIERARCHY); }
static void cmd_show_inspector_(void)         { cmd_toggle_panel_(JCE_PANEL_INSPECTOR); }
static void cmd_show_console_(void)           { cmd_toggle_panel_(JCE_PANEL_CONSOLE); }
static void cmd_show_scene_(void)             { cmd_toggle_panel_(JCE_PANEL_SCENE_VIEW); }
static void cmd_show_game_(void)              { cmd_toggle_panel_(JCE_PANEL_GAME_VIEW); }
static void cmd_show_assets_(void)            { cmd_toggle_panel_(JCE_PANEL_ASSETS); }
static void cmd_show_profiler_(void)          { cmd_toggle_panel_(JCE_PANEL_PROFILER); }
static void cmd_show_postfx_(void)            { cmd_toggle_panel_(JCE_PANEL_POSTFX); }
static void cmd_show_render_pipeline_(void)   { cmd_toggle_panel_(JCE_PANEL_RENDER_PIPELINE); }
static void cmd_show_material_graph_(void)    { cmd_toggle_panel_(JCE_PANEL_MATERIAL_GRAPH); }
static void cmd_show_shader_inspector_(void)  { cmd_toggle_panel_(JCE_PANEL_SHADER_INSPECTOR); }
static void cmd_show_animation_editor_(void)  { cmd_toggle_panel_(JCE_PANEL_ANIMATION_EDITOR); }
static void cmd_show_animator_sm_(void)       { cmd_toggle_panel_(JCE_PANEL_ANIMATOR_SM); }
static void cmd_show_anim_rigging_(void)      { cmd_toggle_panel_(JCE_PANEL_ANIMATION_RIGGING); }
static void cmd_show_sequencer_(void)         { cmd_toggle_panel_(JCE_PANEL_SEQUENCER); }
static void cmd_show_navmesh_(void)           { cmd_toggle_panel_(JCE_PANEL_NAVMESH); }
static void cmd_show_bt_visualizer_(void)     { cmd_toggle_panel_(JCE_PANEL_BT_VISUALIZER); }
static void cmd_show_world_streaming_(void)   { cmd_toggle_panel_(JCE_PANEL_WORLD_STREAMING); }
static void cmd_show_ai_assistant_(void)      { cmd_toggle_panel_(JCE_PANEL_AI_ASSISTANT); }
static void cmd_show_terrain_(void)           { cmd_toggle_panel_(JCE_PANEL_TERRAIN); }
static void cmd_show_lightmap_(void)          { cmd_toggle_panel_(JCE_PANEL_LIGHTMAP_BAKE); }
static void cmd_show_curve_(void)             { cmd_toggle_panel_(JCE_PANEL_CURVE_EDITOR); }
static void cmd_show_particle_(void)          { cmd_toggle_panel_(JCE_PANEL_PARTICLE_EDITOR); }
static void cmd_show_import_presets_(void)    { cmd_toggle_panel_(JCE_PANEL_IMPORT_PRESETS); }

static const PaletteCmd s_palette_cmds[] = {
    /* File */
    { "file.new_scene",      "New Scene",                  "File",      cmd_new_scene_ },
    { "file.open_scene",     "Open Scene\u2026",          "File",      cmd_open_scene_ },
    { "file.save_scene",     "Save Scene",                 "File",      cmd_save_scene_ },
    { "file.save_scene_as",  "Save Scene As\u2026",       "File",      cmd_save_scene_as_ },
    { "file.new_project",    "New Project\u2026",          "File",      cmd_new_project_ },
    { "file.open_project",   "Open Project\u2026",         "File",      cmd_open_project_ },
    { "file.build_settings", "Build Settings\u2026",       "File",      cmd_build_settings_ },
    { "file.build_bundles",  "Build Bundles\u2026",        "File",      cmd_build_bundles_ },
    { "file.pack_current_scene", "Pack Current Scene as Bundle", "File", cmd_pack_current_scene_ },
    { "file.open_bundle",    "Open Bundle\u2026",            "File",      cmd_open_bundle_ },
    { "file.exit",           "Exit Editor",                "File",      cmd_quit_ },
    /* Edit */
    { "edit.undo",           "Undo",                        "Edit",      cmd_undo_ },
    { "edit.redo",           "Redo",                        "Edit",      cmd_redo_ },
    { "edit.settings",       "Editor Settings\u2026",       "Edit",      cmd_settings_ },
    { "edit.project_settings","Project Settings\u2026",     "File",      cmd_proj_settings_ },
    /* Create */
    { "create.empty",        "Create Empty",                "Create",    cmd_create_empty_ },
    { "create.cube",         "Create Cube",                 "Create",    cmd_create_cube_ },
    { "create.sphere",       "Create Sphere",               "Create",    cmd_create_sphere_ },
    { "create.plane",        "Create Plane",                "Create",    cmd_create_plane_ },
    { "create.camera",       "Create Camera",               "Create",    cmd_create_camera_ },
    { "create.dir_light",    "Create Directional Light",    "Create",   cmd_create_dirlight_ },
    { "create.point_light",  "Create Point Light",          "Create",    cmd_create_pointlight_ },
    /* View */
    { "view.top",            "View: Top",                   "View",      cmd_view_top_ },
    { "view.front",          "View: Front",                 "View",      cmd_view_front_ },
    { "view.right",          "View: Right",                 "View",      cmd_view_right_ },
    { "view.reset",          "Reset Camera",                "View",      cmd_view_reset_ },
    { "view.fullscreen",     "Toggle Fullscreen (F11)",     "View",      cmd_toggle_fullscreen_ },
    { "view.screenshot",     "Take Screenshot (F12)",       "View",      cmd_screenshot_ },
    /* Layout */
    { "layout.default",      "Layout: Default",             "Layout",    cmd_layout_default_ },
    { "layout.wide",         "Layout: Wide",                "Layout",    cmd_layout_wide_ },
    { "layout.animation",    "Layout: Animation",           "Layout",    cmd_layout_anim_ },
    { "layout.2x2",          "Layout: 2 by 2",              "Layout",    cmd_layout_2x2_ },
    /* Window toggles */
    { "window.hierarchy",    "Toggle Window: Hierarchy",    "Window",    cmd_show_hierarchy_ },
    { "window.inspector",    "Toggle Window: Inspector",    "Window",    cmd_show_inspector_ },
    { "window.console",      "Toggle Window: Console",      "Window",    cmd_show_console_ },
    { "window.scene",        "Toggle Window: Scene",        "Window",    cmd_show_scene_ },
    { "window.game",         "Toggle Window: Game",         "Window",    cmd_show_game_ },
    { "window.assets",       "Toggle Window: Asset Browser","Window",    cmd_show_assets_ },
    { "window.profiler",     "Toggle Window: Profiler",     "Window",    cmd_show_profiler_ },
    { "window.postfx",       "Toggle Window: Post FX",      "Window",    cmd_show_postfx_ },
    { "window.renderPipeline","Toggle Window: Render Pipeline","Window",  cmd_show_render_pipeline_ },
    { "window.material_graph","Toggle Window: Material Graph","Window",  cmd_show_material_graph_ },
    { "window.animation",    "Toggle Window: Animation Editor","Window", cmd_show_animation_editor_ },
    { "window.animator_sm",  "Toggle Window: Animator SM",  "Window",    cmd_show_animator_sm_ },
    { "window.animationRigging", "Toggle Window: Animation Rigging", "Window", cmd_show_anim_rigging_ },
    { "window.sequencer",    "Toggle Window: Sequencer",    "Window",    cmd_show_sequencer_ },
    { "window.navmesh",      "Toggle Window: NavMesh",      "Window",    cmd_show_navmesh_ },
    { "window.btVisualizer", "Toggle Window: BT Visualizer","Window",    cmd_show_bt_visualizer_ },
    { "window.worldStreaming","Toggle Window: World Streaming","Window", cmd_show_world_streaming_ },
    { "window.shaderInspector","Toggle Window: Shader Inspector","Window", cmd_show_shader_inspector_ },
    { "window.aiAssistant",  "Toggle Window: AI Assistant", "Window",    cmd_show_ai_assistant_ },
    { "window.terrain",      "Toggle Window: Terrain",      "Window",    cmd_show_terrain_ },
    { "window.lightmap",     "Toggle Window: Lightmap Bake","Window",    cmd_show_lightmap_ },
    { "window.curve",        "Toggle Window: Curve Editor", "Window",    cmd_show_curve_ },
    { "window.particle",     "Toggle Window: Particle Editor","Window",  cmd_show_particle_ },
    { "window.import_presets","Toggle Window: Import Presets","Window",  cmd_show_import_presets_ },
    /* Help */
    { "help.about",          "About JCE Editor",            "Help",      cmd_about_ },
};
enum { PALETTE_CMD_COUNT = (int)(sizeof s_palette_cmds / sizeof s_palette_cmds[0]) };

/* Translated label / category for a palette command.  Keys derive from
 * the stable command id ("palette.cmd.<id>") and category
 * ("palette.cat.<category>"); the English table literals stay as the
 * fallback so unknown ids degrade gracefully. */
static const char *palette_cmd_label_(const PaletteCmd &c)
{
    char key[96];
    snprintf(key, sizeof key, "palette.cmd.%s", c.id);
    return jce_editor_i18n_or(key, c.label);
}
static const char *palette_cmd_category_(const PaletteCmd &c)
{
    char key[64];
    snprintf(key, sizeof key, "palette.cat.%s", c.category);
    return jce_editor_i18n_or(key, c.category);
}

/* Lower-case substring match: returns true iff every char of `q` appears
 * in `hay` in order (subsequence match, like VS Code command palette). */
static bool palette_match(const char *hay, const char *q) {
    if (!q || !*q) return true;
    const char *p = q;
    for (; *hay && *p; ++hay) {
        char a = *hay, b = *p;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a == b) ++p;
    }
    return *p == 0;
}

static void draw_command_palette(void)
{
    /* Open the palette via the central registry (default Ctrl+P).
     * Suppressed while typing in fields and while modal dialogs are open. */
    {
        ImGuiIO &io = ImGui::GetIO();
        if (!io.WantTextInput
            && !s_palette_open
            && !should_block_editor_interaction()
            && jce_hotkey_pressed(JCE_HK_UI_COMMAND_PALETTE))
        {
            s_palette_open = true;
            s_palette_focus_query = true;
            s_palette_query[0] = 0;
            s_palette_sel = 0;
        }
    }

    if (!s_palette_open) return;

    /* Center popup near the top of the main viewport. */
    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImVec2 size(540.0f, 360.0f);
    ImVec2 pos(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
               vp->Pos.y + 80.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoMove   | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;

    if (!ImGui::Begin("##cmd_palette", &s_palette_open, flags)) {
        ImGui::End();
        return;
    }

    if (s_palette_focus_query) {
        ImGui::SetKeyboardFocusHere();
        s_palette_focus_query = false;
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##palette_query",
            jce_editor_i18n("commandPalette.hint"),
            s_palette_query, sizeof s_palette_query,
            ImGuiInputTextFlags_AutoSelectAll))
    {
        s_palette_sel = 0;
    }

    /* Build filtered list each frame (cheap, ~50 cmds). */
    int  filt_idx[PALETTE_CMD_COUNT];
    int  filt_n = 0;
    for (int i = 0; i < PALETTE_CMD_COUNT; i++) {
        const PaletteCmd &c = s_palette_cmds[i];
        if (palette_match(palette_cmd_label_(c), s_palette_query)
            || palette_match(c.id, s_palette_query)
            || palette_match(palette_cmd_category_(c), s_palette_query))
        {
            filt_idx[filt_n++] = i;
        }
    }
    if (filt_n == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("commandPalette.noMatch"));
    } else {
        if (s_palette_sel < 0)        s_palette_sel = 0;
        if (s_palette_sel >= filt_n)  s_palette_sel = filt_n - 1;

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  s_palette_sel = (s_palette_sel + 1) % filt_n;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true))  s_palette_sel = (s_palette_sel - 1 + filt_n) % filt_n;

        ImGui::Separator();
        ImGui::BeginChild("##palette_list", ImVec2(0, 0), false);
        for (int row = 0; row < filt_n; row++) {
            int ci = filt_idx[row];
            const PaletteCmd &c = s_palette_cmds[ci];
            char buf[256];
            snprintf(buf, sizeof buf, "%-32s  [%s]",
                     palette_cmd_label_(c), palette_cmd_category_(c));
            bool selected = (row == s_palette_sel);
            if (ImGui::Selectable(buf, selected)) {
                s_palette_sel = row;
                if (c.run) c.run();
                s_palette_open = false;
            }
            if (selected && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
                if (c.run) c.run();
                s_palette_open = false;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndChild();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        s_palette_open = false;

    ImGui::End();
}

/* ══════════════════════════════════════════════════════════════════════
 *  MENU BAR
 * ══════════════════════════════════════════════════════════════════════ */

void cmd_toggle_demo_lod_(void)
{
    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    if (!sr) return;
    if (s_demo_lod_enabled) {
        jce_scene_renderer_set_global_lod(sr, nullptr);
        s_demo_lod_enabled = false;
    } else {
        /* Build (high=cube, mid=sphere, low=plane) so the visual swap
         * is unmistakable when the camera moves. Distances tuned for
         * the JCE_STRESS_CUBES grid (~22 m wide). */
        JceMesh *cube   = jce_scene_renderer_get_builtin_mesh(sr, 0);
        JceMesh *sphere = jce_scene_renderer_get_builtin_mesh(sr, 1);
        JceMesh *plane  = jce_scene_renderer_get_builtin_mesh(sr, 2);
        jce_lod_setup(&s_demo_lod_group, cube, 8.0f, sphere, 18.0f, plane, 35.0f);
        jce_scene_renderer_set_global_lod(sr, &s_demo_lod_group);
        s_demo_lod_enabled = true;
    }
}

/* draw_menu_bar moved to jce_editor_menu_bar.cpp; the shared flags and
 * helpers it needs are declared in jce_editor_layout_internal.h. */

/* ══════════════════════════════════════════════════════════════════════
 *  DEFAULT DOCKING LAYOUT (first frame only)
 * ══════════════════════════════════════════════════════════════════════ */

/* Routes every Sprint-2 / Sprint-3 panel into a sensible dock node so it
 * never opens floating. The `###id` strings here MUST match the stable
 * IDs used by the panels' top-level ImGui::Begin calls. */
static void dock_extension_panels(ImGuiID left_id,
                                  ImGuiID center_id,
                                  ImGuiID right_id,
                                  ImGuiID bottom_id)
{
    /* Only dock window IDs that correspond to a live ImGui::Begin() call —
     * either via a dedicated panel TU or an inline Begin in draw_panel_windows()
     * below. Shim panels (postfx, vfx_graph, particle_editor, anim_sm,
     * curve_editor, sequencer, import_presets, reflection_probes,
     * memory_profiler, frame_debugger, package_manager, build_report,
     * shader_graph, lan_discovery, render_pipeline, lightmap_bake) have no
     * top-level window of their own — activating them redirects to a workbench
     * host's tab — so we deliberately omit them. We dock the 8 workbench hosts
     * and every remaining standalone panel instead. */

    /* ── Right column: scene-wide settings, lighting, terrain, navmesh ── */
    ImGui::DockBuilderDockWindow("###lighting_settings",  right_id); /* Rendering workbench */
    ImGui::DockBuilderDockWindow("###vcam_manager",       right_id);
    ImGui::DockBuilderDockWindow("###physics_debugger",   right_id);
    ImGui::DockBuilderDockWindow("###tags_layers",        right_id);
    ImGui::DockBuilderDockWindow("###physics_layers",     right_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",        right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",        right_id);
    ImGui::DockBuilderDockWindow("###bt_visualizer",      right_id);
    ImGui::DockBuilderDockWindow("###world_streaming",    right_id);
    ImGui::DockBuilderDockWindow("###ai_assistant",       right_id);

    /* ── Bottom strip: profilers, builders, testing, audio, IO, browsers ── */
    ImGui::DockBuilderDockWindow("###profiler",       bottom_id); /* Profiling workbench */
    ImGui::DockBuilderDockWindow("###build_profiles",     bottom_id); /* Build workbench    */
    ImGui::DockBuilderDockWindow("###test_runner",        bottom_id);
    ImGui::DockBuilderDockWindow("###audio_mixer",        bottom_id);
    ImGui::DockBuilderDockWindow("###input_manager",      bottom_id);
    ImGui::DockBuilderDockWindow("###save_browser",       bottom_id);
    ImGui::DockBuilderDockWindow("###systems",            bottom_id);
    ImGui::DockBuilderDockWindow("###version_control",    bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",    bottom_id); /* Animation workbench */
    ImGui::DockBuilderDockWindow("###search",             bottom_id);
    ImGui::DockBuilderDockWindow("###tile_palette",       bottom_id);

    /* ── Center: authoring canvases that share space with Scene/Game ── */
    ImGui::DockBuilderDockWindow("###jce_material_graph", center_id); /* Graphs workbench    */
    ImGui::DockBuilderDockWindow("###bundle_browser",     center_id); /* Asset Pipeline wb   */
    ImGui::DockBuilderDockWindow("###network_stats",      center_id); /* Network workbench   */
    ImGui::DockBuilderDockWindow("###sprite_editor",      center_id);
    ImGui::DockBuilderDockWindow("###project_settings",   center_id);
    ImGui::DockBuilderDockWindow("###user_guide",         center_id);

    (void)left_id;
}

static void setup_default_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    float vw = viewport->WorkSize.x;
    float vh = viewport->WorkSize.y;

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vw, vh));

    /* Split ratios (Unity-like proportions). */
    float bottom_ratio = JCE_LAYOUT_BOTTOM_RATIO;   /* 0.25 */
    float left_ratio   = JCE_LAYOUT_LEFT_RATIO;     /* 0.15 */
    float right_ratio  = JCE_LAYOUT_RIGHT_RATIO;    /* 0.25 */

    /* First split: top and bottom. */
    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down,
        bottom_ratio, &bottom_id, &top_id);

    /* Split top into left and center-right. */
    ImGuiID left_id = 0, center_right_id = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left,
        left_ratio, &left_id, &center_right_id);

    /* Split center-right into center and right. */
    ImGuiID center_id = 0, right_id = 0;
    ImGui::DockBuilderSplitNode(center_right_id, ImGuiDir_Right,
        right_ratio / (1.0f - left_ratio), &right_id, &center_id);

    /* Dock windows to their respective nodes.
       Dock order is reversed — first docked ends up as back tab, last
       docked is the front (active) tab. */
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);

    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);

    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);

    /* Bottom: Asset Browser is the most-used; put it as the front tab so
       new users see asset thumbnails immediately. Console second, then
       Timeline (least-used) hidden behind. */
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);

    /* Newer panels — dock to sensible default tabs (hidden by default,
       but if user enables them via Window menu they appear in the
       expected location). */
    ImGui::DockBuilderDockWindow("###profiler",            bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",         bottom_id);
    ImGui::DockBuilderDockWindow("###jce_material_graph",      center_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",             right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",             right_id);

    dock_extension_panels(left_id, center_id, right_id, bottom_id);

    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── Wide preset: no left panel, big inspector (~35%), small bottom (~18%) ── */
static void setup_wide_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.18f, &bottom_id, &top_id);

    ImGuiID right_id = 0, center_id = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Right, 0.35f, &right_id, &center_id);

    ImGuiID left_id = 0, center2 = 0;
    ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.16f, &left_id, &center2);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",     left_id);
    ImGui::DockBuilderDockWindow("Game###game_view",          center2);
    ImGui::DockBuilderDockWindow("Scene###scene_view",        center2);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer", right_id);
    ImGui::DockBuilderDockWindow("###profiler",           right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",     right_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",       bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",         bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",    bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",        bottom_id);
    /* Specialized panels: dock to sensible target so they appear in the
       expected zone the moment the user enables them via Window menu. */
    ImGui::DockBuilderDockWindow("###jce_material_graph",     center2);
    ImGui::DockBuilderDockWindow("###jce_navmesh",            right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",            right_id);
    dock_extension_panels(left_id, center2, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── Animation preset: timeline + animation editor occupy bottom 40% ── */
static void setup_animation_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.40f, &bottom_id, &top_id);

    ImGuiID left_id = 0, center_right_id = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.15f, &left_id, &center_right_id);

    ImGuiID center_id = 0, right_id = 0;
    ImGui::DockBuilderSplitNode(center_right_id, ImGuiDir_Right, 0.28f, &right_id, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   right_id);
    ImGui::DockBuilderDockWindow("###profiler",             right_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",          bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);
    /* Specialized panels go to the bottom by default in this preset. */
    ImGui::DockBuilderDockWindow("###jce_material_graph",       center_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",              right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",              right_id);
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── Two-by-Two preset: 4 viewports (Scene/Game/Assets/Inspector) ── */
static void setup_two_by_two_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    ImGuiID right_id = 0, left_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.5f, &right_id, &left_id);

    ImGuiID tl = 0, bl = 0, tr = 0, br = 0;
    ImGui::DockBuilderSplitNode(left_id,  ImGuiDir_Down, 0.5f, &bl, &tl);
    ImGui::DockBuilderSplitNode(right_id, ImGuiDir_Down, 0.5f, &br, &tr);

    ImGui::DockBuilderDockWindow("Scene###scene_view",        tl);
    ImGui::DockBuilderDockWindow("Game###game_view",          tr);
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",     bl);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer", bl);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",    bl);
    ImGui::DockBuilderDockWindow("Console###console",         br);
    ImGui::DockBuilderDockWindow("Inspector###inspector",     br);
    ImGui::DockBuilderDockWindow("Timeline###timeline",       br);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",        br);
    /* Specialized panels: route to right column / bottom-left when shown. */
    ImGui::DockBuilderDockWindow("###profiler",           br);
    ImGui::DockBuilderDockWindow("###jce_material_graph",     tl);
    ImGui::DockBuilderDockWindow("###jce_navmesh",            br);
    ImGui::DockBuilderDockWindow("###jce_terrain",            br);
    /* In 2x2 there's no distinct left/right column; reuse br for bottom-y
       routings and tl as the "center" canvas. */
    dock_extension_panels(bl, tl, br, br);
    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── Programmer preset: code/console-centric, no Game view dominance ── */
static void setup_programmer_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.45f, &bottom_id, &top_id);

    ImGuiID left_id = 0, right_block = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.18f, &left_id, &right_block);

    ImGuiID center_id = 0, right_id = 0;
    ImGui::DockBuilderSplitNode(right_block, ImGuiDir_Right, 0.30f, &right_id, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);
    ImGui::DockBuilderDockWindow("###profiler",             right_id);
    /* Bottom: console + asset browser + diagnostics primary. */
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── 2D preset: top-down workflow for sprites / tilemaps ── */
static void setup_two_d_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.30f, &bottom_id, &top_id);

    ImGuiID left_id = 0, right_block = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.16f, &left_id, &right_block);

    ImGuiID center_id = 0, right_id = 0;
    ImGui::DockBuilderSplitNode(right_block, ImGuiDir_Right, 0.28f, &right_id, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);
    /* Center: 2D-friendly canvases get priority over 3D Game view. */
    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   right_id);
    /* Bottom: console + asset browser. */
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);
    /* Pre-dock remaining new panels too. */
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

/* ── Mobile-Portrait preset: simulates a narrow 9:16 device viewport ── */
static void setup_mobile_portrait_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    /* Keep the central viewport narrow (~22% of width) to match a
       portrait phone aspect ratio. Side columns get the rest. */
    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.22f, &bottom_id, &top_id);

    ImGuiID left_id = 0, right_block = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.22f, &left_id, &right_block);

    ImGuiID right_id = 0, center_id = 0;
    /* Take a big right column so the remaining center is narrow and
       resembles a phone screen. */
    ImGui::DockBuilderSplitNode(right_block, ImGuiDir_Right, 0.55f, &right_id, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   left_id);
    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      right_id);
    ImGui::DockBuilderDockWindow("###profiler",             right_id);
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

static void setup_cinematic_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    /* Cinematic: scene view dominant; cinematics tools on the right;
       hierarchy on the left; timeline + curve editor on the bottom. */
    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.30f, &bottom_id, &top_id);

    ImGuiID left_id = 0, mid_block = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.16f, &left_id, &mid_block);

    ImGuiID right_id = 0, center_id = 0;
    ImGui::DockBuilderSplitNode(mid_block, ImGuiDir_Right, 0.28f, &right_id, &center_id);

    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",      left_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",  left_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",         center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",           center_id);
    ImGui::DockBuilderDockWindow("VCam Manager###vcam_manager",right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",      right_id);
    /* Bottom: timeline + curve editor + sequencer cluster for cinematic editing. */
    ImGui::DockBuilderDockWindow("Timeline###timeline",        bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",          bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",     bottom_id);
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

static void setup_profiling_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    /* Profiling: profilers fill the upper area; game view + console below. */
    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.40f, &bottom_id, &top_id);

    ImGuiID left_id = 0, right_id = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.55f, &left_id, &right_id);

    /* Cluster all profiling tools as tabs in the left node so users can
       cycle through profilers without rearranging the layout. */
    ImGui::DockBuilderDockWindow("###profiler",                  left_id);
    ImGui::DockBuilderDockWindow("###network_stats",                 left_id);
    ImGui::DockBuilderDockWindow("Physics Debugger###physics_debugger",left_id);
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",            left_id);
    ImGui::DockBuilderDockWindow("Game###game_view",                 right_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",        right_id);
    ImGui::DockBuilderDockWindow("Console###console",                bottom_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",               bottom_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",            bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",           bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",              bottom_id);
    dock_extension_panels(left_id, right_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

static void setup_lighting_docking_layout(ImGuiID dockspace_id)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_None);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(vp->WorkSize.x, vp->WorkSize.y));

    /* Lighting: scene view in the middle; lighting tools on the right;
       light explorer on the left; reflection probes + console below. */
    ImGuiID bottom_id = 0, top_id = 0;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.28f, &bottom_id, &top_id);

    ImGuiID left_id = 0, mid_block = 0;
    ImGui::DockBuilderSplitNode(top_id, ImGuiDir_Left, 0.20f, &left_id, &mid_block);

    ImGuiID right_id = 0, center_id = 0;
    ImGui::DockBuilderSplitNode(mid_block, ImGuiDir_Right, 0.30f, &right_id, &center_id);

    /* Light Explorer and Time of Day are tabs inside Lighting Settings
       (see P6-A.2/A.3) — do not dock them separately. */
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",                left_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",            left_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",                   center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",                     center_id);
    /* Cluster all lighting tools as tabs in the right node.
     * Reflection Probes / Light Explorer / Time of Day are tabs *inside* the
     * Lighting Settings workbench (see P6-A.2/A.3), so we don't dock them as
     * standalone windows — their ###id maps to a shim that has no Begin(). */
    ImGui::DockBuilderDockWindow("###lighting_settings",                 right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",                right_id);
    ImGui::DockBuilderDockWindow("Console###console",                    bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",               bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",                  bottom_id);
    dock_extension_panels(left_id, center_id, right_id, bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

static void apply_layout_preset(ImGuiID dockspace_id, int preset)
{
    switch (preset) {
        case 1: setup_wide_docking_layout(dockspace_id);            break;
        case 2: setup_animation_docking_layout(dockspace_id);       break;
        case 3: setup_two_by_two_docking_layout(dockspace_id);      break;
        case 4: setup_programmer_docking_layout(dockspace_id);      break;
        case 5: setup_two_d_docking_layout(dockspace_id);           break;
        case 6: setup_mobile_portrait_docking_layout(dockspace_id); break;
        case 7: setup_cinematic_docking_layout(dockspace_id);       break;
        case 8: setup_profiling_docking_layout(dockspace_id);       break;
        case 9: setup_lighting_docking_layout(dockspace_id);        break;
        default: setup_default_docking_layout(dockspace_id);        break;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  PANEL WINDOWS
 * ══════════════════════════════════════════════════════════════════════ */

/* Buddy table for the first-open dock fallback below: a panel with no
 * saved layout docks as a tab next to its most natural sibling instead
 * of opening as a floating centered window.  Entries are ###id suffixes;
 * each buddy list ends with a panel that is open in practically every
 * session (Inspector / Console) so the lookup almost never misses.
 * Panels NOT listed fall through to the generic Console→Inspector chain
 * — i.e. the default for any future panel is "docked", never "floating".
 * (Layout RESETS place everything explicitly via dock_extension_panels;
 * this fallback covers sessions whose imgui.ini predates a new panel.) */
typedef struct {
    const char *id;          /* "###window_id" */
    const char *buddies[3];  /* tried in order; NULL-terminated */
} JcePoseBuddy;

static const JcePoseBuddy k_pose_buddies[] = {
    { "###bt_visualizer",    { "###jce_navmesh", "###inspector",  NULL } },
    { "###world_streaming",  { "###jce_navmesh", "###inspector",  NULL } },
    { "###ai_assistant",     { "###inspector",   "###assets",     NULL } },
    { "###tile_palette",     { "###assets",      "###console",    NULL } },
    { "###physics_layers",   { "###tags_layers", "###inspector",  NULL } },
    { "###project_settings", { "###scene_view",  "###inspector",  NULL } },
    { "###user_guide",       { "###scene_view",  "###inspector",  NULL } },
};

/* Find the dock node of the first buddy window that exists and is
 * docked.  FindWindowByName only sees windows created this session, so
 * the chains end in always-open panels. */
static ImGuiID pose_buddy_dock_id(const char *imgui_window_name)
{
    const char *id = strstr(imgui_window_name, "###");
    if (!id) return 0;

    const char *generic[3] = { "###console", "###inspector", NULL };
    const char *const *buddies = generic;
    for (size_t i = 0; i < sizeof(k_pose_buddies) / sizeof(k_pose_buddies[0]); i++) {
        if (strcmp(k_pose_buddies[i].id, id) == 0) {
            buddies = k_pose_buddies[i].buddies;
            break;
        }
    }
    for (int b = 0; b < 3 && buddies[b]; b++) {
        ImGuiWindow *w = ImGui::FindWindowByName(buddies[b]);
        if (w && w->DockId != 0) return w->DockId;
    }
    return 0;
}

extern "C" void jce_editor_panel_default_pose(const char *imgui_window_name)
{
    if (!imgui_window_name) return;
    ImGuiViewport *vp = ImGui::GetMainViewport();
    if (!vp) return;

    /* First choice: dock beside a buddy (FirstUseEver — never disturbs a
     * window the user has already placed). */
    ImGuiID dock = pose_buddy_dock_id(imgui_window_name);
    if (dock != 0) {
        ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
        return;
    }

    /* Last resort (no buddy window alive yet): centered float. */
    ImVec2 size(vp->WorkSize.x * 0.6f, vp->WorkSize.y * 0.6f);
    ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                  vp->WorkPos.y + vp->WorkSize.y * 0.5f);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
}


/* Rolling per-panel phase sampler for draw_panel_windows: one phase per
 * section, charged from the previous sample point — locates which panel
 * owns the 150k "panels-other-than-hierarchy" 9.9ms/frame remainder. */
static inline void panel_phase(const char *name, uint64_t *t)
{
    uint64_t now = jce_time_perf_counter();
    jce_perf_phase_add(name, jce_time_perf_to_ms(*t, now));
    *t = now;
}

static void draw_panel_windows(void)
{
    uint64_t ed_pt = jce_time_perf_counter();
    char lbl[256];

    /* ── Hierarchy ────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY)) {
        /* ed_hier phase: prime suspect for the pure-UI app_update remainder
         * at large entity counts (per-frame get_roots scan + flatten). */
        uint64_t _t0_hier = jce_time_perf_counter();
        snprintf(lbl, sizeof(lbl), "%s###hierarchy", jce_editor_i18n("Hierarchy"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_hierarchy_content();
        }
        ImGui::End();
        jce_perf_phase_add("ed_hier", jce_time_perf_to_ms(_t0_hier,
                                                          jce_time_perf_counter()));
    }

    panel_phase("ed_p_hierarchy", &ed_pt);
    /* QA-only: JCE_DBG_FOCUS_SCENE keeps the Scene View tab foregrounded so
     * headless whole-window captures (JCE_WINCAP_*) see it instead of the
     * Game View that imgui.ini last selected. No effect unless the env is set. */
    static int s_dbg_focus_scene = -1;
    if (s_dbg_focus_scene < 0)
        s_dbg_focus_scene = getenv("JCE_DBG_FOCUS_SCENE") ? 1 : 0;
    if (s_dbg_focus_scene) s_focus_scene_view = true;

    /* QA-only: the mirror of the above. JCE_DBG_FOCUS_GAME foregrounds the
     * Game View and suppresses the Scene View, so a headless whole-window
     * capture sees the GAME camera.
     *
     * It exists because a defect was reported in Play mode and every
     * measurement available was of the scene view -- a different camera, a
     * different focus disc, and a different far plane (jce_editor_game_render
     * pins 1000). "I cannot reproduce it" from the wrong panel is a statement
     * about the harness. Suppressing the other panel rather than merely
     * raising this one matters: both are docked into one tab bar, and a
     * focused-but-tabbed window still renders behind its neighbour. */
    static int s_dbg_focus_game = -1;
    if (s_dbg_focus_game < 0)
        s_dbg_focus_game = getenv("JCE_DBG_FOCUS_GAME") ? 1 : 0;
    if (s_dbg_focus_game) s_focus_game_view = true;

    /* ── Scene View ───────────────────────────────────────────────── */
    /* No Scene View maximise is offered yet, so the slot can never name it and
     * this guard is always true today. It is kept because the guard, not the UI
     * that sets the slot, is what makes the mode correct -- adding the button
     * back must not also require remembering to add this. */
    if (!s_dbg_focus_game &&
        !jce_editor_panel_is_maximized(JCE_PANEL_SCENE_VIEW) &&
        *jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW)) {
        snprintf(lbl, sizeof(lbl), "%s###scene_view", jce_editor_i18n("Scene"));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (s_focus_scene_view) {
            ImGui::SetNextWindowFocus();
            s_focus_scene_view = false;
        }
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_scene_view_content();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    panel_phase("ed_p_scene_view", &ed_pt);
    /* ── Game View ────────────────────────────────────────────────── */
    if (!s_dbg_focus_scene &&
        (s_dbg_focus_game || *jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW))) {
        snprintf(lbl, sizeof(lbl), "%s###game_view", jce_editor_i18n("Game"));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (s_focus_game_view) {
            ImGui::SetNextWindowFocus();
            s_focus_game_view = false;
        }
        jce_editor_panel_default_pose(lbl);
        /* The docked window is submitted EVERY frame, maximised or not.
         *
         * Skipping it was wrong: an ImGui window that goes one frame without
         * being submitted is dropped from its dock node's tab bar, and on
         * return its tab is re-inserted at the end rather than in its original
         * position. Maximising and restoring would therefore quietly reorder
         * the user's tabs -- exactly the "must not affect the existing layout"
         * property this mode is supposed to have.
         *
         * Submitting it with no content keeps the dock membership, tab order,
         * split sizes and ini state bit-identical; the maximised overlay draws
         * on top of it, so nothing of the empty window is ever seen. The
         * content function still runs exactly once per frame -- there, not
         * here -- because it drives the game render target. */
        const bool maximized = jce_editor_panel_is_maximized(JCE_PANEL_GAME_VIEW);
        const bool docked_open = ImGui::Begin(
            lbl, jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW),
            ImGuiWindowFlags_NoFocusOnAppearing);
        if (docked_open && !maximized) {
            jce_editor_panel_game_view_content();
        } else if (!maximized) {
            jce_editor_game_input_bridge_publish(
                jce_editor_game_input_bridge_shared(), nullptr);
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    panel_phase("ed_p_game_view", &ed_pt);
    /* ── Inspector ────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR)) {
        snprintf(lbl, sizeof(lbl), "%s###inspector", jce_editor_i18n("Inspector"));
        if (s_focus_inspector) {
            ImGui::SetNextWindowFocus();
            s_focus_inspector = false;
        }
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_inspector_content();
        }
        ImGui::End();
    }

    panel_phase("ed_p_inspector", &ed_pt);
    /* ── File Viewer ──────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER)) {
        snprintf(lbl, sizeof(lbl), "%s###file_viewer", jce_editor_i18n("File Viewer"));
        if (s_focus_file_viewer) {
            ImGui::SetNextWindowFocus();
            s_focus_file_viewer = false;
        }
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_file_viewer_content();
        } else {
            jce_file_viewer_suspend_media();
        }
        ImGui::End();
    } else {
        jce_file_viewer_suspend_media();
    }

    panel_phase("ed_p_file_viewer", &ed_pt);
    /* ── Console ──────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE)) {
        snprintf(lbl, sizeof(lbl), "%s###console", jce_editor_i18n("Console"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_console_content();
        }
        ImGui::End();
    }

    /* ── Timeline (merged into Animation Editor workbench) ─────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE)) {
        /* Redirects to the Animation Editor workbench's Timeline tab. */
        jce_editor_panel_timeline();
    }

    panel_phase("ed_p_console", &ed_pt);
    /* ── Asset Browser ────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS)) {
        snprintf(lbl, sizeof(lbl), "%s###assets", jce_editor_i18n("Asset Browser"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_assets_content();
        }
        ImGui::End();
    }

    /* ── Post Processing (merged into Lighting Settings workbench) ──── */
    /* Tick first so open↔close transitions enable/disable the live
     * postfx pipeline regardless of whether the panel renders this
     * frame. */
    jce_editor_panel_postfx_tick();
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX)) {
        /* Redirects to the Lighting Settings workbench's Post-FX tab. */
        jce_editor_panel_postfx();
    }

    panel_phase("ed_p_asset_browser", &ed_pt);
    /* ── Lighting ─────────────────────────────────────────────────── */
    /* (Merged into JCE_PANEL_LIGHTING_SETTINGS — see below.) */

    /* ── Reverb Zones (merged into Audio Mixer) ───────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES)) {
        /* Route legacy visibility to Audio Mixer before the host draws,
         * so Window > Reverb Zones lands on the Reverb tab this frame
         * without flashing a dead "Reverb Zones" window. */
        jce_editor_panel_reverb_zones_content();
    }

    panel_phase("ed_p_lighting", &ed_pt);
    /* ── Audio Mixer ──────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER)) {
        snprintf(lbl, sizeof(lbl), "%s###audio_mixer",
                 jce_editor_i18n("audioMixer.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_audio_mixer_content();
        }
        ImGui::End();
    }

    panel_phase("ed_p_audio_mixer", &ed_pt);
    /* ── Input Manager ────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_INPUT_MANAGER)) {
        snprintf(lbl, sizeof(lbl), "%s###input_manager",
                 jce_editor_i18n("inputManager.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_INPUT_MANAGER), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_input_manager_content();
        }
        ImGui::End();
    }

    /* ── Package Manager (merged into Bundle Browser workbench) ───── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PACKAGE_MANAGER)) {
        /* Redirects to the Bundle Browser workbench's Packages tab. */
        jce_editor_panel_package_manager();
    }

    /* ── Frame Debugger (merged into Profiler workbench) ──────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER)) {
        /* Redirects to the Profiler workbench's Frame Debugger tab. */
        jce_editor_panel_frame_debugger();
    }

    /* ── Sprite Editor (Sprint 3 #12) ─────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR)) {
        snprintf(lbl, sizeof(lbl), "%s###sprite_editor",
                 jce_editor_i18n("spriteEditor.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_sprite_editor_content();
        }
        ImGui::End();
    }

    /* ── Tile Palette (Sprint 3 #12) ──────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE)) {
        snprintf(lbl, sizeof(lbl), "%s###tile_palette",
                 jce_editor_i18n("tilePalette.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_tile_palette_content();
        }
        ImGui::End();
    }

    /* ── VFX Graph (merged into Material Graph workbench) ─────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH)) {
        /* Redirects to the Material Graph workbench's VFX tab. */
        jce_editor_panel_vfx_graph();
    }

    /* ── Test Runner (Sprint 3 #14) ───────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER)) {
        snprintf(lbl, sizeof(lbl), "%s###test_runner",
                 jce_editor_i18n("testRunner.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_test_runner_content();
        }
        ImGui::End();
    }

    /* ── Build Profiles (Sprint 3 #14) ────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)) {
        snprintf(lbl, sizeof(lbl), "%s###build_profiles",
                 jce_editor_i18n("buildProfiles.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES), ImGuiWindowFlags_NoFocusOnAppearing)) {
            jce_editor_panel_build_profiles_content();
        }
        ImGui::End();
    }

    /* ── P3 panels (Memory Profiler / Physics Debugger / Light Explorer /
     * Reflection Probes / Shader Graph / Search / Version Control) ── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER)) {
        /* Redirects to the Profiler workbench's Memory tab. */
        jce_editor_panel_memory_profiler();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER)) {
        snprintf(lbl, sizeof(lbl), "%s###physics_debugger",
                 jce_editor_i18n("window.physicsDebugger"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_physics_debugger_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER)) {
        /* Legacy shim: route to the Lighting Settings workbench tab. */
        jce_editor_panel_light_explorer_content();
        jce_editor_panel_request_focus("###lighting_settings");
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_REFLECTION_PROBES)) {
        /* Redirects to the Lighting Settings workbench's Reflection Probes tab. */
        jce_editor_panel_reflection_probes();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_GRAPH)) {
        /* Redirects to the Material Graph workbench's Shader tab. */
        jce_editor_panel_shader_graph();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_INSPECTOR)) {
        /* Redirects to the Material Graph workbench's Inspector tab. */
        jce_editor_panel_shader_inspector();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH)) {
        snprintf(lbl, sizeof(lbl), "%s###search", jce_editor_i18n("window.search"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_search_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL)) {
        snprintf(lbl, sizeof(lbl), "%s###version_control",
                 jce_editor_i18n("window.versionControl"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_version_control_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY)) {
        /* Legacy shim: route to the Lighting Settings workbench tab. */
        jce_editor_panel_time_of_day_content();
        jce_editor_panel_request_focus("###lighting_settings");
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER)) {
        snprintf(lbl, sizeof(lbl), "%s###vcam_manager",
                 jce_editor_i18n("window.vcamManager"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_vcam_manager_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER)) {
        snprintf(lbl, sizeof(lbl), "%s###save_browser",
                 jce_editor_i18n("window.saveBrowser"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_save_browser_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_USER_GUIDE)) {
        snprintf(lbl, sizeof(lbl), "%s###user_guide",
                 jce_editor_i18n("window.userGuide"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_USER_GUIDE), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_user_guide_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER)) {
        snprintf(lbl, sizeof(lbl), "%s###bundle_browser",
                 jce_editor_i18n("panel.bundle_browser.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_bundle_browser_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_LAYERS)) {
        snprintf(lbl, sizeof(lbl), "%s###physics_layers",
                 jce_editor_i18n("panel.physics_layers.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_LAYERS), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_physics_layers_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TAGS_LAYERS)) {
        snprintf(lbl, sizeof(lbl), "%s###tags_layers",
                 jce_editor_i18n("panel.tags_layers.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TAGS_LAYERS), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_tags_layers_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS)) {
        snprintf(lbl, sizeof(lbl), "%s###lighting_settings",
                 jce_editor_i18n("panel.lighting.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_lighting_settings_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_REPORT)) {
        /* Redirects to the Build Profiles workbench's Report tab. */
        jce_editor_panel_build_report();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SYSTEMS)) {
        snprintf(lbl, sizeof(lbl), "%s###systems",
                 jce_editor_i18n("panel.systems.title"));
        jce_editor_panel_default_pose(lbl);
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SYSTEMS), ImGuiWindowFlags_NoFocusOnAppearing))
            jce_editor_panel_systems_content();
        ImGui::End();
    }
    /* Modal popups own their pose; no SetNextWindowPos needed. */
    jce_editor_panel_project_settings();
    /* LAN Discovery is a shim that redirects to the Network workbench;
     * no Begin → no pose needed. */
    jce_editor_panel_lan_discovery();
    jce_editor_panel_network_stats();

    /* ── Top-level Toolbar (P0) ─────────────────────────────────────
       Drawn inline inside the DockSpace host (see above); the legacy
       standalone call is now a no-op kept only for ABI continuity. */
    /* Bottom Status Bar (P0) */
    jce_editor_panel_status_bar();

    panel_phase("ed_p_input_manager", &ed_pt);
    /* ── Profiler ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER)) {
        char title[64];
        snprintf(title, sizeof(title), "%s###profiler", jce_editor_i18n("panel.profiler"));
        jce_editor_panel_default_pose(title);
        if (ImGui::Begin(title,
                         jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER), ImGuiWindowFlags_NoFocusOnAppearing))
        {
            jce_editor_panel_profiler_content();
        }
        ImGui::End();
    }

    /* ── Profile Analyzer (shim → Profiler workbench) ─────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PROFILE_ANALYZER)) {
        jce_editor_panel_profile_analyzer();
    }

    /* panel_phase attributes everything since the PREVIOUS marker, so a
     * marker has to sit immediately after the panels it names.  This one used
     * to sit seven panels later, which made "ed_p_profiler" a bucket holding
     * the particle editor, the material graph, import presets, lightmap bake
     * and the curve editor as well -- and reading that bucket as the cost of
     * the Profiler panel is exactly the wrong conclusion to draw from it. */
    panel_phase("ed_p_profiler", &ed_pt);

    /* ── Particle Editor (shim → Material Graph workbench) ────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PARTICLE_EDITOR)) {
        jce_editor_panel_particle_editor();
    }

    /* ── Material Graph (workbench host) ─────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH)) {
        jce_editor_panel_default_pose("material_graph");
        jce_editor_panel_material_graph();
    }

    panel_phase("ed_p_graph_editors", &ed_pt);

    /* ── Import Presets (shim → Bundle Browser workbench) ────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_IMPORT_PRESETS)) {
        jce_editor_panel_import_presets();
    }

    /* ── Lightmap Bake (merged into Lighting Settings workbench) ───── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTMAP_BAKE)) {
        /* Redirects to the Lighting Settings workbench's Lightmap tab. */
        jce_editor_panel_lightmap_bake();
    }

    /* ── Curve Editor (merged into Animation Editor workbench) ───── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_CURVE_EDITOR)) {
        /* Redirects to the Animation Editor workbench's Curves tab. */
        jce_editor_panel_curve_editor();
    }

    panel_phase("ed_p_bake_curves", &ed_pt);
    /* ── Animation Editor ────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR)) {
        jce_editor_panel_default_pose("animation_editor");
        jce_editor_panel_animation_editor();
    }

    /* ── Animator State Machine (merged into Animation Editor workbench) ── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATOR_SM)) {
        /* Redirects to the Animation Editor workbench's State Machine tab. */
        jce_editor_panel_animator_sm();
    }

    /* ── Animation Rigging (merged into Animation Editor workbench) ── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_RIGGING)) {
        /* Redirects to the Animation Editor workbench's Rigging tab. */
        jce_editor_panel_animation_rigging();
    }

    /* ── Sequencer (merged into Animation Editor workbench) ───────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SEQUENCER)) {
        /* Redirects to the Animation Editor workbench's Sequencer tab. */
        jce_editor_panel_sequencer();
    }

    panel_phase("ed_p_animation_edit", &ed_pt);
    /* ── NavMesh ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_NAVMESH)) {
        jce_editor_panel_default_pose("navmesh");
        jce_editor_panel_navmesh();
    }

    panel_phase("ed_p_navmesh", &ed_pt);
    /* ── BT Visualizer ───────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_BT_VISUALIZER)) {
        jce_editor_panel_default_pose("bt_visualizer");
        jce_editor_panel_bt_visualizer();
    }

    panel_phase("ed_p_bt_visualizer", &ed_pt);
    /* ── World Streaming ─────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_AI_ASSISTANT)) {
        jce_editor_panel_default_pose("ai_assistant");
        jce_editor_panel_ai_assistant();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_WORLD_STREAMING)) {
        jce_editor_panel_default_pose("world_streaming");
        jce_editor_panel_world_streaming();
    }

    panel_phase("ed_p_world_streamin", &ed_pt);
    /* ── Terrain ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN)) {
        jce_editor_panel_default_pose("terrain");
        jce_editor_panel_terrain();
    }

    /* ── Render Pipeline (merged into Lighting Settings workbench) ── */
    jce_editor_panel_render_pipeline_tick();
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_RENDER_PIPELINE)) {
        /* Redirects to the Lighting Settings workbench's Pipeline tab. */
        jce_editor_panel_render_pipeline();
    }
    panel_phase("ed_p_terrain", &ed_pt);
}

/* ══════════════════════════════════════════════════════════════════════
/* ══════════════════════════════════════════════════════════════════════
 *  STATUS BAR (bottom of viewport)
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_status_bar(void)
{
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float h = ImGui::GetFrameHeight();

    /* Proximity-based fade: status bar is fully visible by default and only
       fades when the cursor is VERY close (so it rarely obscures content,
       but you can still hover-through to click on docked content directly
       behind it). Within 6 px → 0.0 (fully gone, can click through);
       6–40 px → linear fade; beyond 40 px → fully visible. Rendered as an
       OVERLAY (not a viewport sidebar) so the docked content underneath
       shows through cleanly instead of fading to a black gap. */
    float bar_top_y   = vp->Pos.y + vp->Size.y - h;
    ImVec2 mp         = ImGui::GetMousePos();
    float dy          = bar_top_y - mp.y;            /* >0 above, <0 inside */
    float alpha;
    if (dy <= 6.0f) {
        alpha = 0.0f;
    } else if (dy < 40.0f) {
        alpha = ((dy - 6.0f) / 34.0f);
    } else {
        alpha = 1.0f;
    }
    if (alpha <= 0.01f) return;                      /* fully hidden */

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::SetNextWindowBgAlpha(alpha * 0.85f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration
                        | ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoSavedSettings
                        | ImGuiWindowFlags_NoFocusOnAppearing
                        | ImGuiWindowFlags_NoBringToFrontOnFocus
                        | ImGuiWindowFlags_NoNav
                        | ImGuiWindowFlags_NoDocking;

    if (!ImGui::Begin("##JCEStatusBar", NULL, wf)) {
        ImGui::End();
        ImGui::PopStyleVar(4);
        return;
    }

    /* ── Left: Play badge ─────────────────────────────────────────── */
    JcePlayState ps = jce_state_get_play_state();
    ImVec4 badge_col;
    const char *badge_key = "statusBar.play.stopped";
    switch (ps) {
        case JCE_PLAY_PLAYING: badge_col = ImVec4(0.95f, 0.45f, 0.10f, 1.0f);
                               badge_key = "statusBar.play.playing"; break;
        case JCE_PLAY_PAUSED:  badge_col = ImVec4(0.95f, 0.70f, 0.10f, 1.0f);
                               badge_key = "statusBar.play.paused";  break;
        default:               badge_col = ImVec4(0.30f, 0.55f, 0.30f, 1.0f); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, badge_col);
    ImGui::Text("[%s]", jce_editor_i18n(badge_key));
    ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 8.0f); ImGui::TextDisabled("|"); ImGui::SameLine(0.0f, 8.0f);

    /* ── Scene name + dirty marker ────────────────────────────────── */
    const char *scene_path = jce_state_get_current_scene_path();
    const char *scene_name = (scene_path && scene_path[0])
        ? scene_path
        : jce_editor_i18n("statusBar.untitled");
    /* Show only basename if it looks like a path. */
    scene_name = jce_editor_path_basename_view(scene_name);

    ImGui::Text("%s %s", jce_editor_i18n("statusBar.scene"), scene_name);
    if (jce_state_is_scene_modified()) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.10f, 1.0f));
        ImGui::TextUnformatted(jce_editor_i18n("statusBar.unsavedDot"));
        ImGui::PopStyleColor();
    }

    /* Changed on disk while this editor has unsaved edits -- the one state in
     * which what is on screen is not what the file says.  Shown here and not
     * only as a toast because the toast lasts four seconds and the decision it
     * matters for (approving an agent's scene before its changeset is
     * committed) can happen an hour later.  The reload discards human edits no
     * undo stack can return, so it asks first. */
    if (jce_editor_scene_file_watch_is_stale()) {
        static bool s_confirm_reload = false;
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.25f, 1.0f));
        ImGui::TextUnformatted(jce_editor_i18n("statusBar.sceneChangedOnDisk"));
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0f, 6.0f);
        if (!s_confirm_reload) {
            if (ImGui::SmallButton(jce_editor_i18n("statusBar.reloadFromDisk")))
                s_confirm_reload = true;
        } else if (ImGui::SmallButton(jce_editor_i18n("statusBar.reloadConfirm"))) {
            s_confirm_reload = false;
            jce_editor_scene_file_watch_reload_now();
        }
    }

    ImGui::SameLine(0.0f, 8.0f); ImGui::TextDisabled("|"); ImGui::SameLine(0.0f, 8.0f);

    /* ── Selection count + focused entity name ───────────────────── */
    int sel_count = 0;
    (void)jce_state_get_selection(&sel_count);
    uint32_t focused = jce_state_get_focused();
    if (sel_count > 0) {
        const char *focused_name = focused ? jce_state_entity_name(focused) : NULL;
        if (sel_count == 1 && focused_name)
            ImGui::Text("%s %s", jce_editor_i18n("statusBar.selected"), focused_name);
        else
            ImGui::Text("%s %d", jce_editor_i18n("statusBar.selected"), sel_count);
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("statusBar.ready"));
    }

    ImGui::SameLine(0.0f, 8.0f); ImGui::TextDisabled("|"); ImGui::SameLine(0.0f, 8.0f);

    /* ── Entity count ─────────────────────────────────────────────── */
    ImGui::Text("%s %d", jce_editor_i18n("statusBar.entities"),
                jce_state_get_entity_count());

    /* ── Right: FPS + DPI ─────────────────────────────────────────── */
    char rbuf[128];
    float fps = ImGui::GetIO().Framerate;
    float dpi = ImGui::GetIO().FontGlobalScale;
    snprintf(rbuf, sizeof(rbuf), "%s %.0f   %s %.0f%%",
             jce_editor_i18n("statusBar.fps"), fps,
             jce_editor_i18n("statusBar.dpi"), dpi * 100.0f);
    float right_edge = ImGui::GetWindowContentRegionMax().x;
    float w = ImGui::CalcTextSize(rbuf).x;
    ImGui::SameLine(right_edge - w - 8.0f);
    ImGui::TextUnformatted(rbuf);

    ImGui::End();
    ImGui::PopStyleVar(4);
}

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN DRAW
 * ══════════════════════════════════════════════════════════════════════ */

void jce_editor_layout_draw(void)
{
    resolve_startup_welcome_once();

    /* Play Mode tint: push orange title-bar/border colors so every panel
       (DockSpace, child windows, dialogs) clearly signals we're playing. */
    JcePlayState _play = jce_state_get_play_state();
    int _play_tint = 0;
    if (_play != JCE_PLAY_STOPPED) {
        const ImVec4 orange       = (_play == JCE_PLAY_PAUSED)
            ? ImVec4(0.95f, 0.70f, 0.10f, 1.0f)
            : ImVec4(0.95f, 0.45f, 0.10f, 1.0f);
        const ImVec4 orange_dim   = ImVec4(orange.x*0.55f, orange.y*0.45f, orange.z*0.30f, 1.0f);
        const ImVec4 orange_bord  = ImVec4(orange.x, orange.y, orange.z, 0.85f);
        ImGui::PushStyleColor(ImGuiCol_TitleBg,         orange_dim);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,   orange);
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,orange_dim);
        ImGui::PushStyleColor(ImGuiCol_Border,          orange_bord);
        ImGui::PushStyleColor(ImGuiCol_TabActive,       orange_dim);
        ImGui::PushStyleColor(ImGuiCol_TabHovered,      orange);
        ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, orange);
        _play_tint = 7;
    }

    /* Bottom status bar — must be created before host window so it
       reduces viewport->WorkSize and the DockSpace adapts. Respect the
       Window > Status Bar visibility toggle. */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_STATUS_BAR))
        draw_status_bar();

    /* Full-viewport host window for the menu bar + DockSpace. */
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    /* Menu bar */
    {
        uint64_t _t0_menu = jce_time_perf_counter();
        draw_menu_bar();
        jce_perf_phase_add("ed_menu", jce_time_perf_to_ms(_t0_menu,
                                                          jce_time_perf_counter()));
    }

    /* Top toolbar — drawn inline so the Window > Toolbar visibility
       toggle re-flows the layout (the DockSpace below naturally takes
       the row back when the toolbar is hidden). Previously this lived
       in a separate pinned window which the DockSpace host covered. */
    jce_editor_panel_toolbar_inline();

    /* Create DockSpace. */
    ImGuiID dockspace_id = ImGui::GetID("JCEDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    /* Setup default layout only on first frame AND only if no saved
       layout exists (.jce/imgui.ini).  When .jce/imgui.ini is present, ImGui
       restores the user's docking arrangement automatically. */
    if (!s_layout_initialized) {
        s_layout_initialized = true;
        ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace_id);
        if (!node || node->ChildNodes[0] == NULL) {
            setup_default_docking_layout(dockspace_id);
            s_deferred_focus_frames = 3;
        }
    }

    /* Handle reset layout request from Window menu. */
    if (s_reset_layout_requested) {
        s_reset_layout_requested = false;
        /* Re-enable all panels. */
        for (int p = 0; p < JCE_PANEL_COUNT; p++) {
            if (p != JCE_PANEL_PREFERENCES && p != JCE_PANEL_POSTFX
                && p != JCE_PANEL_PROFILER && p != JCE_PANEL_PARTICLE_EDITOR
                && p != JCE_PANEL_MATERIAL_GRAPH
                && p != JCE_PANEL_IMPORT_PRESETS
                && p != JCE_PANEL_LIGHTMAP_BAKE
                && p != JCE_PANEL_CURVE_EDITOR
                && p != JCE_PANEL_ANIMATION_EDITOR
                && p != JCE_PANEL_FRAME_DEBUGGER
                && p != JCE_PANEL_SPRITE_EDITOR
                && p != JCE_PANEL_TILE_PALETTE
                && p != JCE_PANEL_VFX_GRAPH
                && p != JCE_PANEL_TEST_RUNNER
                && p != JCE_PANEL_BUILD_PROFILES
                && p != JCE_PANEL_MEMORY_PROFILER
                && p != JCE_PANEL_PHYSICS_DEBUGGER
                && p != JCE_PANEL_LIGHT_EXPLORER
                && p != JCE_PANEL_REFLECTION_PROBES
                && p != JCE_PANEL_SHADER_GRAPH
                && p != JCE_PANEL_SHADER_INSPECTOR
                && p != JCE_PANEL_SEARCH
                && p != JCE_PANEL_VERSION_CONTROL
                && p != JCE_PANEL_TIME_OF_DAY
                && p != JCE_PANEL_VCAM_MANAGER
                && p != JCE_PANEL_REVERB_ZONES
                && p != JCE_PANEL_SAVE_BROWSER
                && p != JCE_PANEL_BUNDLE_BROWSER
                && p != JCE_PANEL_PHYSICS_LAYERS
                && p != JCE_PANEL_TAGS_LAYERS
                && p != JCE_PANEL_LIGHTING_SETTINGS
                && p != JCE_PANEL_BUILD_REPORT
                && p != JCE_PANEL_SYSTEMS
                && p != JCE_PANEL_PROJECT_SETTINGS
                && p != JCE_PANEL_LAN_DISCOVERY
                && p != JCE_PANEL_NETWORK_STATS
                && p != JCE_PANEL_ANIMATION_RIGGING)
                *jce_editor_panel_visible_ptr((JceEditorPanel)p) = true;
        }
        *jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX)           = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER)         = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_PARTICLE_EDITOR)  = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_IMPORT_PRESETS)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTMAP_BAKE)    = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_CURVE_EDITOR)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR) = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR)    = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH)        = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER)      = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER)  = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER) = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_REFLECTION_PROBES)= false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_GRAPH)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_INSPECTOR) = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH)           = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL)  = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY)      = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_LAYERS)   = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_TAGS_LAYERS)      = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS)= false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_REPORT)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SYSTEMS)          = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_PROJECT_SETTINGS) = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_LAN_DISCOVERY)    = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_NETWORK_STATS)    = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_RIGGING)= false;

        /* Animation preset: enable anim/curve panels so they actually dock. */
        if (s_layout_preset_pending == 2) {
            *jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR) = true;
            *jce_editor_panel_visible_ptr(JCE_PANEL_CURVE_EDITOR)     = true;
        }

        apply_layout_preset(dockspace_id, s_layout_preset_pending);
        s_layout_preset_pending = 0;
        s_deferred_focus_frames = 3;
    }

    /* Deferred tab focus — ensure layout is applied before focusing. */
    if (s_deferred_focus_frames > 0) {
        s_deferred_focus_frames--;
        if (s_deferred_focus_frames == 0) {
            ImGui::SetWindowFocus("Inspector###inspector");
            ImGui::SetWindowFocus("Scene###scene_view");
        }
    }

    ImGui::End(); /* DockSpace */

    /* Draw all panel windows (dockable). */
    {
        uint64_t _t0_panels = jce_time_perf_counter();
        draw_panel_windows();
        jce_perf_phase_add("ed_panels", jce_time_perf_to_ms(_t0_panels,
                                                            jce_time_perf_counter()));
    }

    /* Maximised Game viewport.
     *
     * Drawn after the dock host and the docked panels so it paints on top (the
     * host carries NoBringToFrontOnFocus). NoSavedSettings keeps this window
     * out of imgui.ini entirely, so toggling it can never perturb the saved
     * dock layout. WorkPos/WorkSize rather than Pos/Size: the menu bar and the
     * status bar stay reachable, which is what makes this a viewing mode and
     * not a trap the user cannot get out of.
     *
     * jce_editor_panel_game_view_content() drives jce_editor_game_render_frame()
     * and publishes the input-bridge viewport, so it must run EXACTLY once per
     * frame -- the docked block above is skipped whenever this one runs. */
    if (s_maximized_panel != JCE_PANEL_COUNT) {
        if (!*jce_editor_panel_visible_ptr(s_maximized_panel)) {
            /* The panel was hidden while maximised; leave the mode rather than
             * showing a window the user just closed. */
            s_maximized_panel = JCE_PANEL_COUNT;
        } else {
            const ImGuiViewport *mv = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(mv->WorkPos);
            ImGui::SetNextWindowSize(mv->WorkSize);
            ImGui::SetNextWindowViewport(mv->ID);
            ImGuiWindowFlags mflags =
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoNavFocus;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin("###viewport_maximized", nullptr, mflags)) {
                if (s_maximized_panel == JCE_PANEL_SCENE_VIEW)
                    jce_editor_panel_scene_view_content();
                else
                    jce_editor_panel_game_view_content();
            } else if (s_maximized_panel == JCE_PANEL_GAME_VIEW) {
                jce_editor_game_input_bridge_publish(
                    jce_editor_game_input_bridge_shared(), nullptr);
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
        }
    }

    /* Panels get first refusal for context-specific editing shortcuts
     * (Assets, Material Graph, Hierarchy). Anything unconsumed falls back
     * to scene/entity commands here. */
    handle_global_edit_shortcuts();

    /* Preferences (modal popup; the retired dockable JCE_PANEL_PREFERENCES
     * panel was removed — JCE_PANEL_USER_PREFERENCES is the single one). */
    jce_editor_panel_default_pose("user_preferences");
    jce_editor_panel_user_preferences();

    if (should_draw_dialog_dimmer())
        draw_dialog_dimmer();

    /* Dialogs */
    jce_editor_about_dialog(&s_show_about);
    jce_editor_inspector_delete_dialog();
    jce_editor_dialog_new_project(&s_show_new_project);
    jce_editor_dialog_open_project(&s_show_open_project);
    jce_editor_dialog_welcome(&s_show_welcome);
    jce_editor_dialog_open_scene(&s_show_open_scene);
    jce_editor_dialog_open_bundle(&s_show_open_bundle);
    jce_editor_dialog_save_as(&s_show_save_as);
    jce_editor_dialog_unsaved_changes(&s_show_unsaved, &s_unsaved_result);
    jce_editor_dialog_build_settings(&s_show_build);
    jce_editor_dialog_bundles(&s_show_bundles);
    jce_editor_dialog_project_settings(&s_show_proj_settings);
    jce_editor_dialog_preferences(&s_show_preferences);
    jce_editor_asset_picker_draw();
    draw_command_palette();

    /* An engine assertion recorded on some other thread surfaces here, on the
     * UI thread, before the toast overlay that shows it. */
    jce_editor_assert_bridge_drain();

    /* Toast overlay — draw last so it renders on top of everything. */
    jce_editor_toast_draw();

    /* Frame-sliced scene load in progress: dim + gate the whole editor so the
     * user cannot operate on a scene that is still being created.  Drawn after
     * everything else so it sits on top of all panels and dialogs. */
    if (jce_state_is_scene_loading())
        draw_scene_loading_overlay();

    /* Unsaved-changes modal resolved → carry out the pending action.
     * The SAME state machine serves quit and the scene-swap gate; which
     * one is distinguished by s_pending_action.  result 1 = Save,
     * 2 = Don't Save, 3 = Cancel. */
    if (!s_show_unsaved && s_unsaved_result != 0) {
        bool is_quit  = (s_pending_action == PGA_QUIT);
        bool confirmed = false;     /* proceed with the action now */
        bool defer_after_save = false;  /* Save needs Save-As first */

        if (s_unsaved_result == 1) {
            SaveSceneResult save_result = save_scene_or_open_save_as();
            if (save_result == SAVE_SCENE_RESULT_OK)
                confirmed = true;
            else if (save_result == SAVE_SCENE_RESULT_NEEDS_PATH)
                defer_after_save = true;
            /* SAVE_SCENE_RESULT_FAILED: abort, keep the scene as-is. */
        } else if (s_unsaved_result == 2) {
            confirmed = true;       /* Don't Save → discard */
        }
        /* result == 3 (Cancel) or save failed: abort, do nothing. */

        if (is_quit) {
            if (confirmed)        s_quit_confirmed     = true;
            if (defer_after_save) s_quit_after_save_as = true;
        } else {
            if (confirmed) {
                run_gated_action(s_pending_action, s_pending_path);
            } else if (defer_after_save) {
                s_run_pending_after_save = true;
            }
        }

        /* Clear the gate now unless we are deferring until Save-As resolves
         * (in which case s_pending_action is needed by the follow-up block). */
        if (!defer_after_save) {
            s_pending_action = PGA_NONE;
            s_pending_path[0] = '\0';
        }
        s_unsaved_result = 0;
    }

    /* Save-As (chosen via the modal's "Save") has resolved — finish the
     * deferred quit or gated action now that a scene path exists. */
    if ((s_quit_after_save_as || s_run_pending_after_save) && !s_show_save_as) {
        const char *scene_path = jce_state_get_current_scene_path();
        bool saved = (scene_path && scene_path[0] != '\0');
        if (s_quit_after_save_as) {
            if (saved) s_quit_confirmed = true;
            s_quit_after_save_as = false;
            /* Whether the save resolved or the user backed out of Save-As,
             * the quit gate is done — release it so later actions aren't
             * blocked by a lingering PGA_QUIT. */
            s_pending_action = PGA_NONE;
            s_pending_path[0] = '\0';
        }
        if (s_run_pending_after_save) {
            if (saved)
                run_gated_action(s_pending_action, s_pending_path);
            s_run_pending_after_save = false;
            s_pending_action = PGA_NONE;
            s_pending_path[0] = '\0';
        }
    }

    /* Play Mode tint: pop styles + draw a thick orange outline around the
       full main viewport so the active state is unmissable. */
    if (_play_tint > 0) {
        ImGui::PopStyleColor(_play_tint);

        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImDrawList *fg = ImGui::GetForegroundDrawList(vp);
        ImU32 col = (_play == JCE_PLAY_PAUSED)
            ? IM_COL32(242, 178, 25,  255)
            : IM_COL32(242, 115, 25,  255);
        ImVec2 a = vp->WorkPos;
        ImVec2 b = ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                          vp->WorkPos.y + vp->WorkSize.y);
        const float th = 4.0f;
        fg->AddRect(a, b, col, 0.0f, 0, th);
        const char *lbl = (_play == JCE_PLAY_PAUSED) ? "PAUSED" : "PLAYING";
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        float pad = 8.0f;
        ImVec2 ba = ImVec2(a.x + (b.x - a.x) * 0.5f - ts.x * 0.5f - pad, a.y + th);
        ImVec2 bb = ImVec2(ba.x + ts.x + pad * 2.0f, ba.y + ts.y + pad);
        fg->AddRectFilled(ba, bb, col, 4.0f);
        fg->AddText(ImVec2(ba.x + pad, ba.y + pad * 0.5f),
                    IM_COL32(20, 20, 20, 255), lbl);
    }

    /* Persist Window menu visibility (writes only when mask changes). */
    jce_editor_panels_persist_visibility();
}

void jce_editor_layout_request_focus_scene_view(void)
{
    *jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW) = true;
    s_focus_scene_view = true;
}

void jce_editor_layout_request_focus_game_view(void)
{
    *jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW) = true;
    s_focus_game_view = true;
}

void jce_editor_layout_request_focus_inspector(void)
{
    *jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR) = true;
    s_focus_inspector = true;
}

void jce_editor_layout_request_focus_file_viewer(void)
{
    *jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER) = true;
    s_focus_file_viewer = true;
}

void jce_editor_layout_request_quit(void)
{
    if (s_quit_confirmed) return;
    if (s_show_unsaved)   return; /* already showing */

    if (!jce_state_is_scene_modified()) {
        s_quit_confirmed = true;
        return;
    }

    s_pending_action = PGA_QUIT;
    s_unsaved_result = 0;
    s_show_unsaved   = true;
}

bool jce_editor_layout_is_quit_confirmed(void)
{
    return s_quit_confirmed;
}
