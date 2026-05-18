/*
 * jce_dialog_bundles.cpp  Build Scene Bundles dialog.
 *
 * Calls jce_bundle_pack_run() in-process on a background JceThread so
 * the editor stays a single executable (no jce_bundle_pack.exe spawn).
 * Worker log lines are pushed onto a mutex-protected queue and drained
 * on the main thread each frame into the editor console panel.  After
 * a successful build the resulting catalog is loaded for summary view.
 */

#include "jce_editor_dialogs_internal.h"
#include "jce_path_input.h"

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/resource/jce_bundle_deps.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_pack.h>
#include "scene/jce_asset_path_index.h"

#include <cjson/cJSON.h>
}

#include "core/jce_editor_state.h"

#include <imgui.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {

/* ── Default project layout (convention) ──────────────────────────────
 *   <project>/scenes/                  *.scene.json source
 *   <project>/resources/assets/        meshes/textures/audio/... etc
 *   <project>/bundles/                 .jbundle output + bundle_catalog.json
 *   <project>/bundles/.prev/           previous catalog (for incremental)
 * Anyone using these defaults can just hit "Build" without configuration.
 */
#define JCE_BUNDLE_DEFAULT_SCENES_SUBDIR    "scenes"
#define JCE_BUNDLE_DEFAULT_RESOURCE_SUBDIR  "resources/assets"
#define JCE_BUNDLE_DEFAULT_OUT_SUBDIR       "bundles"
#define JCE_BUNDLE_DEFAULT_PREV_SUBDIR      "bundles/.prev"

/* True i18n lookup for this dialog.  Falls back to the English literal
 * if the locale JSON doesn't define the key — so adding a new locale
 * just means dropping more "dialog.bundles.*" entries into its JSON. */
static const char *BL(const char *suffix, const char *en_fallback)
{
    char key[160];
    snprintf(key, sizeof(key), "dialog.bundles.%s", suffix);
    return jce_editor_i18n_or(key, en_fallback);
}

struct BundleSummary {
    std::string id;
    std::string kind;
    std::string scene_path;
    uint64_t    size_bytes = 0;
    int         dep_count  = 0;
    int         asset_count = 0;
};

struct LogLine {
    int         level;   /* JceBundlePackLogLevel */
    std::string text;
};

struct DialogState {
    /* Pack mode: 0 = Project, 1 = Selected Scenes, 2 = Single Scene. */
    int mode = 0;

    /* Inputs. */
    char project_root [512] {};   /* primary input — drives everything */
    char scenes_dir   [512] {};
    char resource_root[512] {};
    char out_dir      [512] {};
    char prev_catalog [512] {};
    char shared_id    [128] {};
    int  shared_threshold = 2;
    int  zstd_level       = 3;

    /* Selected / Single Scene mode. */
    std::vector<std::string> scene_files;
    char scene_file_input[512] {};
    char single_bundle_id[128] {};
    bool auto_resource_root = true;

    /* Worker-thread state (engine primitives — no STL threading).
     * `running` / `finished` / `last_exit` are bool-as-int / int counters.
     * Lazily created in initialise_defaults_from_project(). */
    JceThread *   worker     = nullptr;
    JceAtomicI32 *running    = nullptr;   /* 0/1 */
    JceAtomicI32 *finished   = nullptr;   /* 0/1; flipped by worker on exit */
    JceAtomicI32 *last_exit  = nullptr;
    JceMutex *    log_mtx    = nullptr;
    std::deque<LogLine>  log_queue;         /* drained on main thread  */

    /* Owned input snapshots so the worker reads stable strings. */
    std::string project_root_owned;
    std::string scenes_dir_owned;
    std::string resource_root_owned;
    std::string out_dir_owned;
    std::string prev_catalog_owned;
    std::string shared_id_owned;
    std::vector<std::string>  scene_files_owned;
    std::vector<const char *> scene_file_ptrs_owned;
    std::string single_bundle_id_owned;
    int  mode_owned = 0;
    bool auto_resource_root_owned = false;

    std::string last_status;

    /* Result summary. */
    std::vector<BundleSummary> summary;

    bool inited = false;
};

static DialogState g;

