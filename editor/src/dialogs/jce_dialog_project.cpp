/*
 * jce_dialog_project.cpp  New Project and Open Project dialog implementations.
 *
 * Shared helpers (folder picker, recent project validation) live here
 * as they are primarily used by project dialogs.  Scene dialogs are in
 * jce_dialog_scene.cpp.
 */

#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_str.h>
#include <jce/os/core/jce_thread.h>
#include <jce/renderer/jce_render_pipeline.h>

#include "jce_editor_dialogs_internal.h"
#include "core/jce_assetdb.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_game_l10n.h"
#include "core/jce_editor_project.h"
#include "core/jce_pak_key.h"
#include "core/jce_project_settings.h"
#include "core/jce_editor_config.h"

#include <vector>

/* ── Shared state ─────────────────────────────────────────────────── */

char s_current_project_root[512] = {0};
static char s_last_browse_folder[512] = {0};

void set_current_project_root(const char *path)
{
    if (!path || path[0] == '\0') {
        s_current_project_root[0] = '\0';
        jce_assetdb_set_root("");
        jce_editor_project_set_root(nullptr);
        jce_editor_gl10n_unload();
        jce_pak_key_install_process("");  /* clear the process key */
        return;
    }

    /* Normalize: callers occasionally hand us a path to jce_project.json
     * itself (Welcome dialog "Open File…" pick, drag-drop, recents that
     * captured the manifest path).  Treat that as the containing dir.
     * Also unify backslashes and drop a trailing separator so all
     * downstream string-concat code (build profiles, asset DB, etc.)
     * sees a canonical form. */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *q = buf; *q; ++q)
        if (*q == '\\') *q = '/';
    size_t L = strlen(buf);
    const char *suffix = "/jce_project.json";
    size_t slen = strlen(suffix);
    if (L >= slen) {
        const char *tail = buf + (L - slen);
        if (jce_strcasecmp(tail, suffix) == 0) { buf[L - slen] = '\0'; L -= slen; }
    } else if (L == slen - 1 &&
               jce_strcasecmp(buf, suffix + 1) == 0) {
        /* Pure "jce_project.json" with no parent — fall back to "." */
        buf[0] = '.'; buf[1] = '\0'; L = 1;
    }
    while (L > 1 && (buf[L - 1] == '/' || buf[L - 1] == '\\'))
        buf[--L] = '\0';

    snprintf(s_current_project_root, sizeof(s_current_project_root), "%s", buf);
    jce_assetdb_set_root(s_current_project_root);
    jce_editor_project_set_root(s_current_project_root);

    /* Install the project's asset-decryption key (if any) process-wide so
     * encrypted PAKs / bundles of this project open transparently in the
     * editor (Play mode, bundle preview, asset browser).  Clears any key
     * left over from a previously opened project. */
    jce_pak_key_install_process(s_current_project_root);

    /* Game string tables: load <root>/<source_assets>/i18n/*.json into the
     * L10n grid model and point jce_loc at it so UIText locale_key fields
     * resolve in the game view / Play preview. */
    {
        const JceProject *jp = jce_editor_project_get();
        const char *src = (jp && jp->source_assets && jp->source_assets[0])
                          ? jp->source_assets : "assets";
        jce_editor_gl10n_load(s_current_project_root, src);
    }

    /* Reload project settings for the newly opened root and write through
     * to live engine subsystems.  jce_project_settings_load now resolves
     * <root>/.jce/project-settings.json (see jce_project_settings.cpp), so
     * switching projects in-session picks up the right file instead of
     * keeping the previously opened project's cached settings. */
    {
        JceProjectSettings ps;
        jce_project_settings_load(&ps);   /* caches into the module snapshot */
        jce_project_settings_apply(&ps);

        /* One-time forward-migration of the legacy PROJECT-scoped external
         * editor paths into per-user Preferences > External Tools (they used
         * to live in project-settings.json).  Adopt only when the user has
         * no preference set yet, so a value entered in Preferences is never
         * overwritten.  After this, the project copy stops being written. */
        if (ps.editor.external_script_editor[0] || ps.editor.external_image_editor[0]) {
            JceEditorConfig cfg;
            jce_editor_config_load(&cfg);
            bool changed = false;
            if (!cfg.external_script_editor[0] && ps.editor.external_script_editor[0]) {
                snprintf(cfg.external_script_editor, sizeof(cfg.external_script_editor),
                         "%s", ps.editor.external_script_editor);
                changed = true;
            }
            if (!cfg.external_image_editor[0] && ps.editor.external_image_editor[0]) {
                snprintf(cfg.external_image_editor, sizeof(cfg.external_image_editor),
                         "%s", ps.editor.external_image_editor);
                changed = true;
            }
            if (changed) jce_editor_config_save(&cfg);
        }
    }
}

