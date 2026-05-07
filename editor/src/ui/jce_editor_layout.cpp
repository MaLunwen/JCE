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

#include "dialogs/jce_editor_dialogs.h"
#include "core/jce_editor.h"
#include "jce_editor_colors.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_config.h"
extern "C" {
#include <jce/os/core/jce_filesystem.h>
}
#include "core/jce_editor_toast.h"
#include "core/jce_hotkeys.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/middleware/scene/jce_lod.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/tools/jce_imgui.hpp>
#include <jce/tools/jce_imgui_internal.h>
#include <stdio.h>
#include <string.h>

/* ── Dialog state ─────────────────────────────────────────────────── */

static bool s_show_about       = false;
static bool s_demo_lod_enabled = false;
static JceLodGroup s_demo_lod_group = {};
static bool s_show_new_project = false;
static bool s_show_open_project = false;
static bool s_show_new_scene   = false;
static bool s_show_open_scene  = false;
static bool s_show_save_as     = false;
static bool s_show_unsaved     = false;
static bool s_show_build       = false;
static bool s_show_proj_settings = false;
static bool s_show_preferences = false;
static int  s_unsaved_result   = 0;
static bool s_quit_after_save_as = false;
static bool s_quit_confirmed = false;

/* ── Docking state ────────────────────────────────────────────────── */

static bool s_layout_initialized = false;
static int  s_deferred_focus_frames = 0;
static bool s_reset_layout_requested = false;
/* Layout preset to apply on next reset. 0=Default, 1=Wide, 2=Animation, 3=TwoByTwo. */
static int  s_layout_preset_pending = 0;
static bool s_focus_scene_view = false;
static bool s_focus_inspector = false;
static bool s_focus_file_viewer = false;
static void cmd_toggle_demo_lod_(void);  /* fwd-decl: defined further down */

static bool should_draw_dialog_dimmer(void)
{
    return s_show_new_project
        || s_show_open_project
        || s_show_new_scene
        || s_show_open_scene
        || s_show_save_as
        || s_show_unsaved
    || jce_editor_assets_delete_dialog_open()
    || jce_editor_inspector_delete_dialog_open();
}

static bool should_block_editor_interaction(void)
{
    return s_show_about || s_show_preferences || s_show_proj_settings
        || s_show_build || should_draw_dialog_dimmer();
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
    jce_toast_success("Saved scene: %s", scene_path);
    return SAVE_SCENE_RESULT_OK;
}

