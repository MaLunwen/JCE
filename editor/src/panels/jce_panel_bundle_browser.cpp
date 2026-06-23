/*
 * jce_panel_bundle_browser.cpp  Bundle Browser dock panel — P5-A.3.
 *
 * Single source of truth for all bundle UIs. Three tabs:
 *   - Browse  : catalog inspector (P3-A.1 origin)
 *   - Build   : pack scenes into bundles (was jce_dialog_bundles.cpp)
 *   - Open    : load a .jbundle or bundle_catalog.json for preview
 *               (was jce_dialog_open_bundle.cpp)
 *
 * Both retired modals are kept as ~30-LOC shims that flip this panel's
 * visible flag and request the appropriate tab via the helpers exposed
 * at the bottom of this file (see jce_editor_panel_bundle_browser_*).
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_pak_key.h"
#include "dialogs/jce_editor_dialogs_internal.h"
#include "dialogs/jce_path_input.h"

#include <jce/tools/jce_imgui.hpp>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <jce/api_resource.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_thread.h>
#include <jce/resource/jce_bundle_deps.h>
#include <jce/resource/jce_bundle_format.h>
#include <jce/resource/jce_bundle_loader.h>
#include <jce/resource/jce_bundle_pack.h>
#include "scene/jce_asset_path_index.h"

#include <jce/os/core/jce_json.h>
}

namespace {

/* ── Tab routing ─────────────────────────────────────────────── */

enum Tab { TAB_BROWSE = 0, TAB_BUILD = 1, TAB_OPEN = 2 };
int g_pending_tab = -1;   /* set by shims; consumed on next render */
int g_current_bundle_tab = TAB_BROWSE;
bool g_bundle_tab_state_loaded = false;

static const char *k_bundle_tab_state_key = "panel.bundle_browser.current_tab";

bool valid_bundle_tab(int idx)
{
    return idx >= TAB_BROWSE && idx <= TAB_OPEN;
}

void ensure_bundle_tab_state_loaded(void)
{
    if (g_bundle_tab_state_loaded)
        return;
    g_current_bundle_tab =
        jce_editor_ui_state_load_int(k_bundle_tab_state_key,
                                     TAB_BROWSE, TAB_BROWSE, TAB_OPEN);
    g_pending_tab = g_current_bundle_tab;
    g_bundle_tab_state_loaded = true;
}

void set_bundle_tab(int idx)
{
    if (!valid_bundle_tab(idx) || g_current_bundle_tab == idx)
        return;
    g_current_bundle_tab = idx;
    if (g_bundle_tab_state_loaded)
        jce_editor_ui_state_save_int(k_bundle_tab_state_key, idx);
}

/* ════════════════════════════════════════════════════════════════
 * Tab 1 — Browse (catalog inspector)
 * ════════════════════════════════════════════════════════════════ */

struct BundleRow {
    std::string              id;
    std::string              kind;
    std::string              file;
    std::string              scene_path;
    std::string              hash;
    uint64_t                 size_bytes = 0;
    uint32_t                 refcount   = 0;
    uint32_t                 entries    = 0;
    bool                     has_entries = false;
    std::vector<std::string> deps;
    std::vector<std::string> dependents;
};

struct BrowseState {
    char                     catalog_path[1024] = {0};
    bool                     path_initialized   = false;
    std::vector<BundleRow>   rows;
    int                      selected_idx       = -1;
    char                     filter[128]        = {0};
    char                     status_msg[256]    = {0};
    bool                     loaded             = false;
    uint64_t                 total_bytes        = 0;
};
BrowseState g_br;

const char *fmt_size(uint64_t b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int k = 0;
    while (v >= 1024.0 && k < 4) { v /= 1024.0; ++k; }
    snprintf(out, n, "%.2f %s", v, u[k]);
    return out;
}

void browse_init_default_path()
{
    if (g_br.path_initialized) return;
    g_br.path_initialized = true;
    const char *proj = jce_editor_assets_get_project();
    if (proj && *proj) {
        snprintf(g_br.catalog_path, sizeof(g_br.catalog_path),
                 "%s/bundles/bundle_catalog.json", proj);
    }
}

void browse_refresh()
{
    g_br.rows.clear();
    g_br.selected_idx = -1;
    g_br.total_bytes  = 0;
    g_br.loaded       = false;
    g_br.status_msg[0] = '\0';

    if (g_br.catalog_path[0] == '\0') {
        snprintf(g_br.status_msg, sizeof(g_br.status_msg), "%s",
                 jce_editor_i18n("panel.bundle_browser.status.no_path"));
        return;
    }
    if (!jce_fs_host_exists_file(g_br.catalog_path)) {
        snprintf(g_br.status_msg, sizeof(g_br.status_msg), "%s",
                 jce_editor_i18n("panel.bundle_browser.status.not_found"));
        return;
    }

    JceFileSystem *fs = jce_fs_create();
    if (!fs) return;
    JceBundleCatalog *cat = jce_bundle_catalog_open(fs, g_br.catalog_path);
    if (!cat) {
        jce_fs_destroy(fs);
        snprintf(g_br.status_msg, sizeof(g_br.status_msg), "%s",
                 jce_editor_i18n("panel.bundle_browser.status.open_failed"));
        return;
    }

    uint32_t n = jce_bundle_catalog_count(cat);
    g_br.rows.reserve(n);

    std::string base = g_br.catalog_path;
    size_t cut = base.find_last_of("/\\");
    base = (cut == std::string::npos) ? std::string(".") : base.substr(0, cut);

    for (uint32_t i = 0; i < n; ++i) {
        const char *id = jce_bundle_catalog_id_at(cat, i);
        if (!id) continue;
        BundleRow r;
        r.id         = id;
        const char *k = jce_bundle_catalog_kind(cat, id);
        r.kind       = k ? k : "";
        const char *f = jce_bundle_catalog_file(cat, id);
        r.file       = f ? f : "";
        const char *sp = jce_bundle_catalog_scene_path(cat, id);
        r.scene_path = sp ? sp : "";
        const char *h = jce_bundle_catalog_content_hash(cat, id);
        r.hash       = h ? h : "";
        r.size_bytes = jce_bundle_catalog_size_bytes(cat, id);
        r.refcount   = jce_bundle_refcount(cat, id);
        g_br.total_bytes += r.size_bytes;

        uint32_t dc = jce_bundle_catalog_dep_count(cat, id);
        r.deps.reserve(dc);
        for (uint32_t d = 0; d < dc; ++d) {
            const char *dep = jce_bundle_catalog_dep_at(cat, id, d);
            if (dep) r.deps.emplace_back(dep);
        }

        if (!r.file.empty()) {
            std::string fp = base + "/" + r.file;
            JcePakArchive *pak = jce_pak_open_file(fp.c_str());
            if (pak) {
                r.entries     = jce_pak_count(pak);
                r.has_entries = true;
                jce_pak_close(pak);
            }
        }

        g_br.rows.push_back(std::move(r));
    }
    jce_bundle_catalog_close(cat);
    jce_fs_destroy(fs);

    for (size_t i = 0; i < g_br.rows.size(); ++i) {
        for (const auto &dep : g_br.rows[i].deps) {
            for (auto &other : g_br.rows) {
                if (other.id == dep) {
                    other.dependents.push_back(g_br.rows[i].id);
                    break;
                }
            }
        }
    }

    g_br.loaded = true;
}