bool is_valid_project_dir(const char *path)
{
    if (!path || path[0] == '\0') return false;

    if (!jce_fs_host_exists_dir(path))
        return false;

    /* New canonical: jce_project.json.  Legacy: project.jce. */
    char project_json[1024];
    jce_path_join(project_json, sizeof(project_json), path, "jce_project.json");
    if (jce_fs_host_exists_file(project_json)) return true;

    char legacy[1024];
    jce_path_join(legacy, sizeof(legacy), path, "project.jce");
    return jce_fs_host_exists_file(legacy);
}

static bool resolve_project_root_path(const char *path, std::string *out_project_root)
{
    if (!path || path[0] == '\0' || !out_project_root)
        return false;

    char current[1024];
    snprintf(current, sizeof(current), "%s", path);
    
    if (!jce_fs_host_exists_file(current) && !jce_fs_host_exists_dir(current))
        return false;

    if (jce_fs_host_exists_file(current)) {
        jce_path_parent(current, sizeof(current), current);
    }

    while (current[0] != '\0') {
        char project_json[1024];
        jce_path_join(project_json, sizeof(project_json), current, "jce_project.json");
        if (jce_fs_host_exists_file(project_json)) {
            *out_project_root = current;
            return true;
        }

        char legacy[1024];
        jce_path_join(legacy, sizeof(legacy), current, "project.jce");
        if (jce_fs_host_exists_file(legacy)) {
            *out_project_root = current;
            return true;
        }

        char parent[1024];
        if (!jce_path_parent(parent, sizeof(parent), current))
            break;
            
        if (parent[0] == '\0' || strcmp(parent, current) == 0)
            break;
            
        snprintf(current, sizeof(current), "%s", parent);
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

/* ── Folder picker — main-thread-safe marshalling ─────────────────────
 *
 * Background:
 *   On Windows, SDL3's SDL_ShowOpenFolderDialog runs the COM dialog on a
 *   worker thread named "SDL_Windows_ShowFolderDialog" and fires the result
 *   callback on that same worker thread.  The previous implementation wrote
 *   the picked path directly into editor-owned char buffers (e.g.
 *   s_save_as.save_location) from the worker thread, racing with ImGui
 *   InputText reads/writes on the main thread.  This produced sporadic
 *   ACCESS_VIOLATION crashes during heavy editing → save sequences (the
 *   stripped-symbol stack misleadingly resolved to "cJSON_malloc" because
 *   dladdr fell back to the nearest export — see issue #5).
 *
 * Fix:
 *   The host-dialog callback now copies the path into a heap-owned struct
 *   and pushes it onto a mutex-protected queue.  The main thread drains the
 *   queue once per frame in jce_editor_dialogs_pump_pending() and applies
 *   the snprintf updates safely.  Everything that touches editor state is
 *   on the main thread; the worker thread only does atomic enqueue + free.
 */
namespace {

struct FolderPickRequest {
    /* Output targets — file-scope statics in caller, valid for app
       lifetime.  We never free them; we only write through them on the
       main thread. */
    char  *primary;
    size_t primary_size;
    char  *secondary;
    size_t secondary_size;
    bool  *ready_flag;
    bool  *cancelled_flag;

    /* Result captured on worker thread, applied on main thread. */
    bool        completed;
    bool        cancelled;
    std::string path;
};

JceMutex *g_pending_mu = nullptr;
std::vector<FolderPickRequest *> g_pending_pick_requests;

JceMutex *get_pending_mu()
{
    if (!g_pending_mu) g_pending_mu = jce_mutex_create();
    return g_pending_mu;
}

void folder_pick_callback_thread_safe(void *user,
                                      JceDialogResult result,
                                      const char *path)
{
    FolderPickRequest *req = (FolderPickRequest *)user;
    if (!req) return;

    /* WORKER THREAD — do not touch editor state directly. */
    if (result == JCE_DIALOG_OK && path && path[0] != '\0') {
        req->completed = true;
        req->cancelled = false;
        try {
            req->path.assign(path);
        } catch (...) {
            req->path.clear();
            req->cancelled = true;  /* treat OOM as cancel */
            req->completed = false;
        }
    } else {
        req->cancelled = true;
        req->completed = false;
    }

    {
        JceMutex *mu = get_pending_mu();
        jce_mutex_lock(mu);
        g_pending_pick_requests.push_back(req);
        jce_mutex_unlock(mu);
    }
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
    req->completed      = false;
    req->cancelled      = false;

    const char *initial = NULL;
    if (default_path && default_path[0] != '\0')
        initial = default_path;
    else if (s_last_browse_folder[0] != '\0')
        initial = s_last_browse_folder;
    else if (primary_out[0] != '\0')
        initial = primary_out;

    jce_host_dialog_pick_folder(title, initial,
                                folder_pick_callback_thread_safe, req);
}

/* ------------------------------------------------------------------
 *  Save-file dialog (more stable on Windows than the folder picker —
 *  uses SDL_ShowSaveFileDialog).  Reuses the same queue/pump as the
 *  folder picker so writes back into editor state happen on the main
 *  thread.
 * ------------------------------------------------------------------ */
void save_file_dialog_async(const char *title,
                            const char *default_path,
                            const char *filters,
                            char *primary_out, size_t primary_size,
                            bool *ready_flag,
                            bool *cancelled_flag)
{
    if (!primary_out || primary_size == 0) return;

    FolderPickRequest *req = new FolderPickRequest{};
    req->primary        = primary_out;
    req->primary_size   = primary_size;
    req->secondary      = NULL;
    req->secondary_size = 0;
    req->ready_flag     = ready_flag;
    req->cancelled_flag = cancelled_flag;
    req->completed      = false;
    req->cancelled      = false;

    const char *initial = NULL;
    if (default_path && default_path[0] != '\0')
        initial = default_path;
    else if (s_last_browse_folder[0] != '\0')
        initial = s_last_browse_folder;
    else if (primary_out[0] != '\0')
        initial = primary_out;

    jce_host_dialog_save_file(title, initial, filters,
                              folder_pick_callback_thread_safe, req);
}

void open_file_dialog_async(const char *title,
                            const char *default_path,
                            const char *filters,
                            char *primary_out, size_t primary_size,
                            bool *ready_flag,
                            bool *cancelled_flag)
{
    if (!primary_out || primary_size == 0) return;

    FolderPickRequest *req = new FolderPickRequest{};
    req->primary        = primary_out;
    req->primary_size   = primary_size;
    req->secondary      = NULL;
    req->secondary_size = 0;
    req->ready_flag     = ready_flag;
    req->cancelled_flag = cancelled_flag;
    req->completed      = false;
    req->cancelled      = false;

    const char *initial = NULL;
    if (default_path && default_path[0] != '\0')
        initial = default_path;
    else if (s_last_browse_folder[0] != '\0')
        initial = s_last_browse_folder;
    else if (primary_out[0] != '\0')
        initial = primary_out;

    jce_host_dialog_pick_file(title, initial, filters,
                              folder_pick_callback_thread_safe, req);
}

void jce_editor_dialogs_pump_pending(void)
{
    /* Move pending requests under the lock, then process outside the lock
       to keep critical section minimal. */
    std::vector<FolderPickRequest *> pending;
    {
        JceMutex *mu = get_pending_mu();
        jce_mutex_lock(mu);
        if (g_pending_pick_requests.empty()) { jce_mutex_unlock(mu); return; }
        pending.swap(g_pending_pick_requests);
        jce_mutex_unlock(mu);
    }

    for (FolderPickRequest *req : pending) {
        if (!req) continue;
        if (req->completed && !req->path.empty()) {
            const char *p = req->path.c_str();
            if (req->primary && req->primary_size > 0)
                snprintf(req->primary, req->primary_size, "%s", p);
            if (req->secondary && req->secondary_size > 0)
                snprintf(req->secondary, req->secondary_size, "%s", p);
            snprintf(s_last_browse_folder, sizeof(s_last_browse_folder),
                     "%s", p);
            if (req->ready_flag)     *req->ready_flag     = true;
        } else {
            if (req->cancelled_flag) *req->cancelled_flag = true;
        }
        delete req;
    }
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
            char project_dir[1024];
            jce_path_join(project_dir, sizeof(project_dir),
                         s_new_project.project_location,
                         s_new_project.project_name);
            
            /* Check if exists and not empty */
            bool dir_exists = jce_fs_host_exists_dir(project_dir);
            bool has_files = false;
            if (dir_exists) {
                struct CheckCtx { bool has_any; };
                CheckCtx chk = { false };
                auto cb = [](const char *, bool, void *ud) -> bool {
                    static_cast<CheckCtx*>(ud)->has_any = true;
                    return false;
                };
                jce_fs_host_list_dir(project_dir, cb, &chk);
                has_files = chk.has_any;
            }
            
            if (dir_exists && has_files) {
                snprintf(s_new_project.error_msg, sizeof(s_new_project.error_msg), "%s",
                         jce_editor_i18n("newProject.errorExists"));
            } else {
                /* Use the engine's template generator: lays down
                 * jce_project.json + CMakeLists.txt + src/main.c +
                 * assets/.gitkeep so the project is buildable out of
                 * the box.  (Engine API — works the same in headless
                 * tooling later.) */
                JceProjectTemplate tpl = (s_new_project.project_type == 1)
                    ? JCE_PROJECT_TEMPLATE_BASIC_2D
                    : JCE_PROJECT_TEMPLATE_BASIC_3D;
                char tpl_err[256] = {0};
                if (!jce_project_create_from_template(project_dir,
                                                     s_new_project.project_name,
                                                     tpl,
                                                     tpl_err, sizeof tpl_err)) {
                    snprintf(s_new_project.error_msg,
                             sizeof(s_new_project.error_msg),
                             "%s", tpl_err[0] ? tpl_err
                                  : jce_editor_i18n("newProject.errorExists"));
                } else {
                    /* Optional asset sub-trees that are convenient to
                     * pre-create for the inspector / asset browser. */
                    char sub_path[1024];
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "assets/scenes");
                    jce_fs_host_create_directory(sub_path);
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "assets/textures");
                    jce_fs_host_create_directory(sub_path);
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "assets/models");
                    jce_fs_host_create_directory(sub_path);
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "assets/audio");
                    jce_fs_host_create_directory(sub_path);
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "assets/scripts");
                    jce_fs_host_create_directory(sub_path);
                    jce_path_join(sub_path, sizeof(sub_path), project_dir, "build");
                    jce_fs_host_create_directory(sub_path);

                    /* Write Settings/RenderPipeline.rp.json with MID preset so
                     * the built exe doesn't auto-detect HIGH tier and enable
                     * SSR/volfog/TAA at full resolution.  The file is user-editable
                     * and deployed next to the exe by the CMake POST_BUILD step. */
                    {
                        char settings_dir[1024];
                        char rp_path[1024];
                        jce_path_join(settings_dir, sizeof(settings_dir), project_dir, "Settings");
                        jce_fs_host_create_directory(settings_dir);
                        jce_path_join(rp_path, sizeof(rp_path), settings_dir, "RenderPipeline.rp.json");
                        JceRenderPipelineDesc rp_desc;
                        jce_render_pipeline_preset_mid(&rp_desc);
                        jce_render_pipeline_save(rp_path, &rp_desc);
                    }

                    /* Add to recent projects. */
                    JceEditorConfig ecfg;
                    jce_editor_config_load(&ecfg);
                    jce_editor_config_add_recent(&ecfg, project_dir);
                    ecfg.last_project[0] = '\0';
                    jce_editor_config_save(&ecfg);

                    /* Set the asset browser root to the new project. */
                    jce_editor_assets_set_project(project_dir);
                    set_current_project_root(project_dir);
                    jce_editor_layout_request_focus_scene_view();

                    jce_editor_console_log("Created project: %s at %s",
                                           s_new_project.project_name,
                                           project_dir);
                    ImGui::CloseCurrentPopup();
                    *p_open = false;
                }
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
    char base_path[1024];
    if (jce_fs_host_get_current_dir(base_path, sizeof(base_path))) {
        snprintf(s_open_project.browse_path, sizeof(s_open_project.browse_path),
                 "%s", base_path);
    } else {
        snprintf(s_open_project.browse_path, sizeof(s_open_project.browse_path), ".");
    }
    s_open_project.initialized = true;
}

