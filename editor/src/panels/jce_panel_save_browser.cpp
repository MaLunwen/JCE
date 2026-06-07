/*
 * jce_panel_save_browser.cpp  Snapshot / Save File Browser.
 *
 * Lists *.jsnp files in the project's "saves" directory (scanned
 * recursively, capped). Provides metadata via jce_snapshot_peek_header
 * and a refresh button, plus a per-row Load action.
 *
 * Loading rehydrates the CURRENT edit scene from the snapshot's scene/ECS
 * section: a registry is built with the engine's scene provider
 * (jce_save_register_scene_provider) against jce_state_get_scene(), Play is
 * stopped first (a snapshot load swaps scene content out from under a running
 * runtime), and the editor entity order / selection are rebuilt afterwards.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_state_internal.h"  /* rebuild_entity_order_from_ecs */

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
}

namespace {

struct SaveRow {
    std::string             path;
    uint64_t                size_bytes = 0;
    bool                    valid = false;
    JceSnapshotHeaderInfo   hdr{};
};

constexpr int kMaxRows = 200;

struct ScanCtx {
    std::vector<SaveRow> *rows;
};

bool ends_with(const char *str, const char *suffix)
{
    size_t ls = std::strlen(str), lf = std::strlen(suffix);
    if (lf > ls) return false;
    return std::strcmp(str + (ls - lf), suffix) == 0;
}

bool walk_cb(const char *path, bool is_dir, void *user)
{
    auto *ctx = static_cast<ScanCtx *>(user);
    if (is_dir) return true;
    if ((int)ctx->rows->size() >= kMaxRows) return false;
    if (!ends_with(path, ".jsnp")) return true;

    SaveRow r;
    r.path = path;
    jce_fs_host_get_size(path, &r.size_bytes);

    uint64_t got = 0;
    void *buf = jce_fs_host_read_capped(path, 64, &got, nullptr);
    if (buf) {
        r.valid = jce_snapshot_peek_header(buf, (size_t)got, &r.hdr);
        jce_fs_buffer_free(buf);
    }
    ctx->rows->push_back(std::move(r));
    return true;
}

std::vector<SaveRow> g_rows;
std::string          g_root;
bool                 g_loaded = false;
std::string          g_status;     /* last load result, shown under the table */
bool                 g_status_ok = false;

/* Rehydrate the current edit scene from `path`'s scene/ECS section.  Stops
 * Play first (a load swaps scene content from under the runtime), builds a
 * registry with the engine scene provider against the live scene, loads, then
 * rebuilds the editor entity order + clears selection. */
bool load_snapshot(const char *path)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { g_status = jce_editor_i18n("saveBrowser.noScene"); g_status_ok = false; return false; }

    if (jce_state_get_play_state() != JCE_PLAY_STOPPED)
        jce_state_stop();

    /* A snapshot Load replaces the entire scene.  Wrap it in a batch edit so the
     * pre-load scene is captured as ONE undoable step (Ctrl+Z restores it; a
     * failed/partial load is likewise recoverable). */
    jce_state_begin_batch_edit();

    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    if (!reg) {
        g_status = jce_editor_i18n("saveBrowser.regFail"); g_status_ok = false;
        jce_state_end_batch_edit();
        return false;
    }
    jce_save_register_scene_provider(reg, scene);

    bool ok = jce_snapshot_load_from_file(reg, path);
    jce_snapshot_registry_destroy(reg);

    if (ok) {
        rebuild_entity_order_from_ecs();
        jce_state_clear_selection();
        jce_state_mark_scene_modified();
    }
    jce_state_end_batch_edit();

    g_status_ok = ok;
    g_status    = std::string(jce_editor_i18n(ok ? "saveBrowser.loaded"
                                                 : "saveBrowser.loadFailed"))
                + " " + path;
    return ok;
}

void rescan()
{
    g_rows.clear();
    const char *proj = jce_editor_assets_get_project();
    if (!proj || !*proj) { g_root.clear(); g_loaded = true; return; }

    g_root = std::string(proj) + "/saves";
    if (!jce_fs_host_exists_dir(g_root.c_str())) { g_loaded = true; return; }

    ScanCtx ctx{ &g_rows };
    jce_fs_host_walk(g_root.c_str(), &walk_cb, &ctx);
    g_loaded = true;
}

const char *fmt_size(uint64_t b, char *out, size_t n)
{
    const char *u[] = {"B","KB","MB","GB"};
    double v = (double)b; int k = 0;
    while (v >= 1024.0 && k < 3) { v /= 1024.0; k++; }
    snprintf(out, n, "%.2f %s", v, u[k]);
    return out;
}

} /* namespace */

extern "C" void jce_editor_panel_save_browser_content(void)
{
    if (!g_loaded) rescan();

    if (ImGui::Button(jce_editor_i18n("common.refresh"))) rescan();
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", jce_editor_i18n("saveBrowser.root"), g_root.empty() ? "(none)" : g_root.c_str());

    ImGui::Separator();
    if (g_rows.empty()) {
        ImGui::TextDisabled(jce_editor_i18n("saveBrowser.empty"));
        return;
    }

    if (ImGui::BeginTable("##save_tbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("File",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Format",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Sections", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Action",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        char sb[32];
        int row_id = 0;
        for (auto &r : g_rows) {
            ImGui::TableNextRow();
            ImGui::PushID(row_id++);
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", fmt_size(r.size_bytes, sb, sizeof(sb)));
            if (r.valid) {
                ImGui::TableSetColumnIndex(2); ImGui::Text("v%u",  r.hdr.format_version);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%u",   r.hdr.section_count);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1.0f), "OK");
                ImGui::TableSetColumnIndex(5);
                if (ImGui::SmallButton(jce_editor_i18n("saveBrowser.load")))
                    load_snapshot(r.path.c_str());
            } else {
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f), jce_editor_i18n("saveBrowser.bad"));
                ImGui::TableSetColumnIndex(5); ImGui::TextDisabled("-");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!g_status.empty()) {
        ImGui::Separator();
        ImGui::TextColored(g_status_ok ? ImVec4(0.4f,1.0f,0.4f,1.0f)
                                       : ImVec4(1.0f,0.4f,0.4f,1.0f),
                           "%s", g_status.c_str());
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("saveBrowser.runtimeNote"));
}
