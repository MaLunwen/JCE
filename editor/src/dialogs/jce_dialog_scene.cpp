/*
 * jce_dialog_scene.cpp  Scene dialog implementations.
 *
 * New Scene, Open Scene, Save As, Unsaved Changes.
 */

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>

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
                                    std::string *out_scene_file,
                                    std::string *out_scene_dir)
{
    if (out_scene_file)
        *out_scene_file = "";
    if (out_scene_dir)
        *out_scene_dir = "";
    if (!input_path || input_path[0] == '\0')
        return false;

    bool is_dir = jce_fs_host_exists_dir(input_path);
    bool is_file = jce_fs_host_exists_file(input_path);
    
    if (!is_dir && !is_file)
        return false;

    if (is_dir) {
        if (out_scene_dir)
            *out_scene_dir = input_path;
        return true;
    }

    char basename[256];
    jce_path_basename(basename, sizeof(basename), input_path);
    if (is_file && is_scene_filename(basename)) {
        if (out_scene_file)
            *out_scene_file = input_path;
        if (out_scene_dir) {
            char parent[1024];
            jce_path_parent(parent, sizeof(parent), input_path);
            *out_scene_dir = parent;
        }
        return true;
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
        char p[1024];
        jce_path_join(p, sizeof(p), s_current_project_root, "assets");
        jce_path_join(p, sizeof(p), p, "scenes");
        snprintf(s_new_scene.scene_dir, sizeof(s_new_scene.scene_dir), "%s", p);
    } else {
        char base[1024];
        if (jce_fs_host_get_current_dir(base, sizeof(base))) {
            snprintf(s_new_scene.scene_dir, sizeof(s_new_scene.scene_dir), "%s", base);
        } else {
            snprintf(s_new_scene.scene_dir, sizeof(s_new_scene.scene_dir), ".");
        }
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
        pick_folder_dialog_async(jce_editor_i18n("sceneDialog.chooseDirectory"),
                                 NULL,
                                 s_new_scene.scene_dir,
                                 sizeof(s_new_scene.scene_dir),
                                 NULL, 0, NULL, NULL);
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
                jce_fs_host_create_directory(s_new_scene.scene_dir);

                std::string name = s_new_scene.scene_name;
                if (!is_scene_filename(name)) name += ".scene";
                char scene_path_buf[1024];
                jce_path_join(scene_path_buf, sizeof(scene_path_buf),
                              s_new_scene.scene_dir, name.c_str());
                std::string scene_path = scene_path_buf;

                static const char empty_scene[] = "{}";
                if (!ed_write_file(scene_path.c_str(),
                                   empty_scene, sizeof(empty_scene) - 1)) {
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "Failed to create scene file: %s", scene_path.c_str());
                } else {
                    if (jce_state_load_scene_file(scene_path.c_str())) {
                        jce_editor_console_log("Created scene: %s", scene_path.c_str());
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
        char p[1024];
        jce_path_join(p, sizeof(p), s_current_project_root, "assets");
        jce_path_join(p, sizeof(p), p, "scenes");
        snprintf(s_open_scene.scene_dir, sizeof(s_open_scene.scene_dir), "%s", p);
    } else {
        char base[1024];
        if (jce_fs_host_get_current_dir(base, sizeof(base))) {
            snprintf(s_open_scene.scene_dir, sizeof(s_open_scene.scene_dir), "%s", base);
        } else {
            snprintf(s_open_scene.scene_dir, sizeof(s_open_scene.scene_dir), ".");
        }
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
        std::string direct_scene_path;
        std::string browse_dir;
        if (!resolve_scene_input_path(s_open_scene.scene_dir,
                                      &direct_scene_path,
                                      &browse_dir)
            || browse_dir.empty()) {
            s_open_scene.refresh = false;
            return;
        }

        struct ListCtx {
            std::vector<std::string> *files;
        } ctx;
        ctx.files = &s_open_scene.scene_files;
        
        auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
            if (!is_dir && is_scene_filename(name)) {
                static_cast<ListCtx*>(ud)->files->push_back(name);
            }
            return true;
        };
        
        jce_fs_host_list_dir(browse_dir.c_str(), cb, &ctx);
        std::sort(s_open_scene.scene_files.begin(), s_open_scene.scene_files.end());

        if (!direct_scene_path.empty()) {
            char basename[256];
            jce_path_basename(basename, sizeof(basename), direct_scene_path.c_str());
            std::string selected_name = basename;
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
        /* Async pick: callback flips s_open_scene.refresh on success. */
        pick_folder_dialog_async(jce_editor_i18n("sceneDialog.chooseDirectory"),
                                 NULL,
                                 s_open_scene.scene_dir,
                                 sizeof(s_open_scene.scene_dir),
                                 NULL, 0,
                                 &s_open_scene.refresh,
                                 NULL);
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

    std::string direct_scene_path;
    std::string resolved_scene_dir;
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

        char full[1024];
        full[0] = '\0';
        
        if (!direct_scene_path.empty()) {
            snprintf(full, sizeof(full), "%s", direct_scene_path.c_str());
        } else if (resolved_scene_input && s_open_scene.selected_idx >= 0) {
            jce_path_join(full, sizeof(full),
                         resolved_scene_dir.c_str(),
                         s_open_scene.scene_files[s_open_scene.selected_idx].c_str());
        }

        if (full[0] != '\0' && jce_fs_host_exists_file(full) && jce_state_load_scene_file(full)) {
            jce_editor_layout_request_focus_scene_view();
            jce_editor_console_log("Opened scene: %s", full);
            *p_open = false;
        } else if (full[0] != '\0') {
            snprintf(s_open_scene.error_msg, sizeof(s_open_scene.error_msg), "%s",
                     jce_editor_i18n("sceneDialog.errorOpen"));
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "Failed to open scene: %s", full);
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
 *  SAVE AS — native-only flow
 *
 *  No ImGui form.  When the menu fires Save Scene As (*p_open becomes
 *  true), we synchronously kick off SDL_ShowSaveFileDialog with a
 *  sensible default path/filename derived from the current scene.  The
 *  callback writes the chosen path into a result buffer; on the next
 *  frame we detect ready_flag and perform the save, then drop *p_open.
 *  Cancel just clears *p_open.
 * ====================================================================== */

static struct {
    char source_scene_path[512];
    char result_path[1024];
    bool dispatched;   /* dialog has been launched, waiting for callback */
    bool ready;        /* callback wrote a path */
    bool cancelled;    /* callback was cancelled / errored */
} s_save_as;

static std::string strip_scene_extension(const std::string &file_name)
{
    if (file_name.size() >= 11 && file_name.substr(file_name.size() - 11) == ".scene.json")
        return file_name.substr(0, file_name.size() - 11);
    if (file_name.size() >= 6 && file_name.substr(file_name.size() - 6) == ".scene")
        return file_name.substr(0, file_name.size() - 6);

    char stem[256];
    jce_path_stem(stem, sizeof(stem), file_name.c_str());
    return std::string(stem);
}

static void save_as_compute_default(char *out_path, size_t out_size)
{
    out_path[0] = '\0';

    const char *current_scene = jce_state_get_current_scene_path();
    char dir_buf[1024]  = {0};
    char name_buf[256]  = {0};

    if (current_scene && current_scene[0] != '\0') {
        jce_path_parent(dir_buf, sizeof(dir_buf), current_scene);
        char base[256];
        jce_path_basename(base, sizeof(base), current_scene);
        std::string stem = strip_scene_extension(base);
        snprintf(name_buf, sizeof(name_buf), "%s",
                 stem.empty() ? "Scene" : stem.c_str());
    } else {
        if (s_current_project_root[0] != '\0') {
            char tmp[1024];
            jce_path_join(tmp, sizeof(tmp), s_current_project_root, "assets");
            jce_path_join(dir_buf, sizeof(dir_buf), tmp, "scenes");
        } else if (!jce_fs_host_get_current_dir(dir_buf, sizeof(dir_buf))) {
            snprintf(dir_buf, sizeof(dir_buf), ".");
        }
        snprintf(name_buf, sizeof(name_buf), "Scene");
    }

    /* Always suggest a .scene extension. */
    std::string fname = name_buf;
    if (!is_scene_filename(fname)) fname += ".scene";
    jce_path_join(out_path, out_size, dir_buf, fname.c_str());
}

void jce_editor_dialog_save_as(bool *p_open)
{
    if (!p_open || !*p_open) return;

    /* Rising edge: launch the native save-file dialog exactly once. */
    if (!s_save_as.dispatched) {
        memset(s_save_as.result_path, 0, sizeof(s_save_as.result_path));
        s_save_as.ready     = false;
        s_save_as.cancelled = false;
        s_save_as.dispatched = true;

        char default_full[1024];
        save_as_compute_default(default_full, sizeof(default_full));

        save_file_dialog_async(jce_editor_i18n("menu.file.saveAs"),
                               default_full[0] ? default_full : NULL,
                               "Scene Files (*.scene *.scene.json);;All Files (*.*)",
                               s_save_as.result_path,
                               sizeof(s_save_as.result_path),
                               &s_save_as.ready,
                               &s_save_as.cancelled);
        return;
    }

    /* Cancelled or errored — close. */
    if (s_save_as.cancelled) {
        s_save_as.dispatched = false;
        s_save_as.cancelled  = false;
        s_save_as.ready      = false;
        *p_open = false;
        return;
    }

    /* Result not ready yet — keep waiting; nothing to draw. */
    if (!s_save_as.ready) return;

    /* Have a path — finalize the save on the main thread. */
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s", s_save_as.result_path);

    /* Append .scene if the user didn't type an extension. */
    char base[256];
    jce_path_basename(base, sizeof(base), out_path);
    if (!is_scene_filename(base)) {
        size_t cur = strlen(out_path);
        snprintf(out_path + cur, sizeof(out_path) - cur, ".scene");
    }

    /* Make sure parent dir exists. */
    char parent[1024];
    jce_path_parent(parent, sizeof(parent), out_path);
    if (parent[0] != '\0')
        jce_fs_host_create_directory(parent);

    if (jce_state_save_scene_file(out_path)) {
        snprintf(s_save_as.source_scene_path,
                 sizeof(s_save_as.source_scene_path), "%s", out_path);
        jce_editor_layout_request_focus_scene_view();
        jce_editor_console_log("Saved scene as: %s", out_path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "Save scene failed: %s", out_path);
    }

    /* Reset state and close. */
    s_save_as.dispatched = false;
    s_save_as.ready      = false;
    s_save_as.cancelled  = false;
    *p_open = false;
}

/* ======================================================================
 *  UNSAVED CHANGES DIALOG
 * ====================================================================== */

void jce_editor_dialog_unsaved_changes(bool *p_open, int *result)
{
    if (!p_open || !*p_open) return;

    const char *popup_id = "###UnsavedDialog";
    if (*p_open && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char _title[256];
    snprintf(_title, sizeof(_title), "%s%s",
             jce_editor_i18n("unsaved.title"), popup_id);

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 180), ImGuiCond_Appearing);
    ImGui::SetNextWindowViewport(vp->ID);

    if (!ImGui::BeginPopupModal(_title, p_open,
                      ImGuiWindowFlags_NoCollapse
                    | ImGuiWindowFlags_NoDocking
                    | ImGuiWindowFlags_NoResize)) {
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
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.30f, 0.30f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(jce_editor_i18n("unsaved.dontSave"), ImVec2(btn_w, 0))) {
        if (result) *result = 2;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("dialog.cancel"), ImVec2(btn_w, 0))) {
        if (result) *result = 3;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (result) *result = 3;
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }

    ImGui::EndPopup();
}
