/*
 * jce_panel_build_report.cpp  Asset Build Report (P3-A.4).
 *
 * Unity parity: Window > Analysis > Build Report.
 *
 * Reads a build_report.json (emitted by jce_bundle_pack alongside
 * bundle_catalog.json) and visualizes per-bundle totals, per-asset
 * entries, hash-duplicate clusters and a Top-N by size view.
 *
 * The panel is read-only: it never modifies the report.  Refresh
 * (Load) re-reads the file from disk; nothing is held open between
 * frames so a background packer can rewrite it freely.
 *
 * Schema: jce.buildreport.v1 (see engine/src/resource/jce_bundle_pack.c).
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
}

/* jce_editor_assets_get_project() lives in editor C runtime. */
extern "C" const char *jce_editor_assets_get_project(void);

namespace {

struct EntryRow {
    std::string path;
    std::string hash;
    std::string type;
    uint64_t    size_bytes = 0;
};

struct BundleRow {
    std::string             id;
    std::string             file;
    std::string             hash;
    uint64_t                size_bytes  = 0;
    uint32_t                entry_count = 0;
    uint32_t                dep_count   = 0;
    bool                    encrypted   = false;
    std::vector<EntryRow>   entries;
};

struct DuplicateRow {
    std::string              hash;
    uint64_t                 size_bytes = 0;
    std::vector<std::string> bundles;
};

struct State {
    char                       report_path[1024] = {0};
    bool                       path_initialized  = false;
    bool                       loaded            = false;
    char                       status_msg[256]   = {0};

    std::string                schema;
    std::string                timestamp;
    uint64_t                   total_bytes     = 0;
    uint32_t                   unique_assets   = 0;
    std::vector<BundleRow>     bundles;
    std::vector<DuplicateRow>  duplicates;

    /* Flattened all-entries view (rebuilt at load). */
    struct FlatEntry { uint32_t bundle_idx; uint32_t entry_idx; };
    std::vector<FlatEntry>     flat_entries;