/* Set by jce_editor_dialog_bundles_open_for_current_scene(); the next
 * call into the dialog reads & clears it to (re)configure for single-
 * scene mode targeting whatever path the caller supplied. */
static std::string g_pending_single_scene;

extern "C" bool jce_editor_dialog_bundles_open_for_current_scene(void)
{
    const char *spath = jce_state_get_current_scene_path();
    if (!spath || !spath[0]) {
        jce_editor_console_log("[bundle] no current scene to pack");
        return false;
    }
    g_pending_single_scene = spath;
    return true;
}

static void apply_default_layout_paths()
{
    const char *proj_root = s_current_project_root;
    if (!proj_root || !proj_root[0]) return;
    /* Primary mode: only project_root is needed; the engine derives
     * the rest.  Filling the explicit fields just makes them visible
     * for users who want to override. */
    snprintf(g.project_root,  sizeof(g.project_root),  "%s", proj_root);
    snprintf(g.out_dir,       sizeof(g.out_dir),       "%s/.bundles",
             proj_root);
    snprintf(g.prev_catalog,  sizeof(g.prev_catalog),  "%s/.bundles/.prev/%s",
             proj_root, JCE_BUNDLE_CATALOG_NAME);
    /* scenes_dir / resource_root left empty so engine uses project_root. */
}

static bool initialise_default_layout_dirs()
{
    const char *proj_root = s_current_project_root;
    if (!proj_root || !proj_root[0]) return false;

    char tmp[1024];
    bool ok = true;
    const char *subs[] = { ".bundles", ".bundles/.prev", nullptr };
    for (size_t i = 0; subs[i]; ++i) {
        snprintf(tmp, sizeof(tmp), "%s/%s", proj_root, subs[i]);
        if (!jce_fs_host_create_directory(tmp)) ok = false;
    }
    apply_default_layout_paths();
    return ok;
}

static void initialise_defaults_from_project()
{
    if (!g.inited) {
        g.inited = true;
        snprintf(g.shared_id, sizeof(g.shared_id), "%s",
                 JCE_BUNDLE_SHARED_DEFAULT_ID);
        /* One-shot lazy creation of engine sync primitives. */
        if (!g.running)   g.running   = jce_atomic_i32_create(0);
        if (!g.finished)  g.finished  = jce_atomic_i32_create(0);
        if (!g.last_exit) g.last_exit = jce_atomic_i32_create(0);
        if (!g.log_mtx)   g.log_mtx   = jce_mutex_create();
    }
    /* Auto-fill any path field that is still empty whenever a project
     * root is known. */
    const char *proj_root = s_current_project_root;
    if (!proj_root || !proj_root[0]) return;
    if (g.project_root[0] == '\0')
        snprintf(g.project_root, sizeof(g.project_root), "%s", proj_root);
    if (g.out_dir[0] == '\0')
        snprintf(g.out_dir, sizeof(g.out_dir), "%s/.bundles", proj_root);
    if (g.prev_catalog[0] == '\0')
        snprintf(g.prev_catalog, sizeof(g.prev_catalog), "%s/.bundles/.prev/%s",
                 proj_root, JCE_BUNDLE_CATALOG_NAME);
}

/* Worker-thread log sink — push to queue; main thread drains. */
/* Bridge bundle packer's external resolver hook to the editor's project-wide
 * asset path index so any indexed file (incl. external roots like
 * `halloween-test/`) gets pulled in even when no VFS is mounted. */
static bool editor_pack_resolve(const char *vpath, char *out, size_t outsz,
                                void * /*user*/)
{
    if (!vpath || !out || outsz == 0) return false;
    return jce_asset_path_index_lookup(vpath, out, (int)outsz);
}


static void worker_log_sink(JceBundlePackLogLevel level, const char *msg,
                            void *user)
{
    DialogState *st = static_cast<DialogState *>(user);
    if (!st || !msg) return;
    LogLine line;
    line.level = static_cast<int>(level);
    line.text  = msg;
    jce_mutex_lock(st->log_mtx);
    st->log_queue.emplace_back(std::move(line));
    jce_mutex_unlock(st->log_mtx);
}

