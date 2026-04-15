/*
 * jce_dialog_scene.cpp  Scene dialog implementations.
 *
 * New Scene, Open Scene, Save As, Unsaved Changes.
 */

#include "jce_editor_dialogs_internal.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

static bool is_scene_filename(const std::string &name)
{
    if (name.size() >= 6 && name.substr(name.size() - 6) == ".scene")
        return true;
    if (name.size() >= 11 && name.substr(name.size() - 11) == ".scene.json")
        return true;
    return false;
}

static bool resolve_scene_input_path(const char *input_path,
                                    fs::path *out_scene_file,
                                    fs::path *out_scene_dir)
{
    if (out_scene_file)
        *out_scene_file = fs::path();
    if (out_scene_dir)
        *out_scene_dir = fs::path();
    if (!input_path || input_path[0] == '\0')
        return false;

    try {
        fs::path path(input_path);
        if (!fs::exists(path))
            return false;

        if (fs::is_directory(path)) {
            if (out_scene_dir)
                *out_scene_dir = path;
            return true;
        }

        if (fs::is_regular_file(path) && is_scene_filename(path.filename().string())) {
            if (out_scene_file)
                *out_scene_file = path;
            if (out_scene_dir)
                *out_scene_dir = path.parent_path();
            return true;
        }
    } catch (...) {
    }

    return false;
}

/* ======================================================================
 *  NEW SCENE DIALOG
 * ====================================================================== */

static struct {
    char scene_name[256];
    char scene_dir[512];
    char last_project_root[512];
    bool initialized;
    char error_msg[256];
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

    if (ImGui::IsWindowAppearing())
        s_new_scene.error_msg[0] = '\0';

    ImGui::Text("%s", jce_editor_i18n("sceneDialog.name"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("###ns_name", s_new_scene.scene_name,
                     sizeof(s_new_scene.scene_name));

    bool name_valid = (s_new_scene.scene_name[0] != '\0');

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

    bool dir_valid = (s_new_scene.scene_dir[0] != '\0');
    bool can_create = name_valid && dir_valid;

    /* Inline error */
    if (s_new_scene.error_msg[0] != '\0') {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", s_new_scene.error_msg);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float btn_w = 90.0f;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_btn_w = btn_w * 2 + spacing;
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x - total_btn_w
                         + ImGui::GetCursorPosX());

    bool enter_pressed = ImGui::IsKeyPressed(ImGuiKey_Enter)
                      || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    ImGui::BeginDisabled(!can_create);
    if (ImGui::Button(jce_editor_i18n("dialog.create"), ImVec2(btn_w, 0))
        || (enter_pressed && can_create)) {
        s_new_scene.error_msg[0] = '\0';
        if (s_new_scene.scene_name[0] == '\0' || s_new_scene.scene_dir[0] == '\0') {
            snprintf(s_new_scene.error_msg, sizeof(s_new_scene.error_msg),
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
                    if (jce_state_load_scene_file(scene_path.string().c_str())) {
                        jce_editor_console_log("Created scene: %s", scene_path.string().c_str());
                        jce_editor_layout_request_focus_scene_view();
                        *p_open = false;
                    } else {
                        snprintf(s_new_scene.error_msg, sizeof(s_new_scene.error_msg), "%s",
                                 jce_editor_i18n("sceneDialog.errorOpen"));
                    }
                }
            } catch (const std::exception &e) {
                snprintf(s_new_scene.error_msg, sizeof(s_new_scene.error_msg),
                    "Create scene failed: %s", e.what());
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "Create scene failed: %s", e.what());
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
 *  OPEN SCENE DIALOG
 * ====================================================================== */

static struct {
    char scene_dir[512];
    char last_project_root[512];
    std::vector<std::string> scene_files;
    int selected_idx;
    bool refresh;
    bool initialized;
    char error_msg[256];
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
    s_open_scene.error_msg[0] = '\0';

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
        fs::path direct_scene;
        fs::path browse_dir;
        if (!resolve_scene_input_path(s_open_scene.scene_dir,
                                      &direct_scene,
                                      &browse_dir)
            || browse_dir.empty()) {
            s_open_scene.refresh = false;
            return;
        }

        for (auto &de : fs::directory_iterator(browse_dir)) {
            if (!de.is_regular_file()) continue;
            std::string name = de.path().filename().string();
            if (is_scene_filename(name))
                s_open_scene.scene_files.push_back(name);
        }
        std::sort(s_open_scene.scene_files.begin(), s_open_scene.scene_files.end());

        if (!direct_scene.empty()) {
            std::string selected_name = direct_scene.filename().string();
            for (int index = 0; index < (int)s_open_scene.scene_files.size(); index++) {
                if (s_open_scene.scene_files[index] == selected_name) {
                    s_open_scene.selected_idx = index;
                    break;
                }
            }
        }
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

    if (ImGui::IsWindowAppearing())
        s_open_scene.error_msg[0] = '\0';

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
    bool open_from_double_click = false;
    for (int i = 0; i < (int)s_open_scene.scene_files.size(); i++) {
        if (ImGui::Selectable(s_open_scene.scene_files[i].c_str(),
                              s_open_scene.selected_idx == i,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            s_open_scene.selected_idx = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                open_from_double_click = true;
        }
    }
    ImGui::EndChild();

    fs::path direct_scene_path;
    fs::path resolved_scene_dir;
    bool resolved_scene_input = resolve_scene_input_path(s_open_scene.scene_dir,
                                                         &direct_scene_path,
                                                         &resolved_scene_dir);
    bool can_open = (!direct_scene_path.empty()) || s_open_scene.selected_idx >= 0;

    if (s_open_scene.error_msg[0] != '\0')
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", s_open_scene.error_msg);

    ImGui::BeginDisabled(!can_open);
    if (ImGui::Button(jce_editor_i18n("dialog.open"), ImVec2(90, 0))
        || (can_open
            && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        || (can_open && open_from_double_click)) {
        s_open_scene.error_msg[0] = '\0';

        fs::path full;
        if (!direct_scene_path.empty())
            full = direct_scene_path;
        else if (resolved_scene_input && s_open_scene.selected_idx >= 0)
            full = resolved_scene_dir / s_open_scene.scene_files[s_open_scene.selected_idx];

        if (!full.empty() && fs::exists(full) && jce_state_load_scene_file(full.string().c_str())) {
            jce_editor_layout_request_focus_scene_view();
            jce_editor_console_log("Opened scene: %s", full.string().c_str());
            *p_open = false;
        } else if (!full.empty()) {
            snprintf(s_open_scene.error_msg, sizeof(s_open_scene.error_msg), "%s",
                     jce_editor_i18n("sceneDialog.errorOpen"));
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "Failed to open scene: %s", full.string().c_str());
        } else {
            snprintf(s_open_scene.error_msg, sizeof(s_open_scene.error_msg), "%s",
                     jce_editor_i18n("sceneDialog.errorInvalid"));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(90, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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

    if (ImGui::Button(jce_editor_i18n("dialog.save"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Enter)
        || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
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
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
    ImGui::SetNextWindowSize(ImVec2(500, 180), ImGuiCond_Appearing);
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
    ImGui::TextColored(JCE_COLOR_CONSOLE_WARN, " ? ");
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

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.62f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.70f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.52f, 0.20f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("dialog.save"), ImVec2(btn_w, 0))
        || ImGui::IsKeyPressed(ImGuiKey_Enter)
        || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (result) *result = 1;
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.30f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("unsaved.dontSave"), ImVec2(btn_w, 0))) {
        if (result) *result = 2;
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
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