    char                       filter[128]     = {0};
    int                        top_n           = 25;
};
State g_st;

JcePanelTabState g_tabs{ "panel.build_report.current_tab", /*max_tab=*/4 };

void init_default_path()
{
    if (g_st.path_initialized) return;
    g_st.path_initialized = true;
    const char *proj = jce_editor_assets_get_project();
    if (proj && *proj) {
        snprintf(g_st.report_path, sizeof(g_st.report_path),
                 "%s/bundles/build_report.json", proj);
    }
}

void reset_state()
{
    g_st.loaded = false;
    g_st.schema.clear();
    g_st.timestamp.clear();
    g_st.total_bytes   = 0;
    g_st.unique_assets = 0;
    g_st.bundles.clear();
    g_st.duplicates.clear();
    g_st.flat_entries.clear();
}

/* std::string overload of the shared filter test — the report rows are
 * all std::string, so this keeps the call sites free of .c_str(). */
bool ci_contains(const std::string &hay, const char *needle)
{
    return jce_panel_filter_match_ci(hay.c_str(), needle);
}

void refresh()
{
    reset_state();
    g_st.status_msg[0] = '\0';

    if (g_st.report_path[0] == '\0') {
        snprintf(g_st.status_msg, sizeof(g_st.status_msg), "%s",
                 jce_editor_i18n("panel.build_report.status.no_path"));
        return;
    }
    if (!jce_fs_host_exists_file(g_st.report_path)) {
        snprintf(g_st.status_msg, sizeof(g_st.status_msg), "%s",
                 jce_editor_i18n("panel.build_report.status.not_found"));
        return;
    }

    uint64_t sz = 0;
    void *buf = jce_fs_host_read_all(g_st.report_path, &sz);
    if (!buf || sz == 0) {
        if (buf) jce_fs_buffer_free(buf);
        snprintf(g_st.status_msg, sizeof(g_st.status_msg), "%s",
                 jce_editor_i18n("panel.build_report.status.read_failed"));
        return;
    }

    JceJson *root = jce_json_parse((const char *)buf, (size_t)sz);
    jce_fs_buffer_free(buf);
    if (!root) {
        snprintf(g_st.status_msg, sizeof(g_st.status_msg), "%s",
                 jce_editor_i18n("panel.build_report.status.parse_failed"));
        return;
    }

    if (const char *s = jce_json_get_string(root, "$schema", nullptr)) g_st.schema = s;
    if (const char *s = jce_json_get_string(root, "timestamp", nullptr)) g_st.timestamp = s;
    const JceJson *totals = jce_json_get(root, "totals");
    if (jce_json_is_object(totals)) {
        g_st.total_bytes   = (uint64_t)jce_json_get_number(totals, "total_bytes", 0.0);
        g_st.unique_assets = (uint32_t)jce_json_get_number(totals, "unique_assets", 0.0);
    }

    const JceJson *bundles = jce_json_get(root, "bundles");
    if (jce_json_is_array(bundles)) {
        int bn = jce_json_array_size(bundles);
        for (int bi = 0; bi < bn; ++bi) {
            const JceJson *b = jce_json_array_at(bundles, bi);
            if (!jce_json_is_object(b)) continue;
            BundleRow br;
            if (const char *s = jce_json_get_string(b, "id", nullptr))   br.id = s;
            if (const char *s = jce_json_get_string(b, "file", nullptr)) br.file = s;
            if (const char *s = jce_json_get_string(b, "hash", nullptr)) br.hash = s;
            br.size_bytes   = (uint64_t)jce_json_get_number(b, "size_bytes", 0.0);
            br.entry_count  = (uint32_t)jce_json_get_number(b, "entry_count", 0.0);
            br.dep_count    = (uint32_t)jce_json_get_number(b, "dep_count", 0.0);
            br.encrypted    = jce_json_get_bool(b, "encrypted", false);
            const JceJson *entries = jce_json_get(b, "entries");
            if (jce_json_is_array(entries)) {
                int en = jce_json_array_size(entries);
                for (int ei = 0; ei < en; ++ei) {
                    const JceJson *e = jce_json_array_at(entries, ei);
                    if (!jce_json_is_object(e)) continue;
                    EntryRow er;
                    if (const char *s = jce_json_get_string(e, "path", nullptr)) er.path = s;
                    if (const char *s = jce_json_get_string(e, "hash", nullptr)) er.hash = s;
                    if (const char *s = jce_json_get_string(e, "type", nullptr)) er.type = s;
                    er.size_bytes = (uint64_t)jce_json_get_number(e, "size_bytes", 0.0);
                    br.entries.push_back(std::move(er));
                }
            }
            g_st.bundles.push_back(std::move(br));
        }
    }

    const JceJson *dups = jce_json_get(root, "duplicates");
    if (jce_json_is_array(dups)) {
        int dn = jce_json_array_size(dups);
        for (int di = 0; di < dn; ++di) {
            const JceJson *d = jce_json_array_at(dups, di);
            if (!jce_json_is_object(d)) continue;
            DuplicateRow dr;
            if (const char *s = jce_json_get_string(d, "hash", nullptr)) dr.hash = s;
            dr.size_bytes = (uint64_t)jce_json_get_number(d, "size_bytes", 0.0);
            const JceJson *db = jce_json_get(d, "bundles");
            if (jce_json_is_array(db)) {
                int dbn = jce_json_array_size(db);
                for (int k = 0; k < dbn; ++k) {
                    const char *s = jce_json_string_value(jce_json_array_at(db, k), nullptr);
                    if (s) dr.bundles.emplace_back(s);
                }
            }
            g_st.duplicates.push_back(std::move(dr));
        }
    }

    jce_json_free(root);

    /* Build flat entries index for the Entries / Top-N tabs. */
    for (uint32_t bi = 0; bi < (uint32_t)g_st.bundles.size(); ++bi) {
        const BundleRow &br = g_st.bundles[bi];
        for (uint32_t ei = 0; ei < (uint32_t)br.entries.size(); ++ei)
            g_st.flat_entries.push_back({ bi, ei });
    }

    g_st.loaded = true;
    char sb[32];
    jce_panel_fmt_size(g_st.total_bytes, sb, sizeof(sb));
    snprintf(g_st.status_msg, sizeof(g_st.status_msg),
             jce_editor_i18n("panel.build_report.status.loaded"),
             (int)g_st.bundles.size(), sb);
}

void draw_overview()
{
    if (ImGui::BeginTable("##br_ov", 2,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        char sb[32];

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.schema"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(g_st.schema.empty() ? "-" : g_st.schema.c_str());

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.timestamp"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(g_st.timestamp.empty() ? "-" : g_st.timestamp.c_str());

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.bundle_count"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", (int)g_st.bundles.size());

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.total_size"));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(jce_panel_fmt_size(g_st.total_bytes, sb, sizeof(sb)));

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.unique_assets"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%u", g_st.unique_assets);

        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.duplicate_clusters"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d", (int)g_st.duplicates.size());