static void drain_log_queue()
{
    std::deque<LogLine> local;
    jce_mutex_lock(g.log_mtx);
    local.swap(g.log_queue);
    jce_mutex_unlock(g.log_mtx);
    for (const auto &line : local) {
        JceConsoleLevel lvl = JCE_CONSOLE_INFO;
        switch (line.level) {
            case JCE_BUNDLE_PACK_LOG_WARNING: lvl = JCE_CONSOLE_WARNING; break;
            case JCE_BUNDLE_PACK_LOG_ERROR:   lvl = JCE_CONSOLE_ERROR;   break;
            case JCE_BUNDLE_PACK_LOG_SUCCESS: lvl = JCE_CONSOLE_INFO;    break;
            default:                          lvl = JCE_CONSOLE_INFO;    break;
        }
        jce_editor_console_log_level(lvl, "[bundle] %s", line.text.c_str());
    }
}

/* Worker entry — runs on a dedicated JceThread. Reads snapshotted inputs
 * from the global state set up by start_build() before spawning us. */
static void bundle_pack_worker_main(void * /*user*/)
{
    JceBundlePackOptions opts{};
    const bool selected_or_single =
        g.mode_owned == 1 || g.mode_owned == 2;
    opts.project_root     = (selected_or_single ||
                             g.project_root_owned.empty())
                                ? nullptr
                                : g.project_root_owned.c_str();
    opts.scenes_dir       = (selected_or_single ||
                             g.scenes_dir_owned.empty())
                                ? nullptr
                                : g.scenes_dir_owned.c_str();
    opts.resource_root    = g.resource_root_owned.empty()
                                ? nullptr
                                : g.resource_root_owned.c_str();
    opts.out_dir          = g.out_dir_owned.empty()
                                ? nullptr
                                : g.out_dir_owned.c_str();
    opts.prev_catalog     = (g.mode_owned == 2 ||
                             g.prev_catalog_owned.empty())
                                ? nullptr
                                : g.prev_catalog_owned.c_str();
    opts.shared_id        = g.shared_id_owned.empty()
                                ? nullptr
                                : g.shared_id_owned.c_str();
    opts.shared_threshold = g.shared_threshold;
    opts.zstd_level       = g.zstd_level;
    opts.catalog_version  = 0;
    opts.quiet            = false;
    if (selected_or_single && !g.scene_file_ptrs_owned.empty()) {
        opts.scene_files      = g.scene_file_ptrs_owned.data();
        opts.scene_file_count = g.scene_file_ptrs_owned.size();
    }
    opts.auto_resource_root = g.auto_resource_root_owned;
    opts.single_file_mode   = (g.mode_owned == 2);
    opts.single_bundle_id   = g.single_bundle_id_owned.empty()
                                ? nullptr
                                : g.single_bundle_id_owned.c_str();
    opts.resolve_fn         = &editor_pack_resolve;
    opts.resolve_user       = nullptr;

    int rc = jce_bundle_pack_run(&opts, worker_log_sink, &g);
    jce_atomic_i32_store(g.last_exit, rc);
    jce_atomic_i32_store(g.running, 0);
    jce_atomic_i32_store(g.finished, 1);
}

static void start_build()
{
    if (jce_atomic_i32_load(g.running)) return;

    /* Snapshot UI inputs so the worker thread can read stable storage. */
    g.project_root_owned  = g.project_root;
    g.scenes_dir_owned    = g.scenes_dir;
    g.resource_root_owned = g.resource_root;
    g.out_dir_owned       = g.out_dir;
    g.prev_catalog_owned  = g.prev_catalog;
    g.shared_id_owned     = g.shared_id;
    g.mode_owned                = g.mode;
    g.auto_resource_root_owned  = g.auto_resource_root;
    g.single_bundle_id_owned    = g.single_bundle_id;
    g.scene_files_owned         = g.scene_files;
    g.scene_file_ptrs_owned.clear();
    g.scene_file_ptrs_owned.reserve(g.scene_files_owned.size());
    for (auto &s : g.scene_files_owned)
        g.scene_file_ptrs_owned.push_back(s.c_str());

    g.summary.clear();
    g.last_status.clear();
    jce_atomic_i32_store(g.last_exit, 0);
    jce_atomic_i32_store(g.finished, 0);
    jce_atomic_i32_store(g.running, 1);

    /* Reap any prior thread handle. */
    if (g.worker) {
        jce_thread_join(g.worker);
        g.worker = nullptr;
    }

    g.worker = jce_thread_create(&bundle_pack_worker_main, nullptr,
                                  "jce-bundle-pack");
    if (!g.worker) {
        /* Spawn failure: roll back atomic flags and surface to UI. */
        jce_atomic_i32_store(g.running, 0);
        jce_atomic_i32_store(g.finished, 1);
        jce_atomic_i32_store(g.last_exit, -1);
        g.last_status = BL("status.failed_fmt", "build failed (rc=%d)");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "[bundle] worker thread spawn failed");
        return;
    }

    g.last_status = BL("status.running", "running...");
    jce_editor_console_log("[bundle] %s",
                            BL("status.started", "build started (in-process)"));
}

