/*
 * jce_editor_layout.cpp  Fixed-region layout with tab groups.
 *
 * Simulates docking without ImGui DockBuilder (not available in 1.92.5).
 * Uses SetNextWindowPos/Size with fixed ratios to create 4 regions:
 *   Left   (15%) — Hierarchy
 *   Center (60%) — Scene View / Game View (tab group)
 *   Right  (25%) — Inspector / File Viewer (tab group)
 *   Bottom (25%) — Console / Timeline / Asset Browser (tab group)
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
#include <stdio.h>

/* ── Dialog state ─────────────────────────────────────────────────── */

static bool s_show_about       = false;
static bool s_show_settings    = false;
static bool s_show_new_project = false;
static bool s_show_open_project = false;
static bool s_show_save_as     = false;
static bool s_show_unsaved     = false;
static int  s_unsaved_result   = 0;

/* ── Region flags (shared by all fixed panels) ────────────────────── */

static const ImGuiWindowFlags kRegionFlags =
    ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoBringToFrontOnFocus;

/* ══════════════════════════════════════════════════════════════════════
 *  MENU BAR
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_menu_bar(void)
{
    if (!ImGui::BeginMenuBar()) return;

    /* ── File ──────────────────────────────────────────────────────── */
    if (ImGui::BeginMenu(jce_editor_i18n("menu.file"))) {
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.newScene"),    "Ctrl+N"))  { /* TODO */ }
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.openScene"),   "Ctrl+O"))  { /* TODO */ }
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveScene"),   "Ctrl+S"))  { /* TODO */ }
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.saveAs"),   "Ctrl+Shift+S"))
            s_show_save_as = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.new")))
            s_show_new_project = true;
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.open")))
            s_show_open_project = true;
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("menu.file.exit"), "Alt+F4"))
            s_show_unsaved = true;
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
            if (f) jce_state_delete_entity(f);
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
        float avail = ImGui::GetContentRegionAvail().x;
        float btn_w = 80.0f; /* approx width of play + stop buttons */
        ImGui::SameLine(ImGui::GetCursorPosX() + avail - btn_w);

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

    ImGui::EndMenuBar();
}

/* ══════════════════════════════════════════════════════════════════════
 *  FIXED LAYOUT REGIONS
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_region_borders(float x0, float y0, float total_w, float total_h,
                                float left_w, float top_h, float center_w, float pad)
{
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImU32 border_col = IM_COL32(45, 45, 55, 255); /* JCE_COLOR_BG_HEADER */

    /* Vertical: left | center */
    float vx1 = x0 + left_w + pad * 0.5f;
    dl->AddLine(ImVec2(vx1, y0), ImVec2(vx1, y0 + top_h), border_col, 1.0f);

    /* Vertical: center | right */
    float vx2 = x0 + left_w + pad + center_w + pad * 0.5f;
    dl->AddLine(ImVec2(vx2, y0), ImVec2(vx2, y0 + top_h), border_col, 1.0f);

    /* Horizontal: top | bottom */
    float hy = y0 + top_h + pad * 0.5f;
    dl->AddLine(ImVec2(x0, hy), ImVec2(x0 + total_w, hy), border_col, 1.0f);
}

