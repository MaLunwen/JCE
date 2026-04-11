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
#include "jce_file_viewer.h"
#include "jce_editor_layout.h"
#include "jce_editor_state.h"

#include <imgui.h>
#include <stdio.h>
#include <string.h>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace fs = std::filesystem;

static char s_current_project_root[512] = {0};
static char s_last_browse_folder[512] = {0};

static void set_current_project_root(const char *path)
{
    if (!path || path[0] == '\0') {
        s_current_project_root[0] = '\0';
        return;
    }
    snprintf(s_current_project_root, sizeof(s_current_project_root), "%s", path);
}

static bool is_valid_project_dir(const char *path)
{
    if (!path || path[0] == '\0') return false;
    try {
        fs::path p(path);
        return fs::exists(p) && fs::is_directory(p) && fs::exists(p / "project.jce");
    } catch (...) {
        return false;
    }
}

static bool sanitize_recent_projects(JceEditorConfig *cfg)
{
    if (!cfg) return false;

    bool changed = false;
    int write_idx = 0;

    for (int i = 0; i < cfg->recent_count && i < 10; i++) {
        const char *src = cfg->recent_projects[i];
        if (!is_valid_project_dir(src)) {
            changed = true;
            continue;
        }

        bool duplicate = false;
        for (int j = 0; j < write_idx; j++) {
            if (strcmp(cfg->recent_projects[j], src) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            changed = true;
            continue;
        }

        if (write_idx != i) {
            snprintf(cfg->recent_projects[write_idx],
                     sizeof(cfg->recent_projects[write_idx]), "%s", src);
            changed = true;
        }
        write_idx++;
    }

    for (int i = write_idx; i < 10; i++) {
        if (cfg->recent_projects[i][0] != '\0')
            changed = true;
        cfg->recent_projects[i][0] = '\0';
    }

    if (cfg->recent_count != write_idx)
        changed = true;
    cfg->recent_count = write_idx;

    if (cfg->last_project[0] != '\0' && !is_valid_project_dir(cfg->last_project)) {
        cfg->last_project[0] = '\0';
        changed = true;
    }

    return changed;
}

static bool pick_folder_dialog(const char *title, char *out_path, size_t out_path_size)
{
    if (!out_path || out_path_size == 0) return false;
#ifdef _WIN32
    auto browse_callback = [](HWND hwnd, UINT uMsg, LPARAM, LPARAM lpData) -> int {
        if (uMsg == BFFM_INITIALIZED) {
            const char *initial = (const char *)lpData;
            if (initial && initial[0] != '\0')
                SendMessageA(hwnd, BFFM_SETSELECTIONA, TRUE, (LPARAM)initial);
        }
        return 0;
    };

    const char *initial_folder = nullptr;
    if (s_last_browse_folder[0] != '\0')
        initial_folder = s_last_browse_folder;
    else if (out_path[0] != '\0')
        initial_folder = out_path;

    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    bi.lpfn = browse_callback;
    bi.lParam = (LPARAM)initial_folder;

    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;

    char path[MAX_PATH] = {0};
    bool ok = (SHGetPathFromIDListA(pidl, path) == TRUE);
    CoTaskMemFree(pidl);
    if (!ok) return false;

    snprintf(out_path, out_path_size, "%s", path);
    snprintf(s_last_browse_folder, sizeof(s_last_browse_folder), "%s", path);
    return true;
#else
    (void)title;
    (void)out_path;
    (void)out_path_size;
    return false;
#endif
}

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
    if (!ImGui::Begin(_title, p_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
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
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText(_lbl, s_new_project.project_location,
                     sizeof(s_new_project.project_location));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("openProject.browse"), ImVec2(80, 0))) {
        pick_folder_dialog("Choose project location",
                           s_new_project.project_location,
                           sizeof(s_new_project.project_location));
    }

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
            /* Create the project directory structure. */
            fs::path project_dir = fs::path(s_new_project.project_location)
                                 / s_new_project.project_name;
            try {
                fs::create_directories(project_dir);
                fs::create_directories(project_dir / "assets" / "scenes");
                fs::create_directories(project_dir / "assets" / "textures");
                fs::create_directories(project_dir / "assets" / "models");
                fs::create_directories(project_dir / "assets" / "audio");
                fs::create_directories(project_dir / "assets" / "scripts");
                fs::create_directories(project_dir / "build");

                /* Write a minimal project file with editor/engine version. */
                fs::path proj_file = project_dir / "project.jce";
                FILE *pf = fopen(proj_file.string().c_str(), "w");
                if (pf) {
                    fprintf(pf, "{\n");
                    fprintf(pf, "    \"name\": \"%s\",\n", s_new_project.project_name);
                    fprintf(pf, "    \"type\": \"%s\",\n",
                            s_new_project.project_type == 0 ? "3D" : "2D");
                    fprintf(pf, "    \"version\": \"1.0\",\n");
                    fprintf(pf, "    \"engineVersion\": \"0.1.0\",\n");
                    fprintf(pf, "    \"editorVersion\": \"0.1.0\"\n");
                    fprintf(pf, "}\n");
                    fclose(pf);
                }

                /* Add to recent projects. */
                JceEditorConfig ecfg;
                jce_editor_config_load(&ecfg);
                jce_editor_config_add_recent(&ecfg, project_dir.string().c_str());
                ecfg.last_project[0] = '\0';
                jce_editor_config_save(&ecfg);

                /* Set the asset browser root to the new project. */
                jce_editor_assets_set_project(project_dir.string().c_str());
                set_current_project_root(project_dir.string().c_str());
                jce_editor_layout_request_focus_scene_view();

                jce_editor_console_log("Created project: %s at %s",
                                       s_new_project.project_name,
                                       project_dir.string().c_str());
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Failed to create project: %s", e.what());
            }
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
    JceEditorConfig cfg;
    bool cfg_loaded;
    /* Browse state */
    char browse_path[512];
    std::vector<std::string> browse_entries;
    bool browse_open;
    bool browse_refresh;
    bool initialized;
} s_open_project;