static void load_summary_from_catalog()
{
    g.summary.clear();
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g.out_dir, JCE_BUNDLE_CATALOG_NAME);
    uint64_t sz = 0;
    void *vbuf = jce_fs_host_read_all(path, &sz);
    if (!vbuf || sz == 0) {
        if (vbuf) jce_fs_buffer_free(vbuf);
        return;
    }
    std::string text(static_cast<const char *>(vbuf), static_cast<size_t>(sz));
    jce_fs_buffer_free(vbuf);

    cJSON *root = cJSON_ParseWithLength(text.data(), text.size());
    if (!root) return;
    cJSON *bundles = cJSON_GetObjectItemCaseSensitive(root,
                            JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (bundles && cJSON_IsObject(bundles)) {
        cJSON *e = nullptr;
        cJSON_ArrayForEach(e, bundles) {
            BundleSummary bs;
            bs.id = e->string ? e->string : "?";
            cJSON *jk = cJSON_GetObjectItemCaseSensitive(e, JCE_BUNDLE_CATALOG_KEY_KIND);
            cJSON *js = cJSON_GetObjectItemCaseSensitive(e, JCE_BUNDLE_CATALOG_KEY_SCENE);
            cJSON *jz = cJSON_GetObjectItemCaseSensitive(e, JCE_BUNDLE_CATALOG_KEY_SIZE);
            cJSON *jd = cJSON_GetObjectItemCaseSensitive(e, JCE_BUNDLE_CATALOG_KEY_DEPS);
            if (jk && cJSON_IsString(jk)) bs.kind = jk->valuestring;
            if (js && cJSON_IsString(js)) bs.scene_path = js->valuestring;
            if (jz) bs.size_bytes = static_cast<uint64_t>(jz->valuedouble);
            if (jd && cJSON_IsArray(jd)) bs.dep_count = cJSON_GetArraySize(jd);
            g.summary.push_back(std::move(bs));
        }
    }
    cJSON_Delete(root);
}

} /* namespace */

