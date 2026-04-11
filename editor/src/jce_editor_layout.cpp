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
 * Menu bar matches reference EditorUI.java:
 *   File | Edit | GameObject | Window | Help | [centered Play controls]
 */

#include "jce_editor_layout.h"
#include "jce_editor_panels.h"
#include "jce_editor_dialogs.h"
#include "jce_editor_state.h"
#include "jce_editor_i18n.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <SDL3/SDL.h>
#include <stdio.h>

/* ── Dialog state ─────────────────────────────────────────────────── */

static bool s_show_about       = false;
static bool s_show_settings    = false;
static bool s_show_new_project = false;
static bool s_show_open_project = false;
static bool s_show_new_scene   = false;
static bool s_show_open_scene  = false;
static bool s_show_save_as     = false;
static bool s_show_unsaved     = false;
static int  s_unsaved_result   = 0;
static bool s_quit_after_save_as = false;

/* ── Docking state ────────────────────────────────────────────────── */

static bool s_layout_initialized = false;
static int  s_deferred_focus_frames = 0;
static bool s_reset_layout_requested = false;
static bool s_focus_scene_view = false;
static bool s_focus_inspector = false;
static bool s_focus_file_viewer = false;

static bool should_draw_dialog_dimmer(void)
{
    return s_show_about
        || s_show_settings
        || s_show_new_project
        || s_show_open_project
        || s_show_new_scene
        || s_show_open_scene
        || s_show_save_as
        || s_show_unsaved
    || jce_editor_assets_delete_dialog_open()
    || jce_editor_inspector_delete_dialog_open();
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
        ImGuiWindowFlags_NoInputs;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

    ImGui::Begin("##DialogDimmer", NULL, flags);
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void request_app_quit(void)
{
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
}

typedef enum {
    SAVE_SCENE_RESULT_FAILED = 0,
    SAVE_SCENE_RESULT_OK,
    SAVE_SCENE_RESULT_NEEDS_PATH,
} SaveSceneResult;

static SaveSceneResult save_scene_or_open_save_as(void)
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
    return SAVE_SCENE_RESULT_OK;
}