bool browse_row_matches_filter(const BundleRow &r)
{
    if (g_br.filter[0] == '\0') return true;
    auto hit = [](const std::string &hay, const char *needle) -> bool {
        size_t hn = hay.size(), nn = std::strlen(needle);
        if (nn == 0) return true;
        if (nn > hn) return false;
        for (size_t i = 0; i + nn <= hn; ++i) {
            size_t j = 0;
            for (; j < nn; ++j) {
                char a = hay[i + j], b = needle[j];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
            }
            if (j == nn) return true;
        }
        return false;
    };
    return hit(r.id, g_br.filter)
        || hit(r.scene_path, g_br.filter)
        || hit(r.file, g_br.filter);
}

void browse_draw_deps_pane(const BundleRow &r)
{
    if (ImGui::TreeNodeEx("##deps_out",
            ImGuiTreeNodeFlags_DefaultOpen,
            "%s (%d)",
            jce_editor_i18n("panel.bundle_browser.deps.dependencies"),
            (int)r.deps.size())) {
        if (r.deps.empty()) ImGui::TextDisabled("-");
        else for (const auto &d : r.deps) ImGui::BulletText("%s", d.c_str());
        ImGui::TreePop();
    }
    if (ImGui::TreeNodeEx("##deps_in",
            ImGuiTreeNodeFlags_DefaultOpen,
            "%s (%d)",
            jce_editor_i18n("panel.bundle_browser.deps.dependents"),
            (int)r.dependents.size())) {
        if (r.dependents.empty()) ImGui::TextDisabled("-");
        else for (const auto &d : r.dependents) ImGui::BulletText("%s", d.c_str());
        ImGui::TreePop();
    }
}

