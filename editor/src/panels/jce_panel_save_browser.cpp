/*
 * jce_panel_save_browser.cpp  Snapshot / Save File Browser.
 *
 * Lists *.jsnp files in the project's "saves" directory (scanned
 * recursively, capped). Provides metadata via jce_snapshot_peek_header
 * and a refresh button. This is a read-only browser; actually loading
 * a snapshot requires a registry that knows what subsystems to rehydrate
 * — that wiring belongs to the game runtime, not the editor.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <jce/middleware/save/jce_snapshot.h>
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

bool ends_with(const char *s, const char *suffix)
{
    size_t ls = std::strlen(s), lf = std::strlen(suffix);
    if (lf > ls) return false;
    return std::strcmp(s + (ls - lf), suffix) == 0;
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

    if (ImGui::BeginTable("##save_tbl", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("File",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size",     ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Format",   ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Sections", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();

        char sb[32];
        for (auto &r : g_rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.path.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%s", fmt_size(r.size_bytes, sb, sizeof(sb)));
            if (r.valid) {
                ImGui::TableSetColumnIndex(2); ImGui::Text("v%u",  r.hdr.format_version);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%u",   r.hdr.section_count);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1.0f), "OK");
            } else {
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f), jce_editor_i18n("saveBrowser.bad"));
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", jce_editor_i18n("saveBrowser.runtimeNote"));
}