static void handle_global_edit_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyCtrl)
        return;

    /* Avoid stealing shortcuts while typing in text fields. */
    if (io.WantTextInput)
        return;

    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        if (jce_state_can_undo())
            jce_state_undo();
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Y, false)
        || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
    {
        if (jce_state_can_redo())
            jce_state_redo();
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  MENU BAR
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_menu_bar(void)
{
    if (!ImGui::BeginMenuBar()) return;

    /* Wider spacing between menu items to match reference editor. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(16, 4));

    /* ── File ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.file"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.newScene"),    "Ctrl+N"))
            s_show_new_scene = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.openScene"),   "Ctrl+O"))
            s_show_open_scene = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveScene"),   "Ctrl+S"))
            save_scene_or_open_save_as();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveAs"),   "Ctrl+Shift+S"))
            s_show_save_as = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.new")))
            s_show_new_project = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.open")))
            s_show_open_project = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.exit"), "Alt+F4")) {
            s_unsaved_result = 0;
            s_show_unsaved = true;
        }
        ImGui::EndMenu();
    }

    /* ── Edit ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.edit"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.undo"),  "Ctrl+Z", false, jce_state_can_undo()))
            jce_state_undo();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.redo"),  "Ctrl+Y", false, jce_state_can_redo()))
            jce_state_redo();
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"),  "Ctrl+C"))  { /* TODO */ }
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V"))  { /* TODO */ }
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.duplicate"), "Ctrl+D")) {
            uint32_t f = jce_state_get_focused();
            if (f) {
                uint32_t d = jce_state_duplicate_entity(f);
                jce_state_select_entity(d, false);
            }
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.delete"), "Del")) {
            uint32_t f = jce_state_get_focused();
            if (f) jce_editor_inspector_request_delete_confirm(f);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.settings")))
            s_show_settings = true;
        ImGui::EndMenu();
    }

    /* ── GameObject (was "Create") ─────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createEmpty"))) {
            uint32_t id = jce_state_create_entity("New Entity", 0);
            jce_state_select_entity(id, false);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject.3dObject"))) {
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                uint32_t id = jce_state_create_entity("Cube", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                uint32_t id = jce_state_create_entity("Sphere", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                uint32_t id = jce_state_create_entity("Plane", 0);
                jce_state_select_entity(id, false);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
            uint32_t id = jce_state_create_entity("Camera", 0);
            jce_state_select_entity(id, false);
        }
        if (ImGui::BeginMenu(jce_editor_i18n("menu.gameObject.createLight"))) {
            if (ImGui::MenuItem(jce_editor_i18n("light.directional"))) {
                uint32_t id = jce_state_create_entity("Directional Light", 0);
                jce_state_select_entity(id, false);
            }
            if (ImGui::MenuItem(jce_editor_i18n("light.point"))) {
                uint32_t id = jce_state_create_entity("Point Light", 0);
                jce_state_select_entity(id, false);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    /* ── Window (was "View") ───────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.window"))) {
        ImGui::MenuItem(jce_editor_i18n("Hierarchy"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY));
        ImGui::MenuItem(jce_editor_i18n("Inspector"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR));
        ImGui::MenuItem(jce_editor_i18n("Console"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE));
        ImGui::Separator();
        ImGui::MenuItem(jce_editor_i18n("Scene"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW));
        ImGui::MenuItem(jce_editor_i18n("Game"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW));
        ImGui::Separator();
        ImGui::MenuItem(jce_editor_i18n("Timeline"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE));
        ImGui::MenuItem(jce_editor_i18n("Asset Browser"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS));
        ImGui::MenuItem(jce_editor_i18n("File Viewer"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER));
        ImGui::MenuItem(jce_editor_i18n("postfx.title"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX));
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.window.resetLayout")))
            s_reset_layout_requested = true;
        ImGui::EndMenu();
    }

    /* ── Help ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.help"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.help.about")))
            s_show_about = true;
        ImGui::EndMenu();
    }

    /* ── Play controls (right-aligned, matches reference) ──────────── */
    {
        /* Calculate width of play + stop buttons + spacing for right-alignment. */
        float btn_w = ImGui::CalcTextSize(" > ").x + ImGui::GetStyle().FramePadding.x * 2
                    + ImGui::GetStyle().ItemSpacing.x
                    + ImGui::CalcTextSize(" [] ").x + ImGui::GetStyle().FramePadding.x * 2;
        float right_edge = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(right_edge - btn_w);

        JcePlayState ps = jce_state_get_play_state();

        ImGui::PushStyleColor(ImGuiCol_Button,
            ps == JCE_PLAY_PLAYING ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f)
                                   : ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
        if (ImGui::SmallButton(ps == JCE_PLAY_STOPPED ? " > " : " || ")) {
            if (ps == JCE_PLAY_STOPPED) jce_state_play();
            else jce_state_pause();
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        bool can_stop = (ps != JCE_PLAY_STOPPED);
        if (!can_stop) ImGui::BeginDisabled();
        if (ImGui::SmallButton(" [] ")) jce_state_stop();
        if (!can_stop) ImGui::EndDisabled();
    }

    ImGui::PopStyleVar(); /* ItemSpacing for menu bar */
    ImGui::EndMenuBar();
}

/* ══════════════════════════════════════════════════════════════════════
 *  DEFAULT DOCKING LAYOUT (first frame only)
 * ══════════════════════════════════════════════════════════════════════ */

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
       Dock order is reversed — first docked ends up as back tab. */
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",       left_id);

    ImGui::DockBuilderDockWindow("Game###game_view",            center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);

    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);

    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);

    ImGui::DockBuilderFinish(dockspace_id);
}