void draw_browse_tab()
{
    browse_init_default_path();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::InputText("##bundle_catalog_path",
                     g_br.catalog_path, sizeof(g_br.catalog_path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("panel.bundle_browser.toolbar.refresh"))) {
        browse_refresh();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##bundle_filter",
        jce_editor_i18n("panel.bundle_browser.toolbar.filter"),
        g_br.filter, sizeof(g_br.filter));

    if (g_br.status_msg[0]) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f), "%s", g_br.status_msg);
    }
    ImGui::Separator();

    if (!g_br.loaded && g_br.rows.empty()) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.bundle_browser.empty"));
        return;
    }

    const float deps_h = 200.0f;
    float avail_h = ImGui::GetContentRegionAvail().y;
    float table_h = avail_h - deps_h - ImGui::GetFrameHeightWithSpacing();
    if (table_h < 80.0f) table_h = 80.0f;

    if (ImGui::BeginTable("##bundle_tbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Sortable,
            ImVec2(0, table_h))) {
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.name"),
            ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.size"),
            ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.entries"),
            ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.refs"),
            ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.kind"),
            ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(
            jce_editor_i18n("panel.bundle_browser.col.hash"),
            ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();

        char sb[32];
        for (int i = 0; i < (int)g_br.rows.size(); ++i) {
            BundleRow &r = g_br.rows[i];
            if (!browse_row_matches_filter(r)) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            bool sel = (g_br.selected_idx == i);
            if (ImGui::Selectable(r.id.c_str(), sel,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap)) {
                g_br.selected_idx = i;
            }
            if (ImGui::BeginPopupContextItem("##row_ctx")) {
                if (ImGui::MenuItem(
                        jce_editor_i18n("panel.bundle_browser.ctx.copy_hash"))) {
                    ImGui::SetClipboardText(r.hash.c_str());
                }
                ImGui::BeginDisabled();
                ImGui::MenuItem(
                    jce_editor_i18n("panel.bundle_browser.ctx.reveal"));
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(fmt_size(r.size_bytes, sb, sizeof(sb)));
            ImGui::TableSetColumnIndex(2);
            if (r.has_entries) ImGui::Text("%u", r.entries);
            else               ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", r.refcount);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(r.kind.c_str());
            ImGui::TableSetColumnIndex(5);
            if (r.hash.empty()) ImGui::TextDisabled("-");
            else                ImGui::TextUnformatted(r.hash.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    if (g_br.selected_idx >= 0 &&
        g_br.selected_idx < (int)g_br.rows.size()) {
        ImGui::BeginChild("##deps_view", ImVec2(0, deps_h), true);
        const BundleRow &r = g_br.rows[g_br.selected_idx];
        ImGui::Text("%s  [%s]", r.id.c_str(),
                    r.scene_path.empty() ? "-" : r.scene_path.c_str());
        ImGui::Separator();
        browse_draw_deps_pane(r);
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##deps_view", ImVec2(0, deps_h), true);
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.bundle_browser.deps.no_selection"));
        ImGui::EndChild();
    }

    ImGui::Separator();
    char sb[32];
    fmt_size(g_br.total_bytes, sb, sizeof(sb));
    ImGui::Text(jce_editor_i18n("panel.bundle_browser.status.total"),
                (int)g_br.rows.size(), sb);
}

/* ════════════════════════════════════════════════════════════════
 * Tab 2 — Build (was jce_dialog_bundles.cpp)
 * ════════════════════════════════════════════════════════════════ */

#define JCE_BUNDLE_DEFAULT_SCENES_SUBDIR    "scenes"
#define JCE_BUNDLE_DEFAULT_RESOURCE_SUBDIR  "resources/assets"
#define JCE_BUNDLE_DEFAULT_OUT_SUBDIR       "bundles"
#define JCE_BUNDLE_DEFAULT_PREV_SUBDIR      "bundles/.prev"

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
    int         level;
    std::string text;
};

struct BuildState {
    int mode = 0;   /* 0=Project, 1=Selected, 2=Single */

    char project_root [512] {};
    char scenes_dir   [512] {};
    char resource_root[512] {};
    char out_dir      [512] {};
    char prev_catalog [512] {};
    char shared_id    [128] {};
    int  shared_threshold = 2;
    int  zstd_level       = 3;
    bool cook_assets      = true;   /* P0: cook textures/audio/models    */
    int  target_platform  = 0;      /* 0=Win 1=Linux 2=mac 3=Android 4=iOS 5=Web */
    bool encrypt          = false;  /* ChaCha20-encrypt bundle payloads  */

    std::vector<std::string> scene_files;
    char scene_file_input[512] {};
    char single_bundle_id[128] {};
    bool auto_resource_root = true;

    JceThread *   worker     = nullptr;
    JceAtomicI32 *running    = nullptr;
    JceAtomicI32 *finished   = nullptr;
    JceAtomicI32 *last_exit  = nullptr;
    JceMutex *    log_mtx    = nullptr;
    std::deque<LogLine>  log_queue;

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
    bool cook_assets_owned = false;
    int  target_platform_owned = 0;
    bool    encrypt_owned = false;
    uint8_t key_owned[32] = {0};    /* loaded on the main thread at start */

    std::string last_status;
    std::vector<BundleSummary> summary;
    bool inited = false;
};
BuildState gb;

/* One-shot scene path queued by jce_editor_dialog_bundles_open_for_current_scene(). */
std::string g_pending_single_scene;

void apply_default_layout_paths()
{
    const char *proj_root = s_current_project_root;
    if (!proj_root || !proj_root[0]) return;
    snprintf(gb.project_root,  sizeof(gb.project_root),  "%s", proj_root);
    snprintf(gb.out_dir,       sizeof(gb.out_dir),       "%s/.bundles",
             proj_root);
    snprintf(gb.prev_catalog,  sizeof(gb.prev_catalog),  "%s/.bundles/.prev/%s",
             proj_root, JCE_BUNDLE_CATALOG_NAME);
}

bool initialise_default_layout_dirs()
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

void build_initialise_defaults_from_project()
{
    if (!gb.inited) {
        gb.inited = true;
        snprintf(gb.shared_id, sizeof(gb.shared_id), "%s",
                 JCE_BUNDLE_SHARED_DEFAULT_ID);
        if (!gb.running)   gb.running   = jce_atomic_i32_create(0);
        if (!gb.finished)  gb.finished  = jce_atomic_i32_create(0);
        if (!gb.last_exit) gb.last_exit = jce_atomic_i32_create(0);
        if (!gb.log_mtx)   gb.log_mtx   = jce_mutex_create();
    }
    const char *proj_root = s_current_project_root;
    if (!proj_root || !proj_root[0]) return;
    if (gb.project_root[0] == '\0')
        snprintf(gb.project_root, sizeof(gb.project_root), "%s", proj_root);
    if (gb.out_dir[0] == '\0')
        snprintf(gb.out_dir, sizeof(gb.out_dir), "%s/.bundles", proj_root);
    if (gb.prev_catalog[0] == '\0')
        snprintf(gb.prev_catalog, sizeof(gb.prev_catalog), "%s/.bundles/.prev/%s",
                 proj_root, JCE_BUNDLE_CATALOG_NAME);
}

bool editor_pack_resolve(const char *vpath, char *out, size_t outsz,
                         void * /*user*/)
{
    if (!vpath || !out || outsz == 0) return false;
    return jce_asset_path_index_lookup(vpath, out, (int)outsz);
}

void worker_log_sink(JceBundlePackLogLevel level, const char *msg, void *user)
{
    BuildState *st = static_cast<BuildState *>(user);
    if (!st || !msg) return;
    LogLine line;
    line.level = static_cast<int>(level);
    line.text  = msg;
    jce_mutex_lock(st->log_mtx);
    st->log_queue.emplace_back(std::move(line));
    jce_mutex_unlock(st->log_mtx);
}

void drain_log_queue()
{
    std::deque<LogLine> local;
    jce_mutex_lock(gb.log_mtx);
    local.swap(gb.log_queue);
    jce_mutex_unlock(gb.log_mtx);
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

void bundle_pack_worker_main(void * /*user*/)
{
    JceBundlePackOptions opts{};
    const bool selected_or_single = gb.mode_owned == 1 || gb.mode_owned == 2;
    opts.project_root     = (selected_or_single || gb.project_root_owned.empty())
                                ? nullptr : gb.project_root_owned.c_str();
    opts.scenes_dir       = (selected_or_single || gb.scenes_dir_owned.empty())
                                ? nullptr : gb.scenes_dir_owned.c_str();
    opts.resource_root    = gb.resource_root_owned.empty()
                                ? nullptr : gb.resource_root_owned.c_str();
    opts.out_dir          = gb.out_dir_owned.empty()
                                ? nullptr : gb.out_dir_owned.c_str();
    opts.prev_catalog     = (gb.mode_owned == 2 || gb.prev_catalog_owned.empty())
                                ? nullptr : gb.prev_catalog_owned.c_str();
    opts.shared_id        = gb.shared_id_owned.empty()
                                ? nullptr : gb.shared_id_owned.c_str();
    opts.shared_threshold = gb.shared_threshold;
    opts.zstd_level       = gb.zstd_level;
    opts.catalog_version  = 0;
    opts.quiet            = false;
    if (selected_or_single && !gb.scene_file_ptrs_owned.empty()) {
        opts.scene_files      = gb.scene_file_ptrs_owned.data();
        opts.scene_file_count = gb.scene_file_ptrs_owned.size();
    }
    opts.auto_resource_root = gb.auto_resource_root_owned;
    opts.single_file_mode   = (gb.mode_owned == 2);
    opts.single_bundle_id   = gb.single_bundle_id_owned.empty()
                                ? nullptr : gb.single_bundle_id_owned.c_str();
    opts.resolve_fn         = &editor_pack_resolve;
    opts.resolve_user       = nullptr;
    opts.cook_assets        = gb.cook_assets_owned;
    opts.target_platform    = gb.target_platform_owned;
    opts.encrypt            = gb.encrypt_owned;
    opts.encryption_key     = gb.encrypt_owned ? gb.key_owned : nullptr;

    int rc = jce_bundle_pack_run(&opts, worker_log_sink, &gb);
    jce_atomic_i32_store(gb.last_exit, rc);
    jce_atomic_i32_store(gb.running, 0);
    jce_atomic_i32_store(gb.finished, 1);
}

void start_build()
{
    if (jce_atomic_i32_load(gb.running)) return;

    /* Resolve the encryption key on the main thread before the worker
     * spawns.  No key and no project -> refuse rather than silently pack
     * plaintext. */
    gb.encrypt_owned = gb.encrypt;
    if (gb.encrypt) {
        const char *proj = jce_editor_assets_get_project();
        std::string root = (proj && proj[0]) ? proj
                          : (s_current_project_root[0] ? s_current_project_root
                                                       : "");
        bool have = !root.empty() && jce_pak_key_load(root, gb.key_owned);
        if (!have && !root.empty()) {
            std::string err;
            if (jce_pak_key_generate(root, false, &err) &&
                jce_pak_key_load(root, gb.key_owned)) {
                have = true;
                jce_editor_console_log_level(JCE_CONSOLE_WARNING,
                    "[bundle] generated asset key: %s",
                    jce_pak_key_path(root).c_str());
            }
        }
        if (!have) {
            gb.last_status = BL("status.no_key",
                                "no encryption key (open a project first)");
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "[bundle] encryption requested but no key is available");
            return;
        }
    }

    gb.project_root_owned  = gb.project_root;
    gb.scenes_dir_owned    = gb.scenes_dir;
    gb.resource_root_owned = gb.resource_root;
    gb.out_dir_owned       = gb.out_dir;
    gb.prev_catalog_owned  = gb.prev_catalog;
    gb.shared_id_owned     = gb.shared_id;
    gb.mode_owned                = gb.mode;
    gb.auto_resource_root_owned  = gb.auto_resource_root;
    gb.cook_assets_owned         = gb.cook_assets;
    gb.target_platform_owned     = gb.target_platform;
    gb.single_bundle_id_owned    = gb.single_bundle_id;
    gb.scene_files_owned         = gb.scene_files;
    gb.scene_file_ptrs_owned.clear();
    gb.scene_file_ptrs_owned.reserve(gb.scene_files_owned.size());
    for (auto &s : gb.scene_files_owned)
        gb.scene_file_ptrs_owned.push_back(s.c_str());

    gb.summary.clear();
    gb.last_status.clear();
    jce_atomic_i32_store(gb.last_exit, 0);
    jce_atomic_i32_store(gb.finished, 0);
    jce_atomic_i32_store(gb.running, 1);

    if (gb.worker) { jce_thread_join(gb.worker); gb.worker = nullptr; }

    gb.worker = jce_thread_create(&bundle_pack_worker_main, nullptr,
                                   "jce-bundle-pack");
    if (!gb.worker) {
        jce_atomic_i32_store(gb.running, 0);
        jce_atomic_i32_store(gb.finished, 1);
        jce_atomic_i32_store(gb.last_exit, -1);
        gb.last_status = BL("status.failed_fmt", "build failed (rc=%d)");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "[bundle] worker thread spawn failed");
        return;
    }

    gb.last_status = BL("status.running", "running...");
    jce_editor_console_log("[bundle] %s",
        BL("status.started", "build started (in-process)"));
}

void load_summary_from_catalog()
{
    gb.summary.clear();
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", gb.out_dir, JCE_BUNDLE_CATALOG_NAME);
    uint64_t sz = 0;
    void *vbuf = jce_fs_host_read_all(path, &sz);
    if (!vbuf || sz == 0) {
        if (vbuf) jce_fs_buffer_free(vbuf);
        return;
    }
    std::string text(static_cast<const char *>(vbuf), static_cast<size_t>(sz));
    jce_fs_buffer_free(vbuf);

    JceJson *root = jce_json_parse(text.data(), text.size());
    if (!root) return;
    const JceJson *bundles = jce_json_get(root, JCE_BUNDLE_CATALOG_KEY_BUNDLES);
    if (jce_json_is_object(bundles)) {
        for (JceJson *e = jce_json_first_child(bundles); e;
             e = jce_json_next_sibling(e)) {
            BundleSummary bs;
            const char *key = jce_json_member_key(e);
            bs.id = key ? key : "?";
            if (const char *s = jce_json_get_string(e, JCE_BUNDLE_CATALOG_KEY_KIND, nullptr))
                bs.kind = s;
            if (const char *s = jce_json_get_string(e, JCE_BUNDLE_CATALOG_KEY_SCENE, nullptr))
                bs.scene_path = s;
            const JceJson *jz = jce_json_get(e, JCE_BUNDLE_CATALOG_KEY_SIZE);
            if (jz) bs.size_bytes = static_cast<uint64_t>(jce_json_number_value(jz, 0.0));
            const JceJson *jd = jce_json_get(e, JCE_BUNDLE_CATALOG_KEY_DEPS);
            if (jce_json_is_array(jd)) bs.dep_count = jce_json_array_size(jd);
            gb.summary.push_back(std::move(bs));
        }
    }
    jce_json_free(root);
}

/* P0-build-bundles-cook: cook toggle + target-platform selector, shared by
 * the project and single-scene "Pack options" sections. */
void draw_cook_options()
{
    ImGui::Checkbox(BL("field.cook_assets", "Cook assets (BC/ASTC + GLB)"),
                    &gb.cook_assets);
    if (gb.cook_assets) {
        const char *plats =
            "Windows (BC)\0Linux (BC)\0macOS (BC)\0Android (ASTC)\0"
            "iOS (ASTC)\0Web (ASTC)\0";
        ImGui::Combo(BL("field.target_platform", "Target platform"),
                     &gb.target_platform, plats);
        if (gb.target_platform < 0) gb.target_platform = 0;
        ImGui::TextDisabled("%s", BL("hint.cook_assets",
            "Textures -> GPU block format (.jceasset), models -> GLB+meshopt,\n"
            "audio -> PCM. Reads <asset>.import.json presets."));
    }

    /* ── Encryption (ChaCha20, keyed obfuscation) ───────────────────── */
    if (ImGui::Checkbox(BL("field.encrypt", "Encrypt bundles"),
                        &gb.encrypt)) {
        /* First enable: generate the project key on the spot so the user
         * sees the status flip to "key ready" immediately. */
        const char *proj = jce_editor_assets_get_project();
        std::string root = (proj && proj[0]) ? proj
                          : (s_current_project_root[0] ? s_current_project_root
                                                       : "");
        if (gb.encrypt && !root.empty() && !jce_pak_key_exists(root)) {
            std::string err;
            if (!jce_pak_key_generate(root, false, &err))
                jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                    "[bundle] key generate failed: %s", err.c_str());
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", BL("tooltip.encrypt",
            "Deters casual extraction; the key ships inside the game binary."));
    if (gb.encrypt) {
        const char *proj = jce_editor_assets_get_project();
        std::string root = (proj && proj[0]) ? proj
                          : (s_current_project_root[0] ? s_current_project_root
                                                       : "");
        uint64_t fp = 0;
        ImGui::SameLine();
        if (!root.empty() && jce_pak_key_fingerprint_of(root, &fp)) {
            char fphex[16];
            snprintf(fphex, sizeof(fphex), "%08x", (unsigned)(fp >> 32));
            ImGui::TextDisabled("%s %s",
                                BL("status.key_ready", "key:"), fphex);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "%s",
                BL("status.key_missing", "no key"));
            ImGui::SameLine();
            if (ImGui::SmallButton(BL("btn.generate_key", "Generate")) &&
                !root.empty()) {
                std::string err;
                if (!jce_pak_key_generate(root, false, &err))
                    jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                        "[bundle] key generate failed: %s", err.c_str());
            }
        }
    }
}