static void browse_refresh_entries(void)
{
    s_open_project.browse_entries.clear();
    
    /* Add parent directory entry. */
    char parent[1024];
    if (jce_path_parent(parent, sizeof(parent), s_open_project.browse_path) &&
        parent[0] != '\0') {
        s_open_project.browse_entries.push_back("..");
    }

    std::vector<std::string> dirs;
    
    struct ListCtx {
        std::vector<std::string> *dirs;
    } ctx;
    ctx.dirs = &dirs;
    
    auto cb = [](const char *name, bool is_dir, void *ud) -> bool {
        if (is_dir) {
            static_cast<ListCtx*>(ud)->dirs->push_back(name);
        }
        return true;
    };
    
    jce_fs_host_list_dir(s_open_project.browse_path, cb, &ctx);
    
    std::sort(dirs.begin(), dirs.end());
    for (auto &d : dirs)
        s_open_project.browse_entries.push_back(d);
    
    s_open_project.browse_refresh = false;
}

void jce_editor_dialog_open_project_set_path(const char *path)
{
    open_project_ensure_init();
    if (path && path[0]) {
        snprintf(s_open_project.manual_path,
                 sizeof(s_open_project.manual_path), "%s", path);
    }
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
            jce_editor::help_tip(jce_editor_i18n("openProject.removeTooltip"));
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
        /* Async dispatch.  Flags are written by the shared main-thread
           pump when the native dialog returns; we observe them in
           subsequent frames. */
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
                    char next[1024];
                    if (strcmp(name, "..") == 0) {
                        jce_path_parent(next, sizeof(next), s_open_project.browse_path);
                    } else {
                        jce_path_join(next, sizeof(next), s_open_project.browse_path, name);
                    }
                    snprintf(s_open_project.browse_path,
                             sizeof(s_open_project.browse_path),
                             "%s", next);
                    s_open_project.browse_refresh = true;
                } else {
                    /* Single click: set as selected path. */
                    char selected[1024];
                    if (strcmp(name, "..") == 0)
                        jce_path_parent(selected, sizeof(selected), s_open_project.browse_path);
                    else
                        jce_path_join(selected, sizeof(selected), s_open_project.browse_path, name);
                    snprintf(s_open_project.manual_path,
                             sizeof(s_open_project.manual_path),
                             "%s", selected);
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
        if (ImGui::Button(jce_editor_i18n_id("newProject.button.closeBrowser", "op_browse_close"), ImVec2(120, 0))) {
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
                std::string project_root;
                if (resolve_project_root_path(path, &project_root)) {
                    if (!s_open_project.cfg_loaded)
                        jce_editor_config_load(&s_open_project.cfg);
                    jce_editor_config_add_recent(&s_open_project.cfg, project_root.c_str());
                    s_open_project.cfg.last_project[0] = '\0';
                    jce_editor_config_save(&s_open_project.cfg);

                    /* Set the asset browser root to the project directory. */
                    jce_editor_assets_set_project(project_root.c_str());
                    set_current_project_root(project_root.c_str());
                    snprintf(s_open_project.manual_path,
                             sizeof(s_open_project.manual_path), "%s",
                             project_root.c_str());
                    jce_editor_layout_request_focus_scene_view();

                    jce_editor_console_log("Opened project: %s", project_root.c_str());
                    *p_open = false;
                } else {
                    if (!jce_fs_host_exists_file(path) && !jce_fs_host_exists_dir(path)) {
                        snprintf(s_open_project.error_msg,
                                 sizeof(s_open_project.error_msg), "%s",
                                 jce_editor_i18n("openProject.errorNotExist"));
                    } else {
                        snprintf(s_open_project.error_msg,
                                 sizeof(s_open_project.error_msg), "%s",
                                     jce_editor_i18n("openProject.errorInvalid"));
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