        int enc_count = 0;
        for (const auto &b : g_st.bundles)
            if (b.encrypted) ++enc_count;
        ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(jce_editor_i18n("panel.build_report.overview.encrypted_bundles"));
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%d / %d", enc_count, (int)g_st.bundles.size());

        ImGui::EndTable();
    }
}

void draw_bundles()
{
    if (ImGui::BeginTable("##br_bundles", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.name"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.size"),
                                ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.entry_count"),
                                ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.dep_count"),
                                ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.encrypted"),
                                ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.hash"),
                                ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        char sb[32];
        for (const auto &br : g_st.bundles) {
            if (!ci_contains(br.id, g_st.filter) &&
                !ci_contains(br.file, g_st.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(br.id.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(jce_panel_fmt_size(br.size_bytes, sb, sizeof(sb)));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", br.entry_count);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", br.dep_count);
            ImGui::TableSetColumnIndex(4);
            if (br.encrypted)
                ImGui::TextUnformatted(
                    jce_editor_i18n("panel.build_report.encrypted_yes"));
            else
                ImGui::TextDisabled("-");
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(br.hash.empty() ? "-" : br.hash.c_str());
        }
        ImGui::EndTable();
    }
}

void draw_entries()
{
    if (ImGui::BeginTable("##br_entries", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.path"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.type"),
                                ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.size"),
                                ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.bundle"),
                                ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.hash"),
                                ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        char sb[32];
        for (const auto &fe : g_st.flat_entries) {
            const BundleRow &br = g_st.bundles[fe.bundle_idx];
            const EntryRow  &er = br.entries[fe.entry_idx];
            if (!ci_contains(er.path, g_st.filter) &&
                !ci_contains(br.id, g_st.filter)) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(er.path.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(er.type.empty() ? "-" : er.type.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(jce_panel_fmt_size(er.size_bytes, sb, sizeof(sb)));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(br.id.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(er.hash.empty() ? "-" : er.hash.c_str());
        }
        ImGui::EndTable();
    }
}

void draw_duplicates()
{
    if (g_st.duplicates.empty()) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.build_report.dup.none"));
        return;
    }
    if (ImGui::BeginTable("##br_dups", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.hash"),
                                ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.size"),
                                ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.dup_count"),
                                ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.dup_bundles"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        char sb[32];
        for (const auto &dr : g_st.duplicates) {
            if (!ci_contains(dr.hash, g_st.filter)) {
                bool any = false;
                for (const auto &b : dr.bundles) if (ci_contains(b, g_st.filter)) { any = true; break; }
                if (!any && g_st.filter[0]) continue;
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(dr.hash.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(jce_panel_fmt_size(dr.size_bytes, sb, sizeof(sb)));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%u", (uint32_t)dr.bundles.size());
            ImGui::TableSetColumnIndex(3);
            std::string joined;
            for (size_t i = 0; i < dr.bundles.size(); ++i) {
                if (i) joined += ", ";
                joined += dr.bundles[i];
            }
            ImGui::TextUnformatted(joined.c_str());
        }
        ImGui::EndTable();
    }
}

void draw_top_n()
{
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("##br_top_n", &g_st.top_n, 1, 200, "%d");

    /* Sort indices by entry size desc. */
    std::vector<uint32_t> idx(g_st.flat_entries.size());
    for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [](uint32_t a, uint32_t b) {
        const auto &fa = g_st.flat_entries[a];
        const auto &fb = g_st.flat_entries[b];
        uint64_t sa = g_st.bundles[fa.bundle_idx].entries[fa.entry_idx].size_bytes;
        uint64_t sb = g_st.bundles[fb.bundle_idx].entries[fb.entry_idx].size_bytes;
        return sa > sb;
    });
    int limit = g_st.top_n;
    if (limit > (int)idx.size()) limit = (int)idx.size();

    uint64_t max_size = 0;
    if (limit > 0) {
        const auto &fe0 = g_st.flat_entries[idx[0]];
        max_size = g_st.bundles[fe0.bundle_idx].entries[fe0.entry_idx].size_bytes;
    }
    if (max_size == 0) max_size = 1;

    if (ImGui::BeginTable("##br_topn", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.path"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.size"),
                                ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.bundle"),
                                ImGuiTableColumnFlags_WidthFixed, 180);
        ImGui::TableSetupColumn(jce_editor_i18n("panel.build_report.col.bar"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        char sb[32];
        for (int i = 0; i < limit; ++i) {
            const auto &fe = g_st.flat_entries[idx[i]];
            const BundleRow &br = g_st.bundles[fe.bundle_idx];
            const EntryRow  &er = br.entries[fe.entry_idx];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(er.path.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(jce_panel_fmt_size(er.size_bytes, sb, sizeof(sb)));
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(br.id.c_str());
            ImGui::TableSetColumnIndex(3);
            float frac = (float)((double)er.size_bytes / (double)max_size);
            ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 0), "");
        }
        ImGui::EndTable();
    }
}

} /* namespace */

extern "C" void build_report_draw_content(void)
{
    init_default_path();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::InputText("##br_path",
                     g_st.report_path, sizeof(g_st.report_path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("panel.build_report.load"))) {
        refresh();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##br_filter",
        jce_editor_i18n("panel.build_report.filter"),
        g_st.filter, sizeof(g_st.filter));

    if (g_st.status_msg[0]) {
        ImVec4 col = g_st.loaded
            ? ImVec4(0.6f, 1.0f, 0.6f, 1.0f)
            : ImVec4(1.0f, 0.5f, 0.5f, 1.0f);
        ImGui::TextColored(col, "%s", g_st.status_msg);
    }

    ImGui::Separator();

    if (!g_st.loaded) {
        ImGui::TextDisabled("%s",
            jce_editor_i18n("panel.build_report.no_report_loaded"));
        return;
    }

    jce_panel_tab_ensure_loaded(g_tabs);
    if (ImGui::BeginTabBar("##br_tabs")) {
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.build_report.tab.overview"),
                                nullptr, jce_panel_tab_flags(g_tabs, 0))) {
            jce_panel_tab_set_current(g_tabs, 0);
            draw_overview();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.build_report.tab.bundles"),
                                nullptr, jce_panel_tab_flags(g_tabs, 1))) {
            jce_panel_tab_set_current(g_tabs, 1);
            draw_bundles();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.build_report.tab.entries"),
                                nullptr, jce_panel_tab_flags(g_tabs, 2))) {
            jce_panel_tab_set_current(g_tabs, 2);
            draw_entries();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.build_report.tab.duplicates"),
                                nullptr, jce_panel_tab_flags(g_tabs, 3))) {
            jce_panel_tab_set_current(g_tabs, 3);
            draw_duplicates();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(jce_editor_i18n("panel.build_report.tab.top_n"),
                                nullptr, jce_panel_tab_flags(g_tabs, 4))) {
            jce_panel_tab_set_current(g_tabs, 4);
            draw_top_n();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        g_tabs.request = -1;
    }
}

/* Shim: Build Report has been merged into the Build Profiles workbench
 * as a tab.  Activating this panel now redirects to that workbench and
 * requests the Report tab.  Symbol kept so the menu/hotkey entries
 * registered against JCE_PANEL_BUILD_REPORT keep working. */
extern "C" void jce_editor_panel_build_report(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_BUILD_REPORT,
                                        JCE_PANEL_BUILD_PROFILES,
                                        "buildProfiles.title",
                                        "build_profiles"))
        jce_panel_build_profiles_request_tab(1);
}

/* Back-compat alias for callers that referenced the old _content symbol. */
extern "C" void jce_editor_panel_build_report_content(void)
{
    build_report_draw_content();
}