static void draw_layout_regions(float x0, float y0, float total_w, float total_h)
{
    const float pad = 2.0f; /* gap between regions */

    /* Compute region sizes */
    float left_w   = total_w * JCE_LAYOUT_LEFT_RATIO;
    float right_w  = total_w * JCE_LAYOUT_RIGHT_RATIO;
    float center_w = total_w - left_w - right_w - pad * 2;
    float bottom_h = total_h * JCE_LAYOUT_BOTTOM_RATIO;
    float top_h    = total_h - bottom_h - pad;

    /* Draw 1px separator lines between regions */
    draw_region_borders(x0, y0, total_w, total_h, left_w, top_h, center_w, pad);

    /* ── LEFT REGION (Hierarchy) ──────────────────────────────────── */
    {
        ImGui::SetNextWindowPos(ImVec2(x0, y0));
        ImGui::SetNextWindowSize(ImVec2(left_w, top_h));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        if (ImGui::Begin("##RegionLeft", NULL, kRegionFlags)) {
            if (ImGui::BeginTabBar("LeftTabs")) {
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_hierarchy", jce_editor_i18n("Hierarchy"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_hierarchy_content();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /* ── CENTER REGION (Scene View / Game View) ───────────────────── */
    {
        float cx = x0 + left_w + pad;
        ImGui::SetNextWindowPos(ImVec2(cx, y0));
        ImGui::SetNextWindowSize(ImVec2(center_w, top_h));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("##RegionCenter", NULL, kRegionFlags)) {
            if (ImGui::BeginTabBar("CenterTabs")) {
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_scene", jce_editor_i18n("Scene"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_scene_view_content();
                        ImGui::EndTabItem();
                    }
                }
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_GAME_VIEW)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_game", jce_editor_i18n("Game"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_game_view_content();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /* ── RIGHT REGION (Inspector / File Viewer) ───────────────────── */
    {
        float rx = x0 + left_w + pad + center_w + pad;
        ImGui::SetNextWindowPos(ImVec2(rx, y0));
        ImGui::SetNextWindowSize(ImVec2(right_w, top_h));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        if (ImGui::Begin("##RegionRight", NULL, kRegionFlags)) {
            if (ImGui::BeginTabBar("RightTabs")) {
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_INSPECTOR)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_inspector", jce_editor_i18n("Inspector"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_inspector_content();
                        ImGui::EndTabItem();
                    }
                }
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_FILE_VIEWER)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_file_viewer", jce_editor_i18n("File Viewer"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_file_viewer_content();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    /* ── BOTTOM REGION (Console / Timeline / Assets) ──────────────── */
    {
        float by = y0 + top_h + pad;
        ImGui::SetNextWindowPos(ImVec2(x0, by));
        ImGui::SetNextWindowSize(ImVec2(total_w, bottom_h));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
        if (ImGui::Begin("##RegionBottom", NULL, kRegionFlags)) {
            if (ImGui::BeginTabBar("BottomTabs")) {
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_CONSOLE)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_console", jce_editor_i18n("Console"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_console_content();
                        ImGui::EndTabItem();
                    }
                }
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_TIMELINE)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_timeline", jce_editor_i18n("Timeline"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_timeline_content();
                        ImGui::EndTabItem();
                    }
                }
                if (*jce_editor_panel_visible_ptr(JCE_PANEL_ASSETS)) {
                    char _lbl[256];
                    snprintf(_lbl, sizeof(_lbl), "%s###tab_assets", jce_editor_i18n("Asset Browser"));
                    if (ImGui::BeginTabItem(_lbl)) {
                        jce_editor_panel_assets_content();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN DRAW
 * ══════════════════════════════════════════════════════════════════════ */

void jce_editor_layout_draw(void)
{
    /* Full-viewport host window for the menu bar. */
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("##EditorHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    /* Menu bar */
    draw_menu_bar();

    /* Compute available area below the menu bar */
    float menu_h = ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.y;
    float x0 = viewport->WorkPos.x;
    float y0 = viewport->WorkPos.y + menu_h;
    float total_w = viewport->WorkSize.x;
    float total_h = viewport->WorkSize.y - menu_h;

    ImGui::End(); /* ##EditorHost */

    /* Draw the 4 fixed layout regions */
    draw_layout_regions(x0, y0, total_w, total_h);

    /* Preferences (floating, temporary — will become Settings dialog) */
    jce_editor_panel_preferences();

    /* Dialogs */
    jce_editor_about_dialog(&s_show_about);
    jce_editor_settings_dialog(&s_show_settings);
    jce_editor_dialog_new_project(&s_show_new_project);
    jce_editor_dialog_open_project(&s_show_open_project);
    jce_editor_dialog_save_as(&s_show_save_as);
    jce_editor_dialog_unsaved_changes(&s_show_unsaved, &s_unsaved_result);
}