void draw_build_tab()
{
    if (!g_pending_single_scene.empty()) {
        gb.mode = 2;
        gb.scene_files.clear();
        gb.scene_files.emplace_back(g_pending_single_scene);
        gb.auto_resource_root = true;
        g_pending_single_scene.clear();
    }
    build_initialise_defaults_from_project();

    drain_log_queue();
    if (gb.finished && jce_atomic_i32_exchange(gb.finished, 0)) {
        if (gb.worker) { jce_thread_join(gb.worker); gb.worker = nullptr; }
        int rc = jce_atomic_i32_load(gb.last_exit);
        if (rc == 0) {
            gb.last_status = BL("status.succeeded", "build succeeded");
            load_summary_from_catalog();
        } else {
            char tmp[64];
            snprintf(tmp, sizeof(tmp),
                     BL("status.failed_fmt", "build failed (rc=%d)"), rc);
            gb.last_status = tmp;
        }
    }

    ImGui::TextUnformatted(BL("desc",
        "Pack scene-level asset bundles for incremental shipping.\n"
        "Assets used by 2+ scenes auto-move to a shared bundle."));
    ImGui::Separator();

    ImGui::TextUnformatted(BL("mode.label", "Pack mode:"));
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.project",  "Project"),         &gb.mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.selected", "Selected scenes"), &gb.mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton(BL("mode.single",   "Single scene"),    &gb.mode, 2);
    const bool is_project  = (gb.mode == 0);
    const bool is_selected = (gb.mode == 1);
    const bool is_single   = (gb.mode == 2);
    const bool needs_list  = is_selected || is_single;

    ImGui::Separator();

    if (ImGui::CollapsingHeader(BL("section.source", "Source"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        if (is_project) {
            jce_draw_path_input(BL("field.project_root", "Project root"),
                                gb.project_root, sizeof(gb.project_root),
                                JcePathKind::FolderAbs);
            ImGui::SameLine();
            if (ImGui::Button(BL("btn.use_current", "Use current"))) {
                if (s_current_project_root[0]) {
                    snprintf(gb.project_root, sizeof(gb.project_root),
                             "%s", s_current_project_root);
                    apply_default_layout_paths();
                }
            }
            jce_draw_path_input(BL("field.scenes_dir_opt",
                                    "Scenes dir (optional)"),
                                gb.scenes_dir, sizeof(gb.scenes_dir),
                                JcePathKind::FolderAbs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BL("tooltip.scenes_dir",
                    "Leave empty to scan <project>/ recursively."));
        }
        if (needs_list) {
            JcePathInputOpts so;
            so.filter = "Scene (*.scene *.scene.json);;All Files (*.*)";
            jce_draw_path_input(BL("field.add_scene", "Add scene file"),
                                gb.scene_file_input, sizeof(gb.scene_file_input),
                                JcePathKind::FileAbs, &so);
            ImGui::SameLine();
            if (ImGui::Button(BL("btn.add", "+ Add")) &&
                gb.scene_file_input[0]) {
                if (is_single) gb.scene_files.clear();
                gb.scene_files.emplace_back(gb.scene_file_input);
                gb.scene_file_input[0] = '\0';
            }
            if (is_single && gb.scene_files.size() > 1)
                gb.scene_files.resize(1);
            if (ImGui::BeginListBox("##scene_files", ImVec2(-FLT_MIN, 80))) {
                int del_idx = -1;
                for (size_t i = 0; i < gb.scene_files.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TextUnformatted(gb.scene_files[i].c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton(BL("btn.remove", "X")))
                        del_idx = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (del_idx >= 0)
                    gb.scene_files.erase(gb.scene_files.begin() + del_idx);
                ImGui::EndListBox();
            }
        }
        ImGui::Checkbox(BL("field.auto_asset_root",
                                   "Auto-detect asset root"),
                        &gb.auto_resource_root);
        if (!gb.auto_resource_root) {
            jce_draw_path_input(BL("field.asset_root", "Asset root"),
                                gb.resource_root, sizeof(gb.resource_root),
                                JcePathKind::FolderAbs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", BL("tooltip.asset_root",
                    "Asset paths in scenes are resolved relative to this."));
        }
    }

    if (ImGui::CollapsingHeader(BL("section.output", "Output"),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        jce_draw_path_input(BL("field.output_dir", "Output dir"),
                            gb.out_dir, sizeof(gb.out_dir),
                            JcePathKind::FolderAbs);
        if (is_single) {
            ImGui::InputText(BL("field.bundle_id_opt",
                                "Bundle id (optional)"),
                             gb.single_bundle_id, sizeof(gb.single_bundle_id));
            ImGui::TextDisabled("%s", BL("hint.single_mode",
                "Single-scene mode: one self-contained .jbundle,\n"
                "no catalog, no shared split."));
        } else {
            ImGui::InputText(BL("field.prev_catalog",
                                "Previous catalog (incremental)"),
                             gb.prev_catalog, sizeof(gb.prev_catalog));
        }
    }

    if (!is_single &&
        ImGui::CollapsingHeader(BL("section.options", "Pack options"))) {
        ImGui::InputInt (BL("field.shared_threshold", "Shared threshold"),
                         &gb.shared_threshold);
        if (gb.shared_threshold < 2) gb.shared_threshold = 2;
        ImGui::InputText(BL("field.shared_id", "Shared bundle id"),
                         gb.shared_id, sizeof(gb.shared_id));
        ImGui::SliderInt(BL("field.zstd_level", "Zstd level"),
                         &gb.zstd_level, 1, 22);
        draw_cook_options();
    } else if (is_single &&
               ImGui::CollapsingHeader(BL("section.options",
                                                  "Pack options"))) {
        ImGui::SliderInt(BL("field.zstd_level", "Zstd level"),
                         &gb.zstd_level, 1, 22);
        draw_cook_options();
    }

    if (is_project) {
        if (ImGui::Button(BL("btn.init_folders",
                                     "Initialize Default Folders"))) {
            bool ok = initialise_default_layout_dirs();
            jce_editor_console_log("[bundle] %s",
                ok ? BL("msg.folders_ready", "default folders ready")
                   : BL("msg.folders_failed",
                                "failed to create some folders"));
        }
    }

    ImGui::Separator();

    bool running = gb.running ? jce_atomic_i32_load(gb.running) != 0 : false;
    if (running) ImGui::BeginDisabled();
    const char *build_label = is_single
        ? BL("btn.pack_scene", "Pack Scene")
        : (is_selected ? BL("btn.pack_selected", "Pack Selected")
                       : BL("btn.build",         "Build Bundles"));
    if (ImGui::Button(build_label)) start_build();
    if (running) ImGui::EndDisabled();

    if (!is_single) {
        ImGui::SameLine();
        if (ImGui::Button(BL("btn.reload_summary", "Reload Summary")))
            load_summary_from_catalog();
    }

    if (!gb.last_status.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(gb.last_status.c_str());
    }

    ImGui::Separator();

    if (!is_single &&
        ImGui::CollapsingHeader(BL("section.catalog", "Catalog summary"),
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        if (gb.summary.empty()) {
            ImGui::TextDisabled("%s", BL("msg.no_catalog",
                                                  "(no catalog loaded)"));
        } else if (ImGui::BeginTable("##bundles", 4,
                                       ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_RowBg |
                                       ImGuiTableFlags_ScrollY,
                                       ImVec2(0, 220))) {
            ImGui::TableSetupColumn(BL("col.bundle",  "Bundle"));
            ImGui::TableSetupColumn(BL("col.kind",    "Kind"));
            ImGui::TableSetupColumn(BL("col.size_kb", "Size (KB)"));
            ImGui::TableSetupColumn(BL("col.deps",    "Deps"));
            ImGui::TableHeadersRow();
            for (const auto &b : gb.summary) {
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
        if (gb.scenes_dir[0] == '\0') {
            ImGui::TextDisabled("%s", BL("msg.no_scenes_dir",
                                        "(set scenes dir first)"));
        } else {
            static char preview_scene[256] = "";
            JcePathInputOpts po;
            po.filter = "Scene (*.scene *.scene.json);;All Files (*.*)";
            jce_draw_path_input(BL("field.scene_file", "Scene file"),
                                preview_scene, sizeof(preview_scene),
                                JcePathKind::FileAbs, &po);
            if (ImGui::Button(BL("btn.scan_deps", "Scan deps")) &&
                preview_scene[0]) {
                char full[1024];
                snprintf(full, sizeof(full), "%s/%s", gb.scenes_dir,
                         preview_scene);
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
}

/* ════════════════════════════════════════════════════════════════
 * Tab 3 — Open (was jce_dialog_open_bundle.cpp)
 * ════════════════════════════════════════════════════════════════ */

namespace open_ns {
static const char *BL(const char *suffix, const char *en_fallback)
{
    char key[160];
    snprintf(key, sizeof(key), "dialog.openBundle.%s", suffix);
    return jce_editor_i18n_or(key, en_fallback);
}

struct OpenBundleState {
    char        path_input[1024] = {0};
    bool        is_catalog       = false;
    std::string                 cached_path;
    std::vector<std::string>    bundle_ids;
    std::vector<std::string>    scene_paths;
    int                         selected_idx = -1;
    char                        status_msg[256] = {0};
};
OpenBundleState g_ob;

bool ends_with_ci(const char *s, const char *suf)
{
    if (!s || !suf) return false;
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return false;
    for (size_t i = 0; i < b; ++i) {
        char x = s[a - b + i], y = suf[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool is_bundle_sidecar_path(const char *path)
{
    return ends_with_ci(path, ".jbundle.json");
}

bool bundle_path_from_sidecar(const char *sidecar, char *out, size_t outsz)
{
    if (!is_bundle_sidecar_path(sidecar) || !out || outsz == 0)
        return false;
    size_t n = strlen(sidecar);
    size_t keep = n - strlen(".json");
    if (keep >= outsz)
        return false;
    memcpy(out, sidecar, keep);
    out[keep] = '\0';
    return true;
}

void open_detect_mode()
{
    if (g_ob.path_input[0] && jce_fs_host_exists_dir(g_ob.path_input)) {
        char candidate[1024];
        size_t n = strlen(g_ob.path_input);
        char sep = '/';
        for (size_t i = 0; i < n; ++i)
            if (g_ob.path_input[i] == '\\') { sep = '\\'; break; }
        bool has_trail = (n && (g_ob.path_input[n-1] == '/' ||
                                g_ob.path_input[n-1] == '\\'));
        snprintf(candidate, sizeof(candidate), "%s%sbundle_catalog.json",
                 g_ob.path_input, has_trail ? "" : (sep == '\\' ? "\\" : "/"));
        if (jce_fs_host_exists_file(candidate)) {
            snprintf(g_ob.path_input, sizeof(g_ob.path_input), "%s", candidate);
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.auto_catalog",
                        "directory detected — auto-selected bundle_catalog.json"));
        } else {
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.is_dir",
                        "path is a directory — pick a .jbundle file or bundle_catalog.json"));
        }
    }
    char bundle_path[1024];
    if (bundle_path_from_sidecar(g_ob.path_input, bundle_path,
                                 sizeof(bundle_path))) {
        if (jce_fs_host_exists_file(bundle_path)) {
            snprintf(g_ob.path_input, sizeof(g_ob.path_input), "%s",
                     bundle_path);
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.sidecar_bundle",
                        "sidecar manifest detected - opening sibling .jbundle"));
        } else {
            g_ob.is_catalog = false;
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.sidecar_missing_bundle",
                        "sidecar manifest selected, but sibling .jbundle is missing"));
            return;
        }
    }
    g_ob.is_catalog = ends_with_ci(g_ob.path_input, ".json");
}

void open_refresh_catalog_preview()
{
    g_ob.bundle_ids.clear();
    g_ob.scene_paths.clear();
    g_ob.selected_idx = -1;
    g_ob.cached_path = g_ob.path_input;
    if (!g_ob.is_catalog || g_ob.path_input[0] == '\0') return;

    JceFileSystem *fs = jce_fs_create();
    if (!fs) return;
    JceBundleCatalog *cat = jce_bundle_catalog_open(fs, g_ob.path_input);
    if (cat) {
        uint32_t n = jce_bundle_catalog_count(cat);
        for (uint32_t i = 0; i < n; ++i) {
            const char *id   = jce_bundle_catalog_id_at(cat, i);
            const char *kind = id ? jce_bundle_catalog_kind(cat, id) : nullptr;
            if (!id || !kind || strcmp(kind, "scene") != 0) continue;
            const char *sp = jce_bundle_catalog_scene_path(cat, id);
            g_ob.bundle_ids.emplace_back(id);
            g_ob.scene_paths.emplace_back(sp ? sp : "");
        }
        if (!g_ob.bundle_ids.empty()) g_ob.selected_idx = 0;
        jce_bundle_catalog_close(cat);
    } else {
        snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                 BL("err.catalog_open", "Failed to open catalog"));
    }
    jce_fs_destroy(fs);
}

void draw_open_tab()
{
    ImGui::TextWrapped("%s", BL("desc",
        "Open a packaged .jbundle (single-scene) or bundle_catalog.json\n"
        "to preview the scene as it would load at runtime.\n"
        "Bundle-loaded scenes are read-only — Save will be ignored."));
    ImGui::Separator();

    bool path_changed = ImGui::InputText(BL("field.path",
                                            "Bundle / catalog path"),
                                         g_ob.path_input,
                                         sizeof(g_ob.path_input));
    ImGui::SameLine();
    if (ImGui::Button(BL("btn.browse", "Browse..."))) {
        static bool browse_ready = false, browse_cancel = false;
        open_file_dialog_async(BL("title", "Open Bundle"),
                               g_ob.path_input,
                               jce_editor_i18n_or("fileDialog.filter.bundle",
                                    "Bundle / Catalog (*.jbundle *.json);;"
                                    "Single bundle (*.jbundle);;"
                                    "Bundle catalog (*.json);;"
                                    "All Files (*.*)"),
                               g_ob.path_input, sizeof(g_ob.path_input),
                               &browse_ready, &browse_cancel);
    }
    {
        static char last_seen[1024] = {0};
        if (strcmp(last_seen, g_ob.path_input) != 0) {
            snprintf(last_seen, sizeof(last_seen), "%s", g_ob.path_input);
            if (!path_changed) path_changed = true;
        }
    }
    if (path_changed) {
        g_ob.status_msg[0] = '\0';
        open_detect_mode();
    }

    ImGui::TextDisabled("%s: %s", BL("label.detected", "Detected"),
        g_ob.is_catalog ? BL("label.catalog", "bundle catalog (.json)")
                        : BL("label.single",  "single-file (.jbundle)"));

    if (g_ob.is_catalog) {
        if (g_ob.cached_path != g_ob.path_input) {
            if (ImGui::Button(BL("btn.scan_catalog", "Scan catalog")))
                open_refresh_catalog_preview();
        } else if (!g_ob.bundle_ids.empty()) {
            ImGui::TextUnformatted(BL("label.scene_bundles",
                "Scene bundles in catalog:"));
            if (ImGui::BeginListBox("##bundles_open", ImVec2(-FLT_MIN, 180))) {
                for (int i = 0; i < (int)g_ob.bundle_ids.size(); ++i) {
                    char line[512];
                    snprintf(line, sizeof(line), "%s   [%s]",
                             g_ob.bundle_ids[i].c_str(),
                             g_ob.scene_paths[i].c_str());
                    bool sel = (g_ob.selected_idx == i);
                    if (ImGui::Selectable(line, sel)) g_ob.selected_idx = i;
                }
                ImGui::EndListBox();
            }
        } else if (!g_ob.cached_path.empty()) {
            ImGui::TextDisabled("%s", BL("msg.no_scene_bundles",
                "(no scene bundles found in this catalog)"));
        }
    }

    ImGui::Separator();
    bool can_open = (g_ob.path_input[0] != '\0') &&
                    (!g_ob.is_catalog || g_ob.selected_idx >= 0 ||
                     g_ob.cached_path != g_ob.path_input);

    if (!can_open) ImGui::BeginDisabled();
    if (ImGui::Button(BL("btn.open", "Open"))) {
        bool ok = false;
        bool attempted = false;
        if (g_ob.path_input[0] == '\0') {
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.empty", "path is empty"));
        } else if (jce_fs_host_exists_dir(g_ob.path_input)) {
            open_detect_mode();
        } else if (!jce_fs_host_exists_file(g_ob.path_input)) {
            snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                     BL("msg.not_found", "file does not exist"));
        } else if (g_ob.is_catalog) {
            const char *bid = (g_ob.selected_idx >= 0 &&
                               g_ob.selected_idx < (int)g_ob.bundle_ids.size())
                ? g_ob.bundle_ids[g_ob.selected_idx].c_str()
                : nullptr;
            ok = jce_state_load_scene_from_catalog(g_ob.path_input, bid);
            attempted = true;
        } else {
            ok = jce_state_load_scene_from_jbundle(g_ob.path_input);
            attempted = true;
        }
        if (attempted) {
            if (ok) {
                snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                         BL("msg.opened", "scene loaded"));
            } else {
                snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                         BL("msg.failed", "load failed (see Console)"));
            }
        }
    }
    if (!can_open) ImGui::EndDisabled();

    if (g_ob.status_msg[0]) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g_ob.status_msg);
    }
}

} /* namespace open_ns */

} /* namespace */