static void open_project_ensure_init(void)
{
    if (s_open_project.initialized) return;
    /* Field-by-field init — cannot memset because of std::vector member. */
    s_open_project.manual_path[0]  = '\0';
    s_open_project.selected_recent = -1;
    memset(&s_open_project.cfg, 0, sizeof(s_open_project.cfg));
    s_open_project.cfg_loaded      = false;
    s_open_project.browse_entries.clear();
    s_open_project.browse_open     = false;
    s_open_project.browse_refresh  = true;
    /* Default browse to current directory or user home. */
    snprintf(s_open_project.browse_path, sizeof(s_open_project.browse_path),
             "%s", fs::current_path().string().c_str());
    s_open_project.initialized = true;
}

static void browse_refresh_entries(void)
{
    s_open_project.browse_entries.clear();
    try {
        /* Add parent directory entry. */
        fs::path cur(s_open_project.browse_path);
        if (cur.has_parent_path() && cur.parent_path() != cur)
            s_open_project.browse_entries.push_back("..");

        std::vector<std::string> dirs, files;
        for (auto &de : fs::directory_iterator(cur)) {
            if (de.is_directory())
                dirs.push_back(de.path().filename().string());
        }
        std::sort(dirs.begin(), dirs.end());
        for (auto &d : dirs)
            s_open_project.browse_entries.push_back(d);
    } catch (...) {}
    s_open_project.browse_refresh = false;
}