static void handle_global_edit_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    /* Fullscreen via central registry (default F11). */
    if (jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW)) {
        jce_editor_toggle_fullscreen();
        return;
    }

    /* Avoid stealing shortcuts while typing in text fields. */
    if (io.WantTextInput)
        return;

    /* Modal dialogs must block background state changes. */
    if (should_block_editor_interaction())
        return;

    if (jce_hotkey_pressed(JCE_HK_EDIT_UNDO)) {
        if (jce_state_can_undo())
            jce_state_undo();
        return;
    }
    if (jce_hotkey_pressed(JCE_HK_EDIT_REDO)) {
        if (jce_state_can_redo())
            jce_state_redo();
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
    };
    for (const auto &t : toggles) {
        if (jce_hotkey_pressed(t.hk)) {
            bool *v = jce_editor_panel_visible_ptr(t.panel);
            if (v) *v = !*v;
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        cmd_toggle_demo_lod_();
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
static void cmd_new_scene_(void)         { s_show_new_scene = true; }
static void cmd_open_scene_(void)        { s_show_open_scene = true; }
static void cmd_save_scene_(void)        { save_scene_or_open_save_as(); }
static void cmd_save_scene_as_(void)     { s_show_save_as = true; }
static void cmd_new_project_(void)       { s_show_new_project = true; }
static void cmd_open_project_(void)      { s_show_open_project = true; }
static void cmd_build_settings_(void)    { s_show_build = true; }
static void cmd_settings_(void)          { s_show_preferences = true; }
static void cmd_proj_settings_(void)     { s_show_proj_settings = true; }
static void cmd_about_(void)             { s_show_about = true; }
static void cmd_quit_(void)              { jce_editor_layout_request_quit(); }
static void cmd_toggle_fullscreen_(void) { jce_editor_toggle_fullscreen(); }

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
static void cmd_show_material_graph_(void)    { cmd_toggle_panel_(JCE_PANEL_MATERIAL_GRAPH); }
static void cmd_show_animation_editor_(void)  { cmd_toggle_panel_(JCE_PANEL_ANIMATION_EDITOR); }
static void cmd_show_animator_sm_(void)       { cmd_toggle_panel_(JCE_PANEL_ANIMATOR_SM); }
static void cmd_show_sequencer_(void)         { cmd_toggle_panel_(JCE_PANEL_SEQUENCER); }
static void cmd_show_navmesh_(void)           { cmd_toggle_panel_(JCE_PANEL_NAVMESH); }
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
    { "file.exit",           "Exit Editor",                "File",      cmd_quit_ },
    /* Edit */
    { "edit.undo",           "Undo",                        "Edit",      cmd_undo_ },
    { "edit.redo",           "Redo",                        "Edit",      cmd_redo_ },
    { "edit.settings",       "Editor Settings\u2026",       "Edit",      cmd_settings_ },
    { "edit.project_settings","Project Settings\u2026",     "Edit",      cmd_proj_settings_ },
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
    { "window.material_graph","Toggle Window: Material Graph","Window",  cmd_show_material_graph_ },
    { "window.animation",    "Toggle Window: Animation Editor","Window", cmd_show_animation_editor_ },
    { "window.animator_sm",  "Toggle Window: Animator SM",  "Window",    cmd_show_animator_sm_ },
    { "window.sequencer",    "Toggle Window: Sequencer",    "Window",    cmd_show_sequencer_ },
    { "window.navmesh",      "Toggle Window: NavMesh",      "Window",    cmd_show_navmesh_ },
    { "window.terrain",      "Toggle Window: Terrain",      "Window",    cmd_show_terrain_ },
    { "window.lightmap",     "Toggle Window: Lightmap Bake","Window",    cmd_show_lightmap_ },
    { "window.curve",        "Toggle Window: Curve Editor", "Window",    cmd_show_curve_ },
    { "window.particle",     "Toggle Window: Particle Editor","Window",  cmd_show_particle_ },
    { "window.import_presets","Toggle Window: Import Presets","Window",  cmd_show_import_presets_ },
    /* Help */
    { "help.about",          "About JCE Editor",            "Help",      cmd_about_ },
};
enum { PALETTE_CMD_COUNT = (int)(sizeof s_palette_cmds / sizeof s_palette_cmds[0]) };

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
        if (palette_match(c.label, s_palette_query)
            || palette_match(c.id, s_palette_query)
            || palette_match(c.category, s_palette_query))
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
            snprintf(buf, sizeof buf, "%-32s  [%s]", c.label, c.category);
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

static void cmd_toggle_demo_lod_(void)
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

static void draw_menu_bar(void)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    if (!ImGui::BeginMenuBar()) {
        ImGui::PopStyleVar();
        return;
    }

    /* Match requested larger row spacing and overall bar height. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(18, 8));

    /* Disable entire menu bar when a modal dialog is open. */
    bool dialog_active = should_block_editor_interaction();
    if (dialog_active) ImGui::BeginDisabled(true);

    /* ── File ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.file"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.newScene"),    "Ctrl+N"))
            s_show_new_scene = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.openScene"),   "Ctrl+O"))
            s_show_open_scene = true;
        /* Recent Scenes submenu — fed from JceEditorConfig.recent_scene_paths.
         * Missing files are grayed out (with a (missing) suffix) and clicking
         * them removes them from the list. */
        {
            JceEditorConfig _ecfg;
            (void)jce_editor_config_load(&_ecfg);
            bool has_any = (_ecfg.recent_scene_count > 0);
            if (ImGui::BeginMenu(jce_editor_i18n("menu.file.openRecentScene"), has_any)) {
                int remove_idx = -1;
                for (int i = 0; i < _ecfg.recent_scene_count; i++) {
                    const char *p = _ecfg.recent_scene_paths[i];
                    if (!p || !p[0]) continue;
                    bool exists = jce_fs_host_exists_file(p);
                    char label[600];
                    if (exists) {
                        snprintf(label, sizeof(label), "%s", p);
                    } else {
                        snprintf(label, sizeof(label), "%s %s",
                                 p, jce_editor_i18n("menu.file.recentMissing"));
                    }
                    if (!exists) ImGui::BeginDisabled(true);
                    if (ImGui::MenuItem(label)) {
                        if (!jce_state_load_scene_file(p)) {
                            remove_idx = i;
                        }
                    }
                    if (!exists) {
                        ImGui::EndDisabled();
                        /* Right-click to purge missing entries. */
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            remove_idx = i;
                    }
                }
                if (remove_idx >= 0) {
                    for (int j = remove_idx; j < _ecfg.recent_scene_count - 1; j++) {
                        strncpy(_ecfg.recent_scene_paths[j],
                                _ecfg.recent_scene_paths[j + 1],
                                sizeof(_ecfg.recent_scene_paths[j]) - 1);
                        _ecfg.recent_scene_paths[j][sizeof(_ecfg.recent_scene_paths[j]) - 1] = '\0';
                    }
                    _ecfg.recent_scene_count--;
                    if (_ecfg.recent_scene_count >= 0 && _ecfg.recent_scene_count < 10)
                        _ecfg.recent_scene_paths[_ecfg.recent_scene_count][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(jce_editor_i18n("menu.file.clearRecent"))) {
                    _ecfg.recent_scene_count = 0;
                    for (int i = 0; i < 10; i++) _ecfg.recent_scene_paths[i][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::EndMenu();
            }
            /* Recent Projects submenu — same shape, but switching projects
             * goes through the Open Project dialog which performs the
             * restart-required checks. We pre-fill the path field. */
            bool has_any_proj = (_ecfg.recent_count > 0);
            if (ImGui::BeginMenu(jce_editor_i18n("menu.file.openRecentProject"), has_any_proj)) {
                int remove_idx = -1;
                for (int i = 0; i < _ecfg.recent_count; i++) {
                    const char *p = _ecfg.recent_projects[i];
                    if (!p || !p[0]) continue;
                    bool exists = jce_fs_host_exists_dir(p);
                    char label[600];
                    if (exists) {
                        snprintf(label, sizeof(label), "%s", p);
                    } else {
                        snprintf(label, sizeof(label), "%s %s",
                                 p, jce_editor_i18n("menu.file.recentMissing"));
                    }
                    if (!exists) ImGui::BeginDisabled(true);
                    if (ImGui::MenuItem(label)) {
                        jce_editor_dialog_open_project_set_path(p);
                        s_show_open_project = true;
                    }
                    if (!exists) {
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                            remove_idx = i;
                    }
                }
                if (remove_idx >= 0) {
                    for (int j = remove_idx; j < _ecfg.recent_count - 1; j++) {
                        strncpy(_ecfg.recent_projects[j],
                                _ecfg.recent_projects[j + 1],
                                sizeof(_ecfg.recent_projects[j]) - 1);
                        _ecfg.recent_projects[j][sizeof(_ecfg.recent_projects[j]) - 1] = '\0';
                    }
                    _ecfg.recent_count--;
                    if (_ecfg.recent_count >= 0 && _ecfg.recent_count < 10)
                        _ecfg.recent_projects[_ecfg.recent_count][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(jce_editor_i18n("menu.file.clearRecent"))) {
                    _ecfg.recent_count = 0;
                    for (int i = 0; i < 10; i++) _ecfg.recent_projects[i][0] = '\0';
                    jce_editor_config_save(&_ecfg);
                }
                ImGui::EndMenu();
            }
        }
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
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.buildSettings"), "Ctrl+B"))
            s_show_build = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.exit"), "Alt+F4"))
            jce_editor_layout_request_quit();
        ImGui::EndMenu();
    }

    /* ── Edit ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.edit"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.undo"),  "Ctrl+Z", false, jce_state_can_undo()))
            jce_state_undo();
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.redo"),  "Ctrl+Y", false, jce_state_can_redo()))
            jce_state_redo();
        ImGui::Separator();
        {
            int sel_count = 0;
            const uint32_t *sel = jce_state_get_selection(&sel_count);
            uint32_t focused = jce_state_get_focused();
            bool has_target = (sel_count > 0) || (focused != 0);
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"),  "Ctrl+C", false, has_target)) {
                if (sel_count > 0)        jce_state_copy_entities(sel, sel_count, false);
                else if (focused)         jce_state_copy_entity(focused);
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.cut"),   "Ctrl+X", false, has_target)) {
                if (sel_count > 0)        jce_state_copy_entities(sel, sel_count, true);
                else if (focused)         jce_state_copy_entities(&focused, 1, true);
            }
            bool can_paste = jce_state_clipboard_count() > 0 || jce_state_has_copied();
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false, can_paste)) {
                uint32_t parent = focused; /* paste under focused if any, else root */
                if (jce_state_clipboard_count() > 0) {
                    uint32_t new_ids[64];
                    int n = jce_state_paste_entities(parent, new_ids, 64);
                    if (n > 0) jce_state_select_entity(new_ids[0], false);
                    for (int i = 1; i < n; ++i) jce_state_select_entity(new_ids[i], true);
                } else if (jce_state_has_copied()) {
                    uint32_t nid = jce_state_paste_entity(parent);
                    if (nid) jce_state_select_entity(nid, false);
                }
            }
        }
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
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.preferences"))) {
            s_show_preferences = true;
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.edit.projectSettings")))
            s_show_proj_settings = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("shaders.reload"), "F5")) {
            jce_editor_reload_shaders();
        }
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
        /* Core */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.core"))) {
            ImGui::MenuItem(jce_editor_i18n("Hierarchy"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY));
            ImGui::MenuItem(jce_editor_i18n("Inspector"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR));
            ImGui::MenuItem(jce_editor_i18n("Console"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE));
            ImGui::MenuItem(jce_editor_i18n("Asset Browser"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS));
            ImGui::MenuItem(jce_editor_i18n("File Viewer"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER));
            ImGui::MenuItem(jce_editor_i18n("window.profiler"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER));
            ImGui::EndMenu();
        }
        /* Scene */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.scene"))) {
            ImGui::MenuItem(jce_editor_i18n("Scene"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW));
            ImGui::MenuItem(jce_editor_i18n("Game"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW));
            ImGui::MenuItem(jce_editor_i18n("Timeline"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE));
            ImGui::EndMenu();
        }
        /* Animation */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.animation"))) {
            ImGui::MenuItem(jce_editor_i18n("window.animationEditor"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR));
            ImGui::MenuItem(jce_editor_i18n("window.animatorSM"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATOR_SM));
            ImGui::MenuItem(jce_editor_i18n("window.sequencer"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_SEQUENCER));
            ImGui::MenuItem(jce_editor_i18n("window.curveEditor"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_CURVE_EDITOR));
            ImGui::EndMenu();
        }
        /* Rendering */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.rendering"))) {
            ImGui::MenuItem(jce_editor_i18n("lighting.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING));
            ImGui::MenuItem(jce_editor_i18n("postfx.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_POSTFX));
            ImGui::MenuItem(jce_editor_i18n("window.lightmapBake"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTMAP_BAKE));
            ImGui::MenuItem(jce_editor_i18n("window.materialGraph"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH));
            ImGui::MenuItem(jce_editor_i18n("window.particleEditor"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_PARTICLE_EDITOR));
            ImGui::EndMenu();
        }
        /* World */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.world"))) {
            ImGui::MenuItem(jce_editor_i18n("window.terrain"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN));
            ImGui::MenuItem(jce_editor_i18n("window.navmesh"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_NAVMESH));
            ImGui::EndMenu();
        }
        /* Tools */
        if (ImGui::BeginMenu(jce_editor_i18n("window.group.tools"))) {
            ImGui::MenuItem(jce_editor_i18n("window.importPresets"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_IMPORT_PRESETS));
            ImGui::MenuItem(jce_editor_i18n("audioMixer.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER));
            ImGui::MenuItem(jce_editor_i18n("inputManager.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_INPUT_MANAGER));
            ImGui::MenuItem(jce_editor_i18n("packageManager.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_PACKAGE_MANAGER));
            ImGui::MenuItem(jce_editor_i18n("frameDebugger.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER));
            ImGui::MenuItem(jce_editor_i18n("spriteEditor.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR));
            ImGui::MenuItem(jce_editor_i18n("tilePalette.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE));
            ImGui::MenuItem(jce_editor_i18n("vfxGraph.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH));
            ImGui::MenuItem(jce_editor_i18n("testRunner.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER));
            ImGui::MenuItem(jce_editor_i18n("buildProfiles.title"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES));
            ImGui::Separator();
            ImGui::MenuItem(jce_editor_i18n("window.memoryProfiler"),   NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER));
            ImGui::MenuItem(jce_editor_i18n("window.physicsDebugger"),  NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER));
            ImGui::MenuItem(jce_editor_i18n("window.lightExplorer"),    NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER));
            ImGui::MenuItem(jce_editor_i18n("window.reflectionProbes"), NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_REFLECTION_PROBES));
            ImGui::MenuItem(jce_editor_i18n("window.shaderGraph"),      NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_GRAPH));
            ImGui::MenuItem(jce_editor_i18n("window.search"),            "Ctrl+K",
                            jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH));
            ImGui::MenuItem(jce_editor_i18n("window.versionControl"),   NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL));
            ImGui::Separator();
            ImGui::MenuItem(jce_editor_i18n("window.timeOfDay"),       NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY));
            ImGui::MenuItem(jce_editor_i18n("window.vcamManager"),      NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER));
            ImGui::MenuItem(jce_editor_i18n("window.reverbZones"),      NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES));
            ImGui::MenuItem(jce_editor_i18n("window.saveBrowser"),      NULL,
                            jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER));
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::MenuItem(jce_editor_i18n("window.toolbar"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_TOOLBAR));
        ImGui::MenuItem(jce_editor_i18n("window.statusBar"), NULL,
                        jce_editor_panel_visible_ptr(JCE_PANEL_STATUS_BAR));
        ImGui::Separator();
        if (ImGui::BeginMenu(jce_editor_i18n("window.layoutPresets"))) {
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.default")))   { s_layout_preset_pending = 0; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.wide")))      { s_layout_preset_pending = 1; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.animation"))) { s_layout_preset_pending = 2; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.twoByTwo"))){ s_layout_preset_pending = 3; s_reset_layout_requested = true; }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.programmer"))){ s_layout_preset_pending = 4; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.twoD")))      { s_layout_preset_pending = 5; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.mobilePortrait"))){ s_layout_preset_pending = 6; s_reset_layout_requested = true; }
            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.cinematic")))  { s_layout_preset_pending = 7; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.profiling")))  { s_layout_preset_pending = 8; s_reset_layout_requested = true; }
            if (ImGui::MenuItem(jce_editor_i18n("window.layout.lighting")))   { s_layout_preset_pending = 9; s_reset_layout_requested = true; }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(jce_editor_i18n("menu.window.resetLayout"))) {
            s_layout_preset_pending = 0;
            s_reset_layout_requested = true;
        }
        ImGui::EndMenu();
    }

    /* ── Help ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.debug"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.debug.toggleDemoLod"),
                            "F9", s_demo_lod_enabled)) {
            cmd_toggle_demo_lod_();
        }
        ImGui::EndMenu();
    }

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

        /* Use the theme's Button color when stopped so light themes don't
           render a near-black button on a white menu bar. */
        ImVec4 stopped_btn = ImGui::GetStyleColorVec4(ImGuiCol_Button);
        ImGui::PushStyleColor(ImGuiCol_Button,
            ps == JCE_PLAY_PLAYING ? ImVec4(0.2f, 0.6f, 0.2f, 1.0f)
                                   : stopped_btn);
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

    if (dialog_active) ImGui::EndDisabled();

    ImGui::PopStyleVar(2); /* ItemSpacing + FramePadding */
    ImGui::EndMenuBar();
}

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
    /* Right column (alongside Inspector): scene-wide settings panes. */
    ImGui::DockBuilderDockWindow("###lighting",         right_id);
    ImGui::DockBuilderDockWindow("###tile_palette",     right_id);

    /* Bottom strip (alongside Console / Asset Browser). */
    ImGui::DockBuilderDockWindow("###audio_mixer",      bottom_id);
    ImGui::DockBuilderDockWindow("###input_manager",    bottom_id);
    ImGui::DockBuilderDockWindow("###package_manager",  bottom_id);
    ImGui::DockBuilderDockWindow("###frame_debugger",   bottom_id);
    ImGui::DockBuilderDockWindow("###test_runner",      bottom_id);
    ImGui::DockBuilderDockWindow("###build_profiles",   bottom_id);

    /* Center (alongside Scene/Game): authoring graphs / canvases. */
    ImGui::DockBuilderDockWindow("###vfx_graph",        center_id);
    ImGui::DockBuilderDockWindow("###sprite_editor",    center_id);

    /* ── Legacy panels (Sprint-0/1) — also routed here so newer presets
       inherit the same coverage as the original Default/Wide/Animation. */
    ImGui::DockBuilderDockWindow("###jce_profiler",          right_id);
    ImGui::DockBuilderDockWindow("###postfx",                right_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",           right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",           right_id);
    ImGui::DockBuilderDockWindow("###jce_lightmap_bake",     right_id);

    ImGui::DockBuilderDockWindow("###jce_material_graph",    center_id);
    ImGui::DockBuilderDockWindow("###jce_particle_editor",   center_id);
    ImGui::DockBuilderDockWindow("###jce_anim_sm",           center_id);

    ImGui::DockBuilderDockWindow("###jce_anim_editor",       bottom_id);
    ImGui::DockBuilderDockWindow("###jce_curve_editor",      bottom_id);
    ImGui::DockBuilderDockWindow("###jce_seq",               bottom_id);
    ImGui::DockBuilderDockWindow("###jce_import_presets",    bottom_id);
    ImGui::DockBuilderDockWindow("Animation Editor###animation_editor", bottom_id);
    ImGui::DockBuilderDockWindow("Curve Editor###curve_editor",         bottom_id);

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
    ImGui::DockBuilderDockWindow("###jce_profiler",            bottom_id);
    ImGui::DockBuilderDockWindow("###jce_import_presets",      bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",         bottom_id);
    ImGui::DockBuilderDockWindow("###jce_curve_editor",        bottom_id);
    ImGui::DockBuilderDockWindow("###jce_seq",                 bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_sm",             center_id);
    ImGui::DockBuilderDockWindow("###jce_material_graph",      center_id);
    ImGui::DockBuilderDockWindow("###jce_particle_editor",     center_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",             right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",             right_id);
    ImGui::DockBuilderDockWindow("###jce_lightmap_bake",       right_id);
    ImGui::DockBuilderDockWindow("###postfx",                  right_id);

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
    ImGui::DockBuilderDockWindow("###jce_profiler",           right_id);
    ImGui::DockBuilderDockWindow("###postfx",                 right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",     right_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",       bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",         bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",    bottom_id);
    ImGui::DockBuilderDockWindow("Animation Editor###animation_editor", bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",        bottom_id);
    ImGui::DockBuilderDockWindow("Curve Editor###curve_editor", bottom_id);
    ImGui::DockBuilderDockWindow("###jce_curve_editor",       bottom_id);
    ImGui::DockBuilderDockWindow("###jce_seq",                bottom_id);
    /* Specialized panels: dock to sensible target so they appear in the
       expected zone the moment the user enables them via Window menu. */
    ImGui::DockBuilderDockWindow("###jce_import_presets",     bottom_id);
    ImGui::DockBuilderDockWindow("###jce_material_graph",     center2);
    ImGui::DockBuilderDockWindow("###jce_particle_editor",    center2);
    ImGui::DockBuilderDockWindow("###jce_anim_sm",            center2);
    ImGui::DockBuilderDockWindow("###jce_navmesh",            right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",            right_id);
    ImGui::DockBuilderDockWindow("###jce_lightmap_bake",      right_id);
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
    ImGui::DockBuilderDockWindow("###jce_profiler",             right_id);
    ImGui::DockBuilderDockWindow("###postfx",                   right_id);
    ImGui::DockBuilderDockWindow("Animation Editor###animation_editor", bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",          bottom_id);
    ImGui::DockBuilderDockWindow("Curve Editor###curve_editor", bottom_id);
    ImGui::DockBuilderDockWindow("###jce_curve_editor",         bottom_id);
    ImGui::DockBuilderDockWindow("###jce_anim_sm",              bottom_id);
    ImGui::DockBuilderDockWindow("###jce_seq",                  bottom_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",         bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",           bottom_id);
    ImGui::DockBuilderDockWindow("Asset Browser###assets",      bottom_id);
    /* Specialized panels go to the bottom by default in this preset. */
    ImGui::DockBuilderDockWindow("###jce_import_presets",       bottom_id);
    ImGui::DockBuilderDockWindow("###jce_material_graph",       center_id);
    ImGui::DockBuilderDockWindow("###jce_particle_editor",      center_id);
    ImGui::DockBuilderDockWindow("###jce_navmesh",              right_id);
    ImGui::DockBuilderDockWindow("###jce_terrain",              right_id);
    ImGui::DockBuilderDockWindow("###jce_lightmap_bake",        right_id);
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
    ImGui::DockBuilderDockWindow("Animation Editor###animation_editor", br);
    ImGui::DockBuilderDockWindow("###jce_anim_editor",        br);
    ImGui::DockBuilderDockWindow("Curve Editor###curve_editor", br);
    ImGui::DockBuilderDockWindow("###jce_curve_editor",       br);
    ImGui::DockBuilderDockWindow("###jce_seq",                br);
    /* Specialized panels: route to right column / bottom-left when shown. */
    ImGui::DockBuilderDockWindow("###jce_profiler",           br);
    ImGui::DockBuilderDockWindow("###postfx",                 br);
    ImGui::DockBuilderDockWindow("###jce_import_presets",     bl);
    ImGui::DockBuilderDockWindow("###jce_material_graph",     tl);
    ImGui::DockBuilderDockWindow("###jce_particle_editor",    tl);
    ImGui::DockBuilderDockWindow("###jce_anim_sm",            br);
    ImGui::DockBuilderDockWindow("###jce_navmesh",            br);
    ImGui::DockBuilderDockWindow("###jce_terrain",            br);
    ImGui::DockBuilderDockWindow("###jce_lightmap_bake",      br);
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
    ImGui::DockBuilderDockWindow("###jce_profiler",             right_id);
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
    ImGui::DockBuilderDockWindow("###sprite_editor",            center_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",          center_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",       right_id);
    ImGui::DockBuilderDockWindow("File Viewer###file_viewer",   right_id);
    ImGui::DockBuilderDockWindow("###tile_palette",             right_id);
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
    ImGui::DockBuilderDockWindow("###jce_profiler",             right_id);
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
    ImGui::DockBuilderDockWindow("Scene###scene_view",         center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",           center_id);
    ImGui::DockBuilderDockWindow("VCam Manager###vcam_manager",right_id);
    ImGui::DockBuilderDockWindow("Time of Day###time_of_day",  right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",      right_id);
    ImGui::DockBuilderDockWindow("Timeline###timeline",        bottom_id);
    ImGui::DockBuilderDockWindow("###curve_editor",            bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",          bottom_id);
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

    ImGui::DockBuilderDockWindow("###jce_profiler",                  left_id);
    ImGui::DockBuilderDockWindow("Memory Profiler###memory_profiler",left_id);
    ImGui::DockBuilderDockWindow("Physics Debugger###physics_debugger",right_id);
    ImGui::DockBuilderDockWindow("Game###game_view",                 right_id);
    ImGui::DockBuilderDockWindow("Console###console",                bottom_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",               bottom_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",            bottom_id);
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

    ImGui::DockBuilderDockWindow("Light Explorer###light_explorer",      left_id);
    ImGui::DockBuilderDockWindow("Hierarchy###hierarchy",                left_id);
    ImGui::DockBuilderDockWindow("Scene###scene_view",                   center_id);
    ImGui::DockBuilderDockWindow("Game###game_view",                     center_id);
    ImGui::DockBuilderDockWindow("###lighting",                          right_id);
    ImGui::DockBuilderDockWindow("Time of Day###time_of_day",            right_id);
    ImGui::DockBuilderDockWindow("Inspector###inspector",                right_id);
    ImGui::DockBuilderDockWindow("Reflection Probes###reflection_probes",bottom_id);
    ImGui::DockBuilderDockWindow("Console###console",                    bottom_id);
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

    /* ── Lighting ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING)) {
        snprintf(lbl, sizeof(lbl), "%s###lighting", jce_editor_i18n("lighting.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING))) {
            jce_editor_panel_lighting_content();
        }
        ImGui::End();
    }

    /* ── Audio Mixer ──────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER)) {
        snprintf(lbl, sizeof(lbl), "%s###audio_mixer",
                 jce_editor_i18n("audioMixer.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_AUDIO_MIXER))) {
            jce_editor_panel_audio_mixer_content();
        }
        ImGui::End();
    }

    /* ── Input Manager ────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_INPUT_MANAGER)) {
        snprintf(lbl, sizeof(lbl), "%s###input_manager",
                 jce_editor_i18n("inputManager.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_INPUT_MANAGER))) {
            jce_editor_panel_input_manager_content();
        }
        ImGui::End();
    }

    /* ── Package Manager ──────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PACKAGE_MANAGER)) {
        snprintf(lbl, sizeof(lbl), "%s###package_manager",
                 jce_editor_i18n("packageManager.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_PACKAGE_MANAGER))) {
            jce_editor_panel_package_manager_content();
        }
        ImGui::End();
    }

    /* ── Frame Debugger (Sprint 3 #10) ────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER)) {
        snprintf(lbl, sizeof(lbl), "%s###frame_debugger",
                 jce_editor_i18n("frameDebugger.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER))) {
            jce_editor_panel_frame_debugger_content();
        }
        ImGui::End();
    }

    /* ── Sprite Editor (Sprint 3 #12) ─────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR)) {
        snprintf(lbl, sizeof(lbl), "%s###sprite_editor",
                 jce_editor_i18n("spriteEditor.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SPRITE_EDITOR))) {
            jce_editor_panel_sprite_editor_content();
        }
        ImGui::End();
    }

    /* ── Tile Palette (Sprint 3 #12) ──────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE)) {
        snprintf(lbl, sizeof(lbl), "%s###tile_palette",
                 jce_editor_i18n("tilePalette.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TILE_PALETTE))) {
            jce_editor_panel_tile_palette_content();
        }
        ImGui::End();
    }

    /* ── VFX Graph (Sprint 3 #13) ─────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH)) {
        snprintf(lbl, sizeof(lbl), "%s###vfx_graph",
                 jce_editor_i18n("vfxGraph.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_VFX_GRAPH))) {
            jce_editor_panel_vfx_graph_content();
        }
        ImGui::End();
    }

    /* ── Test Runner (Sprint 3 #14) ───────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER)) {
        snprintf(lbl, sizeof(lbl), "%s###test_runner",
                 jce_editor_i18n("testRunner.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TEST_RUNNER))) {
            jce_editor_panel_test_runner_content();
        }
        ImGui::End();
    }

    /* ── Build Profiles (Sprint 3 #14) ────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES)) {
        snprintf(lbl, sizeof(lbl), "%s###build_profiles",
                 jce_editor_i18n("buildProfiles.title"));
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES))) {
            jce_editor_panel_build_profiles_content();
        }
        ImGui::End();
    }

    /* ── P3 panels (Memory Profiler / Physics Debugger / Light Explorer /
     * Reflection Probes / Shader Graph / Search / Version Control) ── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER)) {
        snprintf(lbl, sizeof(lbl), "Memory Profiler###memory_profiler");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER)))
            jce_editor_panel_memory_profiler_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER)) {
        snprintf(lbl, sizeof(lbl), "Physics Debugger###physics_debugger");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_DEBUGGER)))
            jce_editor_panel_physics_debugger_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER)) {
        snprintf(lbl, sizeof(lbl), "Light Explorer###light_explorer");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER)))
            jce_editor_panel_light_explorer_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_REFLECTION_PROBES)) {
        snprintf(lbl, sizeof(lbl), "Reflection Probes###reflection_probes");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_REFLECTION_PROBES)))
            jce_editor_panel_reflection_probes_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_GRAPH)) {
        snprintf(lbl, sizeof(lbl), "Shader Graph###shader_graph");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SHADER_GRAPH)))
            jce_editor_panel_shader_graph_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH)) {
        snprintf(lbl, sizeof(lbl), "Search###search");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH)))
            jce_editor_panel_search_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL)) {
        snprintf(lbl, sizeof(lbl), "Version Control###version_control");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL)))
            jce_editor_panel_version_control_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY)) {
        snprintf(lbl, sizeof(lbl), "Time of Day###time_of_day");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY)))
            jce_editor_panel_time_of_day_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER)) {
        snprintf(lbl, sizeof(lbl), "VCam Manager###vcam_manager");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER)))
            jce_editor_panel_vcam_manager_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES)) {
        snprintf(lbl, sizeof(lbl), "Reverb Zones###reverb_zones");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES)))
            jce_editor_panel_reverb_zones_content();
        ImGui::End();
    }
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER)) {
        snprintf(lbl, sizeof(lbl), "Save Browser###save_browser");
        if (ImGui::Begin(lbl, jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER)))
            jce_editor_panel_save_browser_content();
        ImGui::End();
    }

    /* ── Top-level Toolbar (P0) ───────────────────────────────────── */
    jce_editor_panel_toolbar();
    /* ── Bottom Status Bar (P0) ───────────────────────────────────── */
    jce_editor_panel_status_bar();

    /* ── Profiler ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER)) {
        char title[64];
        snprintf(title, sizeof(title), "%s###profiler", jce_editor_i18n("panel.profiler"));
        if (ImGui::Begin(title,
                         jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER)))
        {
            jce_editor_panel_profiler_content();
        }
        ImGui::End();
    }

    /* ── Particle Editor ──────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_PARTICLE_EDITOR)) {
        jce_editor_panel_particle_editor();
    }

    /* ── Material Graph ──────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH)) {
        jce_editor_panel_material_graph();
    }

    /* ── Import Presets ──────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_IMPORT_PRESETS)) {
        jce_editor_panel_import_presets();
    }

    /* ── Lightmap Bake ───────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTMAP_BAKE)) {
        jce_editor_panel_lightmap_bake();
    }

    /* ── Curve Editor ────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_CURVE_EDITOR)) {
        jce_editor_panel_curve_editor();
    }

    /* ── Animation Editor ────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATION_EDITOR)) {
        jce_editor_panel_animation_editor();
    }

    /* ── Animator State Machine ──────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_ANIMATOR_SM)) {
        jce_editor_panel_animator_sm();
    }

    /* ── Sequencer ───────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_SEQUENCER)) {
        jce_editor_panel_sequencer();
    }

    /* ── NavMesh ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_NAVMESH)) {
        jce_editor_panel_navmesh();
    }

    /* ── Terrain ─────────────────────────────────────────────────── */
    if (*jce_editor_panel_visible_ptr(JCE_PANEL_TERRAIN)) {
        jce_editor_panel_terrain();
    }
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
    const char *slash = strrchr(scene_name, '/');
    const char *bslash = strrchr(scene_name, '\\');
    if (bslash && bslash > slash) slash = bslash;
    if (slash) scene_name = slash + 1;

    ImGui::Text("%s %s", jce_editor_i18n("statusBar.scene"), scene_name);
    if (jce_state_is_scene_modified()) {
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.65f, 0.10f, 1.0f));
        ImGui::TextUnformatted(jce_editor_i18n("statusBar.unsavedDot"));
        ImGui::PopStyleColor();
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
       reduces viewport->WorkSize and the DockSpace adapts. */
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

    handle_global_edit_shortcuts();

    /* Menu bar */
    draw_menu_bar();

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
                && p != JCE_PANEL_SEARCH
                && p != JCE_PANEL_VERSION_CONTROL
                && p != JCE_PANEL_TIME_OF_DAY
                && p != JCE_PANEL_VCAM_MANAGER
                && p != JCE_PANEL_REVERB_ZONES
                && p != JCE_PANEL_SAVE_BROWSER)
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
        *jce_editor_panel_visible_ptr(JCE_PANEL_SEARCH)           = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_VERSION_CONTROL)  = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY)      = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_VCAM_MANAGER)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_REVERB_ZONES)     = false;
        *jce_editor_panel_visible_ptr(JCE_PANEL_SAVE_BROWSER)     = false;

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
    draw_panel_windows();

    /* Preferences (floating). */
    jce_editor_panel_preferences();

    if (should_draw_dialog_dimmer())
        draw_dialog_dimmer();

    /* Dialogs */
    jce_editor_about_dialog(&s_show_about);
    jce_editor_inspector_delete_dialog();
    jce_editor_dialog_new_project(&s_show_new_project);
    jce_editor_dialog_open_project(&s_show_open_project);
    jce_editor_dialog_new_scene(&s_show_new_scene);
    jce_editor_dialog_open_scene(&s_show_open_scene);
    jce_editor_dialog_save_as(&s_show_save_as);
    jce_editor_dialog_unsaved_changes(&s_show_unsaved, &s_unsaved_result);
    jce_editor_dialog_build_settings(&s_show_build);
    jce_editor_dialog_project_settings(&s_show_proj_settings);
    jce_editor_dialog_preferences(&s_show_preferences);
    draw_command_palette();

    /* Toast overlay — draw last so it renders on top of everything. */
    jce_editor_toast_draw();

    if (!s_show_unsaved && s_unsaved_result != 0) {
        if (s_unsaved_result == 1) {
            SaveSceneResult save_result = save_scene_or_open_save_as();
            if (save_result == SAVE_SCENE_RESULT_OK)
                s_quit_confirmed = true;
            else if (save_result == SAVE_SCENE_RESULT_NEEDS_PATH)
                s_quit_after_save_as = true;
        } else if (s_unsaved_result == 2) {
            s_quit_confirmed = true;
        }
        s_unsaved_result = 0;
    }

    if (s_quit_after_save_as && !s_show_save_as) {
        const char *scene_path = jce_state_get_current_scene_path();
        if (scene_path && scene_path[0] != '\0')
            s_quit_confirmed = true;
        s_quit_after_save_as = false;
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

    s_unsaved_result = 0;
    s_show_unsaved   = true;
}

bool jce_editor_layout_is_quit_confirmed(void)
{
    return s_quit_confirmed;
}
