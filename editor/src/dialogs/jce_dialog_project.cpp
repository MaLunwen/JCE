/*
 * jce_dialog_project.cpp  New Project and Open Project dialog implementations.
 *
 * Shared helpers (folder picker, recent project validation) live here
 * as they are primarily used by project dialogs.  Scene dialogs are in
 * jce_dialog_scene.cpp.
 */

#include "jce_editor_dialogs_internal.h"

/* ── Shared state ─────────────────────────────────────────────────── */

char s_current_project_root[512] = {0};
static char s_last_browse_folder[512] = {0};

void set_current_project_root(const char *path)
{
    if (!path || path[0] == '\0') {
        s_current_project_root[0] = '\0';
        return;
    }
    snprintf(s_current_project_root, sizeof(s_current_project_root), "%s", path);
}

bool is_valid_project_dir(const char *path)
{
    if (!path || path[0] == '\0') return false;
    try {
        fs::path p(path);
        return fs::exists(p) && fs::is_directory(p) && fs::exists(p / "project.jce");
    } catch (...) {
        return false;
    }
}

static bool resolve_project_root_path(const char *path, fs::path *out_project_root)
{
    if (!path || path[0] == '\0' || !out_project_root)
        return false;

    try {
        fs::path current(path);
        if (!fs::exists(current))
            return false;

        if (fs::is_regular_file(current))
            current = current.parent_path();

        while (!current.empty()) {
            if (fs::exists(current / "project.jce")) {
                *out_project_root = current;
                return true;
            }

            fs::path parent = current.parent_path();
            if (parent.empty() || parent == current)
                break;
            current = parent;
        }
    } catch (...) {
    }

    return false;
}

bool sanitize_recent_projects(JceEditorConfig *cfg)
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