/* ════════════════════════════════════════════════════════════════
 * Public entry points
 * ════════════════════════════════════════════════════════════════ */

/* Forward decls from sibling panels — their bodies without their own
 * window chrome, rendered here as outer tabs. */
extern "C" void import_presets_draw_content(void);
extern "C" void package_manager_draw_content(void);

/* Outer "Asset Pipeline" workbench tab request, set by sibling shims. */
static int g_request_outer_tab = -1;
static int g_current_outer_tab = 0;  /* mirror of active outer TabItem for menu markers */
static bool g_outer_tab_state_loaded = false;

static const char *k_outer_tab_state_key = "panel.asset_pipeline.current_tab";

static bool valid_outer_tab(int idx)
{
    return idx >= 0 && idx <= 2;
}

static void ensure_outer_tab_state_loaded(void)
{
    if (g_outer_tab_state_loaded)
        return;
    g_current_outer_tab =
        jce_editor_ui_state_load_int(k_outer_tab_state_key, 0, 0, 2);
    g_request_outer_tab = g_current_outer_tab;
    g_outer_tab_state_loaded = true;
}

static void set_outer_tab(int idx)
{
    if (!valid_outer_tab(idx) || g_current_outer_tab == idx)
        return;
    g_current_outer_tab = idx;
    if (g_outer_tab_state_loaded)
        jce_editor_ui_state_save_int(k_outer_tab_state_key, idx);
}