/* ══════════════════════════════════════════════════════════════════════
 *  PANEL WINDOWS
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_panel_windows(void)
{
    char lbl[256];

    /* ── Hierarchy ────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY)) {
        snprintf(lbl, sizeof(lbl), "%s###hierarchy", jce_editor_i18n("Hierarchy"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY))) {
            jce_editor_panel_hierarchy_content();
        }
        ImGui::End();
    }

    /* ── Scene View ───────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW)) {
        snprintf(lbl, sizeof(lbl), "%s###scene_view", jce_editor_i18n("Scene"));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (s_focus_scene_view) {
            ImGui::SetNextWindowFocus();
            s_focus_scene_view = false;
        }
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW))) {
            jce_editor_panel_scene_view_content();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /* ── Game View ────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW)) {
        snprintf(lbl, sizeof(lbl), "%s###game_view", jce_editor_i18n("Game"));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW))) {
            jce_editor_panel_game_view_content();
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /* ── Inspector ────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR)) {
        snprintf(lbl, sizeof(lbl), "%s###inspector", jce_editor_i18n("Inspector"));
        if (s_focus_inspector) {
            ImGui::SetNextWindowFocus();
            s_focus_inspector = false;
        }
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR))) {
            jce_editor_panel_inspector_content();
        }
        ImGui::End();
    }

    /* ── File Viewer ──────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER)) {
        snprintf(lbl, sizeof(lbl), "%s###file_viewer", jce_editor_i18n("File Viewer"));
        if (s_focus_file_viewer) {
            ImGui::SetNextWindowFocus();
            s_focus_file_viewer = false;
        }
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER))) {
            jce_editor_panel_file_viewer_content();
        }
        ImGui::End();
    }

    /* ── Console ──────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE)) {
        snprintf(lbl, sizeof(lbl), "%s###console", jce_editor_i18n("Console"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE))) {
            jce_editor_panel_console_content();
        }
        ImGui::End();
    }

    /* ── Timeline ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE)) {
        snprintf(lbl, sizeof(lbl), "%s###timeline", jce_editor_i18n("Timeline"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE))) {
            jce_editor_panel_timeline_content();
        }
        ImGui::End();
    }

    /* ── Asset Browser ────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS)) {
        snprintf(lbl, sizeof(lbl), "%s###assets", jce_editor_i18n("Asset Browser"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS))) {
            jce_editor_panel_assets_content();
        }
        ImGui::End();
    }

    /* ── Post Processing ──────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX)) {
        snprintf(lbl, sizeof(lbl), "%s###postfx", jce_editor_i18n("postfx.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX))) {
            jce_editor_panel_postfx_content();
        }
        ImGui::End();
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN DRAW
 * ══════════════════════════════════════════════════════════════════════ */

void jce_editor_layout_draw(void)
{
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

    handle_global_edit_shortcuts();

    /* Menu bar */
    draw_menu_bar();

    /* Create DockSpace. */
    ImGuiID dockspace_id = ImGui::GetID("JCEDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    /* Setup default layout only on first frame AND only if no saved
       layout exists (imgui.ini).  When imgui.ini is present, ImGui
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
            if (p != JCE_PANEL_PREFERENCES)
                *jce_editor_panel_visible_ptr((JceEditorPanel)p) = true;
        }
        setup_default_docking_layout(dockspace_id);
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
    draw_panel_windows();

    /* Preferences (floating). */
    jce_editor_panel_preferences();

    if (should_draw_dialog_dimmer())
        draw_dialog_dimmer();

    /* Dialogs */
    jce_editor_about_dialog(&s_show_about);
    jce_editor_settings_dialog(&s_show_settings);
    jce_editor_inspector_delete_dialog();
    jce_editor_dialog_new_project(&s_show_new_project);
    jce_editor_dialog_open_project(&s_show_open_project);
    jce_editor_dialog_new_scene(&s_show_new_scene);
    jce_editor_dialog_open_scene(&s_show_open_scene);
    jce_editor_dialog_save_as(&s_show_save_as);
    jce_editor_dialog_unsaved_changes(&s_show_unsaved, &s_unsaved_result);

    if (!s_show_unsaved && s_unsaved_result != 0) {
        if (s_unsaved_result == 1) {
            SaveSceneResult save_result = save_scene_or_open_save_as();
            if (save_result == SAVE_SCENE_RESULT_OK)
                request_app_quit();
            else if (save_result == SAVE_SCENE_RESULT_NEEDS_PATH)
                s_quit_after_save_as = true;
        } else if (s_unsaved_result == 2) {
            request_app_quit();
        }
        s_unsaved_result = 0;
    }

    if (s_quit_after_save_as && !s_show_save_as) {
        const char *scene_path = jce_state_get_current_scene_path();
        if (scene_path && scene_path[0] != '\0')
            request_app_quit();
        s_quit_after_save_as = false;
    }
}

void jce_editor_layout_request_focus_scene_view(void)
{
    *jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW) = true;
    s_focus_scene_view = true;
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