extern "C" void jce_editor_dialog_bundles(bool *p_open)
{
    if (!p_open) return;
    if (!g_pending_single_scene.empty()) {
        g.mode = 2;
        g.scene_files.clear();
        g.scene_files.emplace_back(g_pending_single_scene);
        g.auto_resource_root = true;
        g_pending_single_scene.clear();
        *p_open = true;
    }
    if (!*p_open) return;
    initialise_defaults_from_project();

    /* Always drain log queue & finalise completed worker, even if
     * the dialog window is currently closed elsewhere. */
    drain_log_queue();
    if (g.finished && jce_atomic_i32_exchange(g.finished, 0)) {
        if (g.worker) { jce_thread_join(g.worker); g.worker = nullptr; }
        int rc = jce_atomic_i32_load(g.last_exit);
        if (rc == 0) {
            g.last_status = BL("status.succeeded", "build succeeded");
            load_summary_from_catalog();
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp),
                     BL("status.failed_fmt", "build failed (rc=%d)"), rc);
            g.last_status = tmp;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(680, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(BL("title", "Build Scene Bundles"), p_open,
                       ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted(BL("desc",
        "Pack scene-level asset bundles for incremental shipping.\n"
        "Assets used by 2+ scenes auto-move to a shared bundle."));
    ImGui::Separator();

    /* ── Pack mode (always visible at the top) ───────────────────── */
    ImGui::TextUnformatted(BL("mode.label", "Pack mode:"));
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.project", "Project"),             &g.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.selected", "Selected scenes"), &g.mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.single", "Single scene"),    &g.mode, 2);
    const bool is_project  = (g.mode == 0);
    const bool is_selected = (g.mode == 1);
    const bool is_single   = (g.mode == 2);
    const bool needs_list  = is_selected || is_single;

    ImGui::Separator();

    /* ── Source: where scenes come from ──────────────────────────── */
    if (ImGui::CollapsingHeader(BL("section.source", "Source"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (is_project) {
            jce_draw_path_input(BL("field.project_root", "Project root"),
                                g.project_root, sizeof(g.project_root),
                                JcePathKind::FolderAbs);
            ImGui::SameLine();
            if (ImGui::Button(BL("btn.use_current", "Use current"))) {
                if (s_current_project_root[0]) {
                    snprintf(g.project_root, sizeof(g.project_root),
                             "%s", s_current_project_root);
                    apply_default_layout_paths();
                }
            }
            jce_draw_path_input(BL("field.scenes_dir_opt", "Scenes dir (optional)"),
                                g.scenes_dir, sizeof(g.scenes_dir),
                                JcePathKind::FolderAbs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BL("tooltip.scenes_dir",
                    "Leave empty to scan <project>/ recursively."));
        }
        if (needs_list) {
            JcePathInputOpts so; so.filter = "Scene (*.scene.json);;All Files (*.*)";
            jce_draw_path_input(BL("field.add_scene", "Add scene file"),
                                g.scene_file_input, sizeof(g.scene_file_input),
                                JcePathKind::FileAbs, &so);
            ImGui::SameLine();
            if (ImGui::Button(BL("btn.add", "+ Add")) && g.scene_file_input[0]) {
                if (is_single) g.scene_files.clear();
                g.scene_files.emplace_back(g.scene_file_input);
                g.scene_file_input[0] = '\0';
            }
            if (is_single && g.scene_files.size() > 1)
                g.scene_files.resize(1);
            if (ImGui::BeginListBox("##scene_files",
                                    ImVec2(-FLT_MIN, 80))) {
                int del_idx = -1;
                for (size_t i = 0; i < g.scene_files.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TextUnformatted(g.scene_files[i].c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(BL("btn.remove", "X")))
                        del_idx = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (del_idx >= 0)
                    g.scene_files.erase(g.scene_files.begin() + del_idx);
                ImGui::EndListBox();
            }
        }
        /* Asset root — applies to every mode; default differs. */
        ImGui::Checkbox(BL("field.auto_asset_root", "Auto-detect asset root"),
                        &g.auto_resource_root);
        if (!g.auto_resource_root) {
            jce_draw_path_input(BL("field.asset_root", "Asset root"),
                                g.resource_root, sizeof(g.resource_root),
                                JcePathKind::FolderAbs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BL("tooltip.asset_root",
                    "Asset paths in scenes are resolved relative to this."));
        }
    }

    /* ── Output: where bundles go ────────────────────────────────── */
    if (ImGui::CollapsingHeader(BL("section.output", "Output"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        jce_draw_path_input(BL("field.output_dir", "Output dir"),
                            g.out_dir, sizeof(g.out_dir),
                            JcePathKind::FolderAbs);
        if (is_single) {
            ImGui::InputText(BL("field.bundle_id_opt", "Bundle id (optional)"),
                             g.single_bundle_id, sizeof(g.single_bundle_id));
            ImGui::TextDisabled("%s", BL("hint.single_mode",
                "Single-scene mode: one self-contained .jbundle,\n"
                "no catalog, no shared split."));
        } else {
            ImGui::InputText(BL("field.prev_catalog",
                                "Previous catalog (incremental)"),
                             g.prev_catalog, sizeof(g.prev_catalog));
        }
    }

    /* ── Pack options: only meaningful in Project / Selected modes. */
    if (!is_single &&
        ImGui::CollapsingHeader(BL("section.options", "Pack options"))) {
        ImGui::InputInt (BL("field.shared_threshold", "Shared threshold"),
                         &g.shared_threshold);
        if (g.shared_threshold < 2) g.shared_threshold = 2;
        ImGui::InputText(BL("field.shared_id", "Shared bundle id"),
                         g.shared_id, sizeof(g.shared_id));
        ImGui::SliderInt(BL("field.zstd_level", "Zstd level"),
                         &g.zstd_level, 1, 22);
    } else if (is_single &&
               ImGui::CollapsingHeader(BL("section.options", "Pack options"))) {
        ImGui::SliderInt(BL("field.zstd_level", "Zstd level"),
                         &g.zstd_level, 1, 22);
    }

    if (is_project) {
        if (ImGui::Button(BL("btn.init_folders", "Initialize Default Folders"))) {
            bool ok = initialise_default_layout_dirs();
            jce_editor_console_log("[bundle] %s",
                ok ? BL("msg.folders_ready", "default folders ready")
                   : BL("msg.folders_failed",
                        "failed to create some folders"));
        }
    }

    ImGui::Separator();

    bool running = g.running ? jce_atomic_i32_load(g.running) != 0 : false;
    if (running) ImGui::BeginDisabled();
    const char *build_label = is_single
        ? BL("btn.pack_scene", "Pack Scene")
        : (is_selected
            ? BL("btn.pack_selected", "Pack Selected")
            : BL("btn.build", "Build Bundles"));
    if (ImGui::Button(build_label)) {
        start_build();
    }
    if (running) ImGui::EndDisabled();

    /* Reload Summary only meaningful when a catalog exists. */
    if (!is_single) {
        ImGui::SameLine();
        if (ImGui::Button(BL("btn.reload_summary", "Reload Summary"))) {
            load_summary_from_catalog();
        }
    }

    if (!g.last_status.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g.last_status.c_str());
    }

    ImGui::Separator();

    /* Catalog summary — irrelevant in single-file mode (no catalog written). */
    if (!is_single &&
        ImGui::CollapsingHeader(BL("section.catalog", "Catalog summary"),
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        if (g.summary.empty()) {
            ImGui::TextDisabled("%s", BL("msg.no_catalog",
                                        "(no catalog loaded)"));
        } else if (ImGui::BeginTable("##bundles", 4,
                                       ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                       ImVec2(0, 220))) {
            ImGui::TableSetupColumn(BL("col.bundle", "Bundle"));
            ImGui::TableSetupColumn(BL("col.kind", "Kind"));
            ImGui::TableSetupColumn(BL("col.size_kb", "Size (KB)"));
            ImGui::TableSetupColumn(BL("col.deps", "Deps"));
            ImGui::TableHeadersRow();
            for (const auto &b : g.summary) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(b.id.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(b.kind.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", b.size_bytes / 1024.0);
                ImGui::TableNextColumn();
                ImGui::Text("%d", b.dep_count);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader(BL("section.deps_preview",
                                   "Scene dependency preview"))) {
        if (g.scenes_dir[0] == '\0') {
            ImGui::TextDisabled("%s", BL("msg.no_scenes_dir",
                                        "(set scenes dir first)"));
        } else {
            static char preview_scene[256] = "";
            JcePathInputOpts po;
            po.filter = "Scene (*.scene.json);;All Files (*.*)";
            jce_draw_path_input(BL("field.scene_file", "Scene file"),
                                preview_scene, sizeof(preview_scene),
                                JcePathKind::FileAbs, &po);
            if (ImGui::Button(BL("btn.scan_deps", "Scan deps")) && preview_scene[0]) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", g.scenes_dir, preview_scene);
                JceBundleDepList deps{};
                if (jce_bundle_deps_scan_file(full, &deps)) {
                    jce_editor_console_log("[bundle] scan %s: %u dep(s)",
                                           full, deps.count);
                    for (uint32_t i = 0; i < deps.count; ++i) {
                        jce_editor_console_log("[bundle]   - %s%s%s",
                            deps.items[i].path,
                            deps.items[i].bundle ? "  [tag=" : "",
                            deps.items[i].bundle ? deps.items[i].bundle : "");
                    }
                    jce_bundle_deps_free(&deps);
                } else {
                    jce_editor_console_log("[bundle] cannot scan %s", full);
                }
            }
        }
    }

    ImGui::End();
}