extern "C" void jce_panel_bundle_browser_request_tab(int idx)
{
    if (!valid_outer_tab(idx))
        return;
    g_request_outer_tab = idx;
    g_current_outer_tab = idx;
    jce_editor_ui_state_save_int(k_outer_tab_state_key, idx);
}

extern "C" int jce_panel_bundle_browser_current_tab(void)
{
    ensure_outer_tab_state_loaded();
    return g_current_outer_tab;
}

static void draw_bundles_tab(void)
{
    ensure_bundle_tab_state_loaded();

    /* Always pump the build worker, even when not on the Build tab,
     * so completion / log lines surface immediately. */
    if (gb.log_mtx) drain_log_queue();

    if (ImGui::BeginTabBar("##bundle_browser_tabs")) {
        auto tab_flag = [](Tab t) -> ImGuiTabItemFlags {
            if (g_pending_tab == (int)t) return ImGuiTabItemFlags_SetSelected;
            return 0;
        };

        if (ImGui::BeginTabItem(jce_editor_i18n("panel.bundle_browser.tab.browse"),
                                nullptr, tab_flag(TAB_BROWSE))) {
            set_bundle_tab(TAB_BROWSE);
            draw_browse_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.bundle_browser.tab.build"),
                                nullptr, tab_flag(TAB_BUILD))) {
            set_bundle_tab(TAB_BUILD);
            draw_build_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.bundle_browser.tab.open"),
                                nullptr, tab_flag(TAB_OPEN))) {
            set_bundle_tab(TAB_OPEN);
            open_ns::draw_open_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    g_pending_tab = -1;
}