bool pick_folder_dialog(const char *title, char *out_path, size_t out_path_size)
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
    bi.hwndOwner = GetActiveWindow();
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
    char error_msg[256];
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

    if (ImGui::IsWindowAppearing())
        s_new_project.error_msg[0] = '\0';

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

    bool name_valid = (strlen(s_new_project.project_name) > 0);

    ImGui::Spacing();

    /* Location */
    ImGui::Text("%s", jce_editor_i18n("newProject.location"));
    snprintf(_lbl, sizeof(_lbl), "###np_location");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    ImGui::InputText(_lbl, s_new_project.project_location,
                     sizeof(s_new_project.project_location));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("openProject.browse"), ImVec2(80, 0))) {
        pick_folder_dialog(jce_editor_i18n("newProject.selectLocation"),
                           s_new_project.project_location,
                           sizeof(s_new_project.project_location));
    }

    bool loc_valid = (strlen(s_new_project.project_location) > 0);
    bool can_create = name_valid && loc_valid;

    /* Inline error message */
    if (s_new_project.error_msg[0] != '\0') {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", s_new_project.error_msg);
    } else if (!name_valid && ImGui::GetFrameCount() > 1) {
        /* Only show hint after first frame (not on dialog open). */
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

    bool enter_pressed = ImGui::IsKeyPressed(ImGuiKey_Enter)
                      || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    ImGui::BeginDisabled(!can_create);
    if (ImGui::Button(jce_editor_i18n("newProject.create"), ImVec2(btn_w, 0))
        || (enter_pressed && can_create)) {
        s_new_project.error_msg[0] = '\0';
        if (!name_valid) {
            snprintf(s_new_project.error_msg, sizeof(s_new_project.error_msg), "%s",
                     jce_editor_i18n("newProject.errorEmpty"));
        } else if (!loc_valid) {
            snprintf(s_new_project.error_msg, sizeof(s_new_project.error_msg), "%s",
                     jce_editor_i18n("newProject.errorLocationEmpty"));
        } else {
            /* Create the project directory structure. */
            fs::path project_dir = fs::path(s_new_project.project_location)
                                 / s_new_project.project_name;
            try {
                if (fs::exists(project_dir) && !fs::is_empty(project_dir)) {
                    snprintf(s_new_project.error_msg, sizeof(s_new_project.error_msg), "%s",
                             jce_editor_i18n("newProject.errorExists"));
                } else {
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
                    *p_open = false;
                }
            } catch (const std::exception &e) {
                snprintf(s_new_project.error_msg, sizeof(s_new_project.error_msg), "%s",
                         jce_editor_i18n("newProject.errorCreate"));
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Failed to create project: %s", e.what());
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
    char error_msg[256];
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

    if (ImGui::IsWindowAppearing())
        s_open_project.error_msg[0] = '\0';

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
    if (s_open_project.cfg.recent_count == 0)
        ImGui::TextDisabled("%s", jce_editor_i18n("openProject.noRecent"));

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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("openProject.removeTooltip"));
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
        if (!pick_folder_dialog(jce_editor_i18n("openProject.selectFolder"),
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
        if (ImGui::Button("Close Browser###op_browse_close", ImVec2(120, 0))) {
            s_open_project.browse_open = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            s_open_project.browse_open = false;
            *p_open = false;
        }
    }

    /* Inline error message */
    if (s_open_project.error_msg[0] != '\0') {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", s_open_project.error_msg);
    }

    if (!s_open_project.browse_open) {
        /* Buttons: Open | Cancel (right-aligned) */
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btn_w   = 80.0f;
        float spacing  = ImGui::GetStyle().ItemSpacing.x;
        float total_btn_w = btn_w * 2 + spacing;
        ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                             + ImGui::GetCursorPosX());

        bool has_path = (strlen(s_open_project.manual_path) > 0);
        bool enter_pressed = ImGui::IsKeyPressed(ImGuiKey_Enter)
                          || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

        snprintf(_lbl, sizeof(_lbl), "%s###op_open",
                 jce_editor_i18n("openProject.open"));
        ImGui::BeginDisabled(!has_path);
        if (ImGui::Button(_lbl, ImVec2(btn_w, 0))
            || (enter_pressed && has_path)) {
            const char *path = s_open_project.manual_path;
            s_open_project.error_msg[0] = '\0';
            if (strlen(path) > 0) {
                fs::path project_root;
                if (resolve_project_root_path(path, &project_root)) {
                    std::string resolved_root = project_root.string();
                    if (!s_open_project.cfg_loaded)
                        jce_editor_config_load(&s_open_project.cfg);
                    jce_editor_config_add_recent(&s_open_project.cfg, resolved_root.c_str());
                    s_open_project.cfg.last_project[0] = '\0';
                    jce_editor_config_save(&s_open_project.cfg);

                    /* Set the asset browser root to the project directory. */
                    jce_editor_assets_set_project(resolved_root.c_str());
                    set_current_project_root(resolved_root.c_str());
                    snprintf(s_open_project.manual_path,
                             sizeof(s_open_project.manual_path), "%s",
                             resolved_root.c_str());
                    jce_editor_layout_request_focus_scene_view();

                    jce_editor_console_log("Opened project: %s", resolved_root.c_str());
                    *p_open = false;
                } else {
                    try {
                        if (!fs::exists(fs::path(path))) {
                            snprintf(s_open_project.error_msg,
                                     sizeof(s_open_project.error_msg), "%s",
                                     jce_editor_i18n("openProject.errorNotExist"));
                        } else {
                            snprintf(s_open_project.error_msg,
                                     sizeof(s_open_project.error_msg), "%s",
                                     jce_editor_i18n("openProject.errorInvalid"));
                        }
                    } catch (...) {
                        snprintf(s_open_project.error_msg,
                                 sizeof(s_open_project.error_msg), "%s",
                                 jce_editor_i18n("openProject.errorOpen"));
                    }
                    jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                        "Project open failed for path: %s", path);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))
            || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            *p_open = false;
        }
    }

    ImGui::End();

    if (!*p_open) {
        was_open = false;
        s_open_project.cfg_loaded = false;
    }
}
