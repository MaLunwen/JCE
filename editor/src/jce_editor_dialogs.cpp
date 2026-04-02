/*
 * jce_editor_dialogs.cpp  Project dialog implementations.
 *
 * New Project, Open Project, Save As, Unsaved Changes.
 * Each dialog is rendered as an ImGui window with i18n-translated labels.
 * Static state structs hold per-dialog input buffers and flags.
 */

#include "jce_editor_dialogs.h"
#include "jce_editor_panels.h"   /* for console_log */
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_config.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>

/* ======================================================================
 *  NEW PROJECT DIALOG
 * ====================================================================== */

static struct {
    char project_name[256];
    char project_location[512];
    int  project_type;          /* 0 = 3D, 1 = 2D */
    bool initialized;
} s_new_project;

static void new_project_ensure_init(void)
{
    if (s_new_project.initialized) return;
    memset(&s_new_project, 0, sizeof(s_new_project));
    snprintf(s_new_project.project_location,
             sizeof(s_new_project.project_location), "C:/Projects");
    s_new_project.initialized = true;
}

void jce_editor_dialog_new_project(bool *p_open)
{
    if (!p_open || !*p_open) return;

    new_project_ensure_init();

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###NewProject",
             jce_editor_i18n("newProject.title"));

    ImGui::SetNextWindowSize(ImVec2(550, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    char _lbl[256];

    /* Heading */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                       jce_editor_i18n("newProject.title"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Project Name */
    ImGui::Text("%s", jce_editor_i18n("newProject.name"));
    snprintf(_lbl, sizeof(_lbl), "###np_name");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(_lbl, s_new_project.project_name,
                     sizeof(s_new_project.project_name));

    ImGui::Spacing();

    /* Location */
    ImGui::Text("%s", jce_editor_i18n("newProject.location"));
    snprintf(_lbl, sizeof(_lbl), "###np_location");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(_lbl, s_new_project.project_location,
                     sizeof(s_new_project.project_location));

    ImGui::Spacing();

    /* Project Type radio buttons */
    snprintf(_lbl, sizeof(_lbl), "%s###np_type3d",
             jce_editor_i18n("newProject.type3d"));
    ImGui::RadioButton(_lbl, &s_new_project.project_type, 0);
    ImGui::SameLine();
    snprintf(_lbl, sizeof(_lbl), "%s###np_type2d",
             jce_editor_i18n("newProject.type2d"));
    ImGui::RadioButton(_lbl, &s_new_project.project_type, 1);

    /* Buttons: Create | Cancel (right-aligned) */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w   = 80.0f;
    float spacing  = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    if (ImGui::Button(jce_editor_i18n("newProject.create"), ImVec2(btn_w, 0))) {
        if (strlen(s_new_project.project_name) > 0) {
            jce_editor_console_log("Created project: %s at %s",
                                   s_new_project.project_name,
                                   s_new_project.project_location);
            *p_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
    }

    ImGui::End();
}

/* ======================================================================
 *  OPEN PROJECT DIALOG
 * ====================================================================== */

static struct {
    char manual_path[512];
    int  selected_recent;
    bool initialized;
} s_open_project;

static void open_project_ensure_init(void)
{
    if (s_open_project.initialized) return;
    memset(&s_open_project, 0, sizeof(s_open_project));
    s_open_project.selected_recent = -1;
    s_open_project.initialized = true;
}

void jce_editor_dialog_open_project(bool *p_open)
{
    if (!p_open || !*p_open) return;

    open_project_ensure_init();

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###OpenProject",
             jce_editor_i18n("openProject.title"));

    ImGui::SetNextWindowSize(ImVec2(650, 450), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    char _lbl[256];

    /* Recent Projects header */
    ImGui::TextColored(JCE_COLOR_ACCENT, "%s",
                       jce_editor_i18n("openProject.recentProjects"));
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Load config for recent projects list */
    JceEditorConfig cfg;
    jce_editor_config_load(&cfg);

    /* Scrollable child region for recent projects */
    float list_height = ImGui::GetContentRegionAvail().y - 100;
    ImGui::BeginChild("##RecentProjectsList", ImVec2(0, list_height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
    for (int i = 0; i < cfg.recent_count; i++) {
        bool is_selected = (s_open_project.selected_recent == i);

        ImGui::PushID(i);
        if (ImGui::Selectable(cfg.recent_projects[i], is_selected,
                              ImGuiSelectableFlags_None,
                              ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) {
            s_open_project.selected_recent = i;
            snprintf(s_open_project.manual_path,
                     sizeof(s_open_project.manual_path),
                     "%s", cfg.recent_projects[i]);
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        if (ImGui::SmallButton("X")) {
            /* Remove from recents by shifting entries down */
            for (int j = i; j < cfg.recent_count - 1; j++) {
                snprintf(cfg.recent_projects[j],
                         sizeof(cfg.recent_projects[j]),
                         "%s", cfg.recent_projects[j + 1]);
            }
            cfg.recent_count--;
            jce_editor_config_save(&cfg);
            if (s_open_project.selected_recent >= cfg.recent_count)
                s_open_project.selected_recent = -1;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Spacing();

    /* Manual path input + Browse button */
    ImGui::Text("%s", jce_editor_i18n("openProject.path"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("###op_path_input", s_open_project.manual_path,
                     sizeof(s_open_project.manual_path));
    ImGui::SameLine();
    snprintf(_lbl, sizeof(_lbl), "%s###op_browse",
             jce_editor_i18n("openProject.browse"));
    if (ImGui::Button(_lbl, ImVec2(80, 0))) {
        jce_editor_console_log("Browse dialog stub");
    }

    /* Buttons: Open | Cancel (right-aligned) */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w   = 80.0f;
    float spacing  = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    snprintf(_lbl, sizeof(_lbl), "%s###op_open",
             jce_editor_i18n("openProject.open"));
    if (ImGui::Button(_lbl, ImVec2(btn_w, 0))) {
        if (strlen(s_open_project.manual_path) > 0) {
            jce_editor_console_log("Opening project: %s",
                                   s_open_project.manual_path);
            *p_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
    }

    ImGui::End();
}

/* ======================================================================
 *  SAVE AS DIALOG
 * ====================================================================== */

static struct {
    char save_name[256];
    char save_location[512];
    bool initialized;
} s_save_as;

static void save_as_ensure_init(void)
{
    if (s_save_as.initialized) return;
    memset(&s_save_as, 0, sizeof(s_save_as));
    snprintf(s_save_as.save_location,
             sizeof(s_save_as.save_location), "C:/Projects");
    s_save_as.initialized = true;
}

void jce_editor_dialog_save_as(bool *p_open)
{
    if (!p_open || !*p_open) return;

    save_as_ensure_init();

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###SaveAs",
             jce_editor_i18n("saveAs.title"));

    ImGui::SetNextWindowSize(ImVec2(500, 250), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    char _lbl[256];

    /* Project Name */
    ImGui::Text("%s", jce_editor_i18n("saveAs.name"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("###sa_name_input", s_save_as.save_name,
                     sizeof(s_save_as.save_name));

    ImGui::Spacing();

    /* Location */
    ImGui::Text("%s", jce_editor_i18n("saveAs.location"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("###sa_loc_input", s_save_as.save_location,
                     sizeof(s_save_as.save_location));

    /* Buttons: Save | Cancel (right-aligned) */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w   = 80.0f;
    float spacing  = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    if (ImGui::Button(jce_editor_i18n("dialog.save"), ImVec2(btn_w, 0))) {
        if (strlen(s_save_as.save_name) > 0) {
            jce_editor_console_log("Saved project as: %s at %s",
                                   s_save_as.save_name,
                                   s_save_as.save_location);
            *p_open = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
    }

    ImGui::End();
}

/* ======================================================================
 *  UNSAVED CHANGES DIALOG
 * ====================================================================== */

void jce_editor_dialog_unsaved_changes(bool *p_open, int *result)
{
    if (!p_open || !*p_open) return;

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###UnsavedChanges",
             jce_editor_i18n("unsaved.title"));

    ImGui::SetNextWindowSize(ImVec2(480, 180), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Warning icon + message */
    ImGui::Spacing();
    ImGui::TextColored(JCE_COLOR_CONSOLE_WARN, "  !!  ");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", jce_editor_i18n("unsaved.message"));

    /* Buttons: Save | Don't Save | Cancel (right-aligned) */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w   = 100.0f;
    float spacing  = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 3 + spacing * 2;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    if (ImGui::Button(jce_editor_i18n("dialog.save"), ImVec2(btn_w, 0))) {
        if (result) *result = 1;
        *p_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("unsaved.dontSave"), ImVec2(btn_w, 0))) {
        if (result) *result = 2;
        *p_open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        if (result) *result = 3;
        *p_open = false;
    }

    ImGui::End();
}