extern "C" void jce_editor_panel_bundle_browser_content(void)
{
    ensure_outer_tab_state_loaded();
    if (!ImGui::BeginTabBar("##asset_pipeline_tabs"))
        return;

    ImGuiTabItemFlags bun_flags = (g_request_outer_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags imp_flags = (g_request_outer_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags pkg_flags = (g_request_outer_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;

    char bun_label[96];
    char imp_label[96];
    char pkg_label[96];
    std::snprintf(bun_label, sizeof(bun_label), "%s###ap_tab_bundles",
                  jce_editor_i18n("panel.bundle_browser.title"));
    std::snprintf(imp_label, sizeof(imp_label), "%s###ap_tab_import",
                  jce_editor_i18n("importPresets.title"));
    std::snprintf(pkg_label, sizeof(pkg_label), "%s###ap_tab_packages",
                  jce_editor_i18n("packageManager.title"));

    if (ImGui::BeginTabItem(bun_label, nullptr, bun_flags)) {
        set_outer_tab(0);
        draw_bundles_tab();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(imp_label, nullptr, imp_flags)) {
        set_outer_tab(1);
        import_presets_draw_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(pkg_label, nullptr, pkg_flags)) {
        set_outer_tab(2);
        package_manager_draw_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_outer_tab = -1;
}

/* Shim helpers — flip panel visible + select a tab.
 * Called from jce_dialog_bundles.cpp / jce_dialog_open_bundle.cpp shims. */
extern "C" void jce_editor_panel_bundle_browser_show_build(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER);
    if (vis) *vis = true;
    jce_editor_panel_request_focus("###bundle_browser");
    g_request_outer_tab = 0;
    g_current_outer_tab = 0;
    jce_editor_ui_state_save_int(k_outer_tab_state_key, 0);
    g_pending_tab = TAB_BUILD;
    g_current_bundle_tab = TAB_BUILD;
    jce_editor_ui_state_save_int(k_bundle_tab_state_key, TAB_BUILD);
}

extern "C" void jce_editor_panel_bundle_browser_show_open(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUNDLE_BROWSER);
    if (vis) *vis = true;
    jce_editor_panel_request_focus("###bundle_browser");
    g_request_outer_tab = 0;
    g_current_outer_tab = 0;
    jce_editor_ui_state_save_int(k_outer_tab_state_key, 0);
    g_pending_tab = TAB_OPEN;
    g_current_bundle_tab = TAB_OPEN;
    jce_editor_ui_state_save_int(k_bundle_tab_state_key, TAB_OPEN);
}

extern "C" bool jce_editor_panel_bundle_browser_request_pack_scene(
    const char *scene_path)
{
    if (!scene_path || !scene_path[0]) return false;
    g_pending_single_scene = scene_path;
    jce_editor_panel_bundle_browser_show_build();
    return true;
}
