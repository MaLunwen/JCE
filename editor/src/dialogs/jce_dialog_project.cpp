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

/* ── Async folder picker ──────────────────────────────────────────── */
/*
 * The host folder dialog is asynchronous: the SDL callback fires later
 * (on the editor UI thread).  We marshal the result into the caller-
 * supplied output buffers + flags so ImGui code can poll on the next
 * frame instead of blocking.
 */

namespace {

struct FolderPickRequest {
    char  *primary;
    size_t primary_size;
    char  *secondary;
    size_t secondary_size;
    bool  *ready_flag;
    bool  *cancelled_flag;
};

void folder_pick_callback(void *user, JceDialogResult result, const char *path)
{
    FolderPickRequest *req = (FolderPickRequest *)user;
    if (!req) return;

    if (result == JCE_DIALOG_OK && path && path[0] != '\0') {
        if (req->primary && req->primary_size > 0)
            snprintf(req->primary, req->primary_size, "%s", path);
        if (req->secondary && req->secondary_size > 0)
            snprintf(req->secondary, req->secondary_size, "%s", path);
        snprintf(s_last_browse_folder, sizeof(s_last_browse_folder), "%s", path);
        if (req->ready_flag)     *req->ready_flag     = true;
    } else {
        if (req->cancelled_flag) *req->cancelled_flag = true;
    }
    delete req;
}

} // namespace

void pick_folder_dialog_async(const char *title,
                              const char *default_path,
                              char *primary_out, size_t primary_size,
                              char *secondary_out, size_t secondary_size,
                              bool *ready_flag,
                              bool *cancelled_flag)
{
    if (!primary_out || primary_size == 0) return;

    FolderPickRequest *req = new FolderPickRequest{};
    req->primary        = primary_out;
    req->primary_size   = primary_size;
    req->secondary      = secondary_out;
    req->secondary_size = secondary_size;
    req->ready_flag     = ready_flag;
    req->cancelled_flag = cancelled_flag;

    const char *initial = NULL;
    if (default_path && default_path[0] != '\0')
        initial = default_path;
    else if (s_last_browse_folder[0] != '\0')
        initial = s_last_browse_folder;
    else if (primary_out[0] != '\0')
        initial = primary_out;

    jce_host_dialog_pick_folder(title, initial, folder_pick_callback, req);
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

    const char *popup_id = "###NewProject";
    if (*p_open && !ImGui::IsPopupOpen(popup_id))
        ImGui::OpenPopup(popup_id);

    char _title[256];
    snprintf(_title, sizeof(_title), "%s%s",
             jce_editor_i18n("newProject.title"), popup_id);

    ImGui::SetNextWindowSize(ImVec2(550, 350), ImGuiCond_FirstUseEver);
    if (!ImGui::BeginPopupModal(_title, p_open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking)) {
        return;
    }

    if (ImGui::IsWindowAppearing())
        s_new_project.error_msg[0] = '\0';

    char _lbl[256];

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
        pick_folder_dialog_async(jce_editor_i18n("newProject.selectLocation"),
                                 NULL,
                                 s_new_project.project_location,
                                 sizeof(s_new_project.project_location),
                                 NULL, 0, NULL, NULL);
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
                    {
                        char proj_buf[512];
                        int proj_len = snprintf(proj_buf, sizeof(proj_buf),
                            "{\n"
                            "    \"name\": \"%s\",\n"
                            "    \"type\": \"%s\",\n"
                            "    \"version\": \"1.0\",\n"
                            "    \"engineVersion\": \"0.1.0\",\n"
                            "    \"editorVersion\": \"0.1.0\"\n"
                            "}\n",
                            s_new_project.project_name,
                            s_new_project.project_type == 0 ? "3D" : "2D");
                        if (proj_len > 0)
                            ed_write_file(proj_file.string().c_str(), proj_buf, (size_t)proj_len);
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
                    ImGui::CloseCurrentPopup();
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
        ImGui::CloseCurrentPopup();
        *p_open = false;
    }

    ImGui::EndPopup();
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
    /* Async folder-picker outcomes (written from SDL UI thread). */
    bool pick_ready;
    bool pick_cancelled;
    bool initialized;
    bool request_open;  /* Set by double-click on a recent project. */
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
    s_open_project.pick_ready      = false;
    s_open_project.pick_cancelled  = false;
    s_open_project.request_open    = false;
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
                              ImGuiSelectableFlags_AllowDoubleClick,
                              ImVec2(ImGui::GetContentRegionAvail().x - 30, 0))) {
            s_open_project.selected_recent = i;
            snprintf(s_open_project.manual_path,
                     sizeof(s_open_project.manual_path),
                     "%s", s_open_project.cfg.recent_projects[i]);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                s_open_project.request_open = true;
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
        /* Async dispatch.  Flags are written from the SDL UI thread when
           the dialog returns; we observe them in subsequent frames. */
        s_open_project.pick_ready     = false;
        s_open_project.pick_cancelled = false;
        pick_folder_dialog_async(jce_editor_i18n("openProject.selectFolder"),
                                 NULL,
                                 s_open_project.manual_path,
                                 sizeof(s_open_project.manual_path),
                                 s_open_project.browse_path,
                                 sizeof(s_open_project.browse_path),
                                 &s_open_project.pick_ready,
                                 &s_open_project.pick_cancelled);
    }

    /* React to async folder-picker outcome from a previous frame. */
    if (s_open_project.pick_ready) {
        s_open_project.pick_ready  = false;
        s_open_project.browse_open = false;
    } else if (s_open_project.pick_cancelled) {
        s_open_project.pick_cancelled = false;
        s_open_project.browse_open    = true;
        s_open_project.browse_refresh = true;
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
        bool dbl_open = s_open_project.request_open && has_path;
        s_open_project.request_open = false;
        if (ImGui::Button(_lbl, ImVec2(btn_w, 0))
            || (enter_pressed && has_path)
            || dbl_open) {
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