void jce_editor_dialog_open_project(bool *p_open)
{
    static bool was_open = false;
    if (!p_open) return;
    if (!*p_open) {
        was_open = false;
        s_open_project.cfg_loaded = false;
        return;
    }

    open_project_ensure_init();
    if (!was_open || !s_open_project.cfg_loaded) {
        jce_editor_config_load(&s_open_project.cfg);
        if (sanitize_recent_projects(&s_open_project.cfg))
            jce_editor_config_save(&s_open_project.cfg);
        s_open_project.cfg_loaded = true;
        if (s_open_project.selected_recent >= s_open_project.cfg.recent_count)
            s_open_project.selected_recent = -1;
    }
    was_open = true;

    char _title[256];
    snprintf(_title, sizeof(_title), "%s###OpenProject",
             jce_editor_i18n("openProject.title"));

    ImGui::SetNextWindowSize(ImVec2(650, 450), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_title, p_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
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

    /* Scrollable child region for recent projects */
    float list_height = ImGui::GetContentRegionAvail().y - 100;
    ImGui::BeginChild("##RecentProjectsList", ImVec2(0, list_height),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
    for (int i = 0; i < s_open_project.cfg.recent_count; i++) {
        bool is_selected = (s_open_project.selected_recent == i);

        ImGui::PushID(i);
        if (ImGui::Selectable(s_open_project.cfg.recent_projects[i], is_selected,
                              ImGuiSelectableFlags_None,
                              ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) {
            s_open_project.selected_recent = i;
            snprintf(s_open_project.manual_path,
                     sizeof(s_open_project.manual_path),
                     "%s", s_open_project.cfg.recent_projects[i]);
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20);
        if (ImGui::SmallButton("X")) {
            /* Remove from recents by shifting entries down */
            for (int j = i; j < s_open_project.cfg.recent_count - 1; j++) {
                snprintf(s_open_project.cfg.recent_projects[j],
                         sizeof(s_open_project.cfg.recent_projects[j]),
                         "%s", s_open_project.cfg.recent_projects[j + 1]);
            }
            s_open_project.cfg.recent_count--;
            s_open_project.cfg.last_project[0] = '\0';
            jce_editor_config_save(&s_open_project.cfg);
            if (s_open_project.selected_recent >= s_open_project.cfg.recent_count)
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
        if (!pick_folder_dialog("Choose project folder",
                                s_open_project.manual_path,
                                sizeof(s_open_project.manual_path))) {
            s_open_project.browse_open = true;
            s_open_project.browse_refresh = true;
        } else {
            snprintf(s_open_project.browse_path,
                     sizeof(s_open_project.browse_path),
                     "%s", s_open_project.manual_path);
            s_open_project.browse_open = false;
        }
    }

    /* Inline folder browser (shown when Browse was clicked) */
    if (s_open_project.browse_open) {
        ImGui::Spacing();
        ImGui::TextColored(JCE_COLOR_ACCENT, "%s", s_open_project.browse_path);

        if (s_open_project.browse_refresh)
            browse_refresh_entries();

        float list_h = 200.0f;
        ImGui::BeginChild("##BrowseDirs", ImVec2(0, list_h),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
        for (int i = 0; i < (int)s_open_project.browse_entries.size(); i++) {
            const char *name = s_open_project.browse_entries[i].c_str();
            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_ASSET_FOLDER);
            if (ImGui::Selectable(name, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    fs::path cur(s_open_project.browse_path);
                    fs::path next;
                    if (strcmp(name, "..") == 0)
                        next = cur.parent_path();
                    else
                        next = cur / name;
                    snprintf(s_open_project.browse_path,
                             sizeof(s_open_project.browse_path),
                             "%s", next.string().c_str());
                    s_open_project.browse_refresh = true;
                } else {
                    /* Single click: set as selected path. */
                    fs::path selected;
                    if (strcmp(name, "..") == 0)
                        selected = fs::path(s_open_project.browse_path).parent_path();
                    else
                        selected = fs::path(s_open_project.browse_path) / name;
                    snprintf(s_open_project.manual_path,
                             sizeof(s_open_project.manual_path),
                             "%s", selected.string().c_str());
                }
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();

        if (ImGui::Button(jce_editor_i18n("openProject.selectFolder"), ImVec2(120, 0))) {
            snprintf(s_open_project.manual_path,
                     sizeof(s_open_project.manual_path),
                     "%s", s_open_project.browse_path);
            s_open_project.browse_open = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(80, 0))) {
            s_open_project.browse_open = false;
        }
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
        const char *path = s_open_project.manual_path;
        if (strlen(path) > 0) {
            fs::path proj_file = fs::path(path) / "project.jce";
            if (fs::exists(proj_file)) {
                if (!s_open_project.cfg_loaded)
                    jce_editor_config_load(&s_open_project.cfg);
                jce_editor_config_add_recent(&s_open_project.cfg, path);
                s_open_project.cfg.last_project[0] = '\0';
                jce_editor_config_save(&s_open_project.cfg);

                /* Set the asset browser root to the project directory. */
                jce_editor_assets_set_project(path);
                set_current_project_root(path);
                jce_editor_layout_request_focus_scene_view();

                jce_editor_console_log("Opened project: %s", path);
                *p_open = false;
            } else {
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "No project.jce found in: %s", path);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
    }

    ImGui::End();

    if (!*p_open) {
        was_open = false;
        s_open_project.cfg_loaded = false;
    }
}

/* ======================================================================
 *  SCENE DIALOGS
 * ====================================================================== */

static bool is_scene_filename(const std::string &name)
{
    if (name.size() >= 6 && name.substr(name.size() - 6) == ".scene")
        return true;
    if (name.size() >= 11 && name.substr(name.size() - 11) == ".scene.json")
        return true;
    return false;
}

static struct {
    char scene_name[256];
    char scene_dir[512];
    char last_project_root[512];
    bool initialized;
} s_new_scene;

static void new_scene_ensure_init(void)
{
    if (s_new_scene.initialized
        && strcmp(s_new_scene.last_project_root, s_current_project_root) == 0)
        return;

    memset(&s_new_scene, 0, sizeof(s_new_scene));

    if (s_current_project_root[0] != '\0') {
        fs::path p = fs::path(s_current_project_root) / "assets" / "scenes";
        snprintf(s_new_scene.scene_dir, sizeof(s_new_scene.scene_dir), "%s",
                 p.string().c_str());
    } else {
        snprintf(s_new_scene.scene_dir, sizeof(s_new_scene.scene_dir), "%s",
                 fs::current_path().string().c_str());
    }
    snprintf(s_new_scene.last_project_root, sizeof(s_new_scene.last_project_root),
             "%s", s_current_project_root);
    snprintf(s_new_scene.scene_name, sizeof(s_new_scene.scene_name), "%s",
             jce_editor_i18n("menu.file.newScene"));
    s_new_scene.initialized = true;
}

void jce_editor_dialog_new_scene(bool *p_open)
{
    if (!p_open || !*p_open) return;

    new_scene_ensure_init();

    ImGui::SetNextWindowSize(ImVec2(560, 240), ImGuiCond_FirstUseEver);
    char title[256];
    snprintf(title, sizeof(title), "%s###NewScene", jce_editor_i18n("menu.file.newScene"));
    if (!ImGui::Begin(title, p_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::Text("%s", jce_editor_i18n("sceneDialog.name"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("###ns_name", s_new_scene.scene_name,
                     sizeof(s_new_scene.scene_name));

    ImGui::Spacing();
    ImGui::Text("%s", jce_editor_i18n("sceneDialog.directory"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText("###ns_dir", s_new_scene.scene_dir,
                     sizeof(s_new_scene.scene_dir));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("openProject.browse"), ImVec2(80, 0))) {
        pick_folder_dialog(jce_editor_i18n("sceneDialog.chooseDirectory"),
                           s_new_scene.scene_dir,
                           sizeof(s_new_scene.scene_dir));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 90.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    if (ImGui::Button(jce_editor_i18n("dialog.create"), ImVec2(btn_w, 0))) {
        if (s_new_scene.scene_name[0] == '\0' || s_new_scene.scene_dir[0] == '\0') {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "%s", jce_editor_i18n("sceneDialog.errorRequired"));
        } else {
            try {
                fs::create_directories(s_new_scene.scene_dir);

                std::string name = s_new_scene.scene_name;
                if (!is_scene_filename(name)) name += ".scene";
                fs::path scene_path = fs::path(s_new_scene.scene_dir) / name;

                FILE *fp = fopen(scene_path.string().c_str(), "w");
                if (!fp) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Failed to create scene file: %s", scene_path.string().c_str());
                } else {
                    fputs("{}", fp);
                    fclose(fp);
                    jce_editor_console_log("Created scene: %s", scene_path.string().c_str());
                    jce_file_viewer_open(scene_path.string().c_str());
                    jce_editor_layout_request_focus_scene_view();
                    *p_open = false;
                }
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Create scene failed: %s", e.what());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        *p_open = false;
    }

    ImGui::End();
}

static struct {
    char scene_dir[512];
    char last_project_root[512];
    std::vector<std::string> scene_files;
    int selected_idx;
    bool refresh;
    bool initialized;
} s_open_scene;

static void open_scene_ensure_init(void)
{
    if (s_open_scene.initialized
        && strcmp(s_open_scene.last_project_root, s_current_project_root) == 0)
        return;

    /* Field-by-field init — cannot memset because of std::vector member. */
    s_open_scene.scene_dir[0] = '\0';
    s_open_scene.last_project_root[0] = '\0';
    s_open_scene.scene_files.clear();
    s_open_scene.selected_idx = -1;
    s_open_scene.refresh = true;

    if (s_current_project_root[0] != '\0') {
        fs::path p = fs::path(s_current_project_root) / "assets" / "scenes";
        snprintf(s_open_scene.scene_dir, sizeof(s_open_scene.scene_dir), "%s",
                 p.string().c_str());
    } else {
        snprintf(s_open_scene.scene_dir, sizeof(s_open_scene.scene_dir), "%s",
                 fs::current_path().string().c_str());
    }

    snprintf(s_open_scene.last_project_root, sizeof(s_open_scene.last_project_root),
             "%s", s_current_project_root);

    s_open_scene.initialized = true;
}

static void open_scene_refresh_entries(void)
{
    s_open_scene.scene_files.clear();
    s_open_scene.selected_idx = -1;
    try {
        for (auto &de : fs::directory_iterator(s_open_scene.scene_dir)) {
            if (!de.is_regular_file()) continue;
            std::string name = de.path().filename().string();
            if (is_scene_filename(name))
                s_open_scene.scene_files.push_back(name);
        }
        std::sort(s_open_scene.scene_files.begin(), s_open_scene.scene_files.end());
    } catch (...) {
    }
    s_open_scene.refresh = false;
}

void jce_editor_dialog_open_scene(bool *p_open)
{
    if (!p_open || !*p_open) return;

    open_scene_ensure_init();
    if (s_open_scene.refresh)
        open_scene_refresh_entries();

    ImGui::SetNextWindowSize(ImVec2(620, 420), ImGuiCond_FirstUseEver);
    char title[256];
    snprintf(title, sizeof(title), "%s###OpenScene", jce_editor_i18n("menu.file.openScene"));
    if (!ImGui::Begin(title, p_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::Text("%s", jce_editor_i18n("sceneDialog.sceneDirectory"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 180.0f);
    ImGui::InputText("###os_dir", s_open_scene.scene_dir,
                     sizeof(s_open_scene.scene_dir));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("openProject.browse"), ImVec2(80, 0))) {
        if (pick_folder_dialog(jce_editor_i18n("sceneDialog.chooseDirectory"),
                               s_open_scene.scene_dir,
                               sizeof(s_open_scene.scene_dir))) {
            s_open_scene.refresh = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("assetBrowser.refresh"), ImVec2(80, 0))) {
        s_open_scene.refresh = true;
    }

    if (s_open_scene.refresh)
        open_scene_refresh_entries();

    ImGui::Spacing();
    ImGui::BeginChild("##scene_file_list", ImVec2(0, ImGui::GetContentRegionAvail().y - 44.0f),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_None);
    for (int i = 0; i < (int)s_open_scene.scene_files.size(); i++) {
        if (ImGui::Selectable(s_open_scene.scene_files[i].c_str(), s_open_scene.selected_idx == i)) {
            s_open_scene.selected_idx = i;
        }
    }
    ImGui::EndChild();

    ImGui::BeginDisabled(s_open_scene.selected_idx < 0);
    if (ImGui::Button(jce_editor_i18n("dialog.open"), ImVec2(90, 0))) {
        fs::path full = fs::path(s_open_scene.scene_dir)
                      / s_open_scene.scene_files[s_open_scene.selected_idx];
        if (fs::exists(full)) {
            jce_file_viewer_open(full.string().c_str());
            jce_editor_layout_request_focus_scene_view();
            jce_editor_console_log("Opened scene: %s", full.string().c_str());
            *p_open = false;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(90, 0))) {
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
    char source_scene_path[512];
    bool initialized;
} s_save_as;

static std::string strip_scene_extension(const std::string &file_name)
{
    if (file_name.size() >= 11 && file_name.substr(file_name.size() - 11) == ".scene.json")
        return file_name.substr(0, file_name.size() - 11);
    if (file_name.size() >= 6 && file_name.substr(file_name.size() - 6) == ".scene")
        return file_name.substr(0, file_name.size() - 6);
    return fs::path(file_name).stem().string();
}

static void save_as_ensure_init(void)
{
    const char *current_scene = jce_state_get_current_scene_path();
    if (current_scene && current_scene[0] != '\0'
        && (!s_save_as.initialized
         || strcmp(s_save_as.source_scene_path, current_scene) != 0)) {
        fs::path p(current_scene);
        std::string parent = p.parent_path().string();
        std::string base = strip_scene_extension(p.filename().string());

        memset(&s_save_as, 0, sizeof(s_save_as));
        snprintf(s_save_as.save_name, sizeof(s_save_as.save_name), "%s",
                 base.empty() ? "Scene" : base.c_str());
        snprintf(s_save_as.save_location, sizeof(s_save_as.save_location), "%s",
                 parent.empty() ? fs::current_path().string().c_str() : parent.c_str());
        snprintf(s_save_as.source_scene_path, sizeof(s_save_as.source_scene_path), "%s",
                 current_scene);
        s_save_as.initialized = true;
        return;
    }

    if (s_save_as.initialized) return;

    memset(&s_save_as, 0, sizeof(s_save_as));
    if (s_current_project_root[0] != '\0') {
        fs::path p = fs::path(s_current_project_root) / "assets" / "scenes";
        snprintf(s_save_as.save_location,
                 sizeof(s_save_as.save_location), "%s", p.string().c_str());
    } else {
        snprintf(s_save_as.save_location,
                 sizeof(s_save_as.save_location), "%s", fs::current_path().string().c_str());
    }
    snprintf(s_save_as.save_name, sizeof(s_save_as.save_name), "Scene");
    s_save_as.initialized = true;
}

void jce_editor_dialog_save_as(bool *p_open)
{
    if (!p_open || !*p_open) return;

    save_as_ensure_init();

    char title[256];
    snprintf(title, sizeof(title), "%s###SaveAsScene", jce_editor_i18n("menu.file.saveAs"));

    ImGui::SetNextWindowSize(ImVec2(500, 250), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    /* Scene Name */
    ImGui::Text("%s", jce_editor_i18n("sceneDialog.name"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("###sa_name_input", s_save_as.save_name,
                     sizeof(s_save_as.save_name));

    ImGui::Spacing();

    /* Location */
    ImGui::Text("%s", jce_editor_i18n("sceneDialog.directory"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText("###sa_loc_input", s_save_as.save_location,
                     sizeof(s_save_as.save_location));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("openProject.browse"), ImVec2(80, 0))) {
        pick_folder_dialog(jce_editor_i18n("sceneDialog.chooseDirectory"),
                           s_save_as.save_location,
                           sizeof(s_save_as.save_location));
    }

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
        if (strlen(s_save_as.save_name) == 0 || strlen(s_save_as.save_location) == 0) {
            jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                "%s", jce_editor_i18n("sceneDialog.errorRequired"));
        } else {
            try {
                fs::create_directories(s_save_as.save_location);

                std::string name = s_save_as.save_name;
                if (!is_scene_filename(name)) name += ".scene";
                fs::path out_path = fs::path(s_save_as.save_location) / name;

                if (jce_state_save_scene_file(out_path.string().c_str())) {
                    snprintf(s_save_as.source_scene_path,
                             sizeof(s_save_as.source_scene_path), "%s",
                             out_path.string().c_str());
                    jce_file_viewer_open(out_path.string().c_str());
                    jce_editor_layout_request_focus_scene_view();
                    jce_editor_console_log("Saved scene as: %s", out_path.string().c_str());
                    *p_open = false;
                } else {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Save scene failed: %s", out_path.string().c_str());
                }
            } catch (const std::exception &e) {
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Save scene failed: %s", e.what());
            }
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
    snprintf(_title, sizeof(_title), "%s###UnsavedDialog",
             jce_editor_i18n("unsaved.title"));

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 180), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::Begin(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_NoResize)) {
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

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (result) *result = 3;
        *p_open = false;
    }

    ImGui::End();
}
