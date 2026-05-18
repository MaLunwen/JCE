/*
 * jce_dialog_open_bundle.cpp  Open a packaged .jbundle or
 * bundle_catalog.json in the editor for preview / verification.
 *
 * Two input modes, auto-detected by file extension:
 *   *.jbundle  → single-file bundle (jce_state_load_scene_from_jbundle)
 *   *.json     → bundle catalog       (jce_state_load_scene_from_catalog)
 *
 * For catalogs the dialog lists every scene bundle so the user can
 * pick which one to load.  Scenes opened from bundles are marked
 * read-only via the synthetic "bundle://" / "catalog://" path prefix
 * so accidental Save Scene won't corrupt the bundle.
 */

#include "jce_editor_dialogs_internal.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_bundle_loader.h>
}

#include "core/jce_editor_state.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

#define BL(suffix, en) jce_editor_i18n_or("dialog.openBundle." suffix, en)

struct OpenBundleState {
    char        path_input[1024] = {0};
    bool        is_catalog       = false;
    /* Cached catalog scan results (refreshed when path/preview changes). */
    std::string                 cached_path;
    std::vector<std::string>    bundle_ids;
    std::vector<std::string>    scene_paths;
    int                         selected_idx = -1;
    char                        status_msg[256] = {0};
};
OpenBundleState g_ob;

static bool ends_with_ci(const char *s, const char *suf)
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

static void detect_mode()
{
    /* If user pointed at a directory, try to auto-find a catalog inside it. */
    if (g_ob.path_input[0] && jce_fs_host_exists_dir(g_ob.path_input)) {
        char candidate[1024];
        size_t n = strlen(g_ob.path_input);
        char sep = '/';
        for (size_t i = 0; i < n; ++i) if (g_ob.path_input[i] == '\\') { sep = '\\'; break; }
        bool has_trail = (n && (g_ob.path_input[n-1] == '/' || g_ob.path_input[n-1] == '\\'));
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
    g_ob.is_catalog = ends_with_ci(g_ob.path_input, ".json");
}

static void refresh_catalog_preview()
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

} /* namespace */

extern "C" void jce_editor_dialog_open_bundle(bool *p_open)
{
    if (!p_open || !*p_open) return;

    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(BL("title", "Open Bundle"), p_open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("%s", BL("desc",
        "Open a packaged .jbundle (single-scene) or bundle_catalog.json\n"
        "to preview the scene as it would load at runtime.\n"
        "Bundle-loaded scenes are read-only — Save will be ignored."));
    ImGui::Separator();

    bool path_changed = ImGui::InputText(BL("field.path", "Bundle / catalog path"),
                                         g_ob.path_input, sizeof(g_ob.path_input));
    ImGui::SameLine();
    if (ImGui::Button(BL("btn.browse", "Browse..."))) {
        static bool browse_ready = false, browse_cancel = false;
        open_file_dialog_async(BL("title", "Open Bundle"),
                               g_ob.path_input,
                               "Bundle / Catalog (*.jbundle *.json);;"
                               "Single bundle (*.jbundle);;"
                               "Bundle catalog (*.json);;"
                               "All Files (*.*)",
                               g_ob.path_input, sizeof(g_ob.path_input),
                               &browse_ready, &browse_cancel);
    }
    /* Apply browse result (set by pump on main thread) before detect. */
    {
        static char last_seen[1024] = {0};
        if (strcmp(last_seen, g_ob.path_input) != 0) {
            snprintf(last_seen, sizeof(last_seen), "%s", g_ob.path_input);
            if (!path_changed) path_changed = true;
        }
    }
    if (path_changed) {
        detect_mode();
        g_ob.status_msg[0] = '\0';
    }

    ImGui::TextDisabled("%s: %s", BL("label.detected", "Detected"),
        g_ob.is_catalog ? BL("label.catalog", "bundle catalog (.json)")
                     : BL("label.single",  "single-file (.jbundle)"));

    if (g_ob.is_catalog) {
        if (g_ob.cached_path != g_ob.path_input) {
            if (ImGui::Button(BL("btn.scan_catalog", "Scan catalog"))) {
                refresh_catalog_preview();
            }
        } else if (!g_ob.bundle_ids.empty()) {
            ImGui::TextUnformatted(BL("label.scene_bundles", "Scene bundles in catalog:"));
            if (ImGui::BeginListBox("##bundles", ImVec2(-FLT_MIN, 180))) {
                for (int i = 0; i < (int)g_ob.bundle_ids.size(); ++i) {
                    char line[512];
                    snprintf(line, sizeof(line), "%s   [%s]",
                             g_ob.bundle_ids[i].c_str(),
                             g_ob.scene_paths[i].c_str());
                    bool sel = (g_ob.selected_idx == i);
                    if (ImGui::Selectable(line, sel))
                        g_ob.selected_idx = i;
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
            detect_mode();
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
                *p_open = false;
            } else {
                snprintf(g_ob.status_msg, sizeof(g_ob.status_msg), "%s",
                         BL("msg.failed", "load failed (see Console)"));
            }
        }
    }
    if (!can_open) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(BL("btn.close", "Close"))) {
        *p_open = false;
    }

    if (g_ob.status_msg[0]) {
        ImGui::SameLine();
        ImGui::TextUnformatted(g_ob.status_msg);
    }

    ImGui::End();
}
