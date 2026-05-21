/*
 * jce_panel_memory_profiler.cpp  Memory Profiler panel (P3-A.5 rewrite).
 *
 *   Top strip:     process RSS / committed + mimalloc reserved / committed
 *   Tag table:     per-tag current / peak / live count / totals (sortable,
 *                  rows highlighted red when current > 64 MB)
 *   Baseline bar:  total tagged current vs 512 MB device-class baseline
 *   Buttons:       Reset Peaks · Snapshot (writes a timestamped JSON)
 *   Refresh combo: 0.1 / 0.5 / 1 / 5 s (default 0.5 s)
 *
 * Backed entirely by <jce/os/core/jce_mem_profile.h>; no platform calls,
 * no bgfx, no raw libc.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <cfloat>
#include <ctime>

extern "C" {
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
}

namespace {

constexpr uint64_t kBaselineBytes = 512ull * 1024ull * 1024ull;
constexpr uint64_t kHotTagBytes   =  64ull * 1024ull * 1024ull;
constexpr const char *LOG_TAG     = "MemProfiler";

struct TagRow {
    int             tag;
    const char     *name;
    JceMemTagStats  s;
};

struct PanelState {
    double             last_refresh = -1.0;
    float              refresh_rate = 0.5f;
    JceProcessMemStats proc{};
    TagRow             rows[JCE_MEM_TAG_COUNT_MAX];
    int                row_count = 0;
    uint64_t           total_cur = 0;
    uint64_t           total_peak = 0;
    char               snapshot_msg[160] = {0};
    double             snapshot_msg_time = -1.0;
};

PanelState &state()
{
    static PanelState s;
    return s;
}

float bytes_to_mb(uint64_t b)
{
    return (float)((double)b / (1024.0 * 1024.0));
}

void refresh(PanelState &st)
{
    jce_mem_profile_get_process_stats(&st.proc);
    st.row_count = 0;
    for (int i = 0; i < JCE_MEM_TAG_COUNT_MAX; ++i) {
        JceMemTagStats s;
        if (!jce_mem_profile_get_stats((JceMemTag)i, &s)) continue;
        if (s.total_alloc_count == 0 && s.current_bytes == 0) continue;
        st.rows[st.row_count].tag  = i;
        st.rows[st.row_count].name = jce_mem_profile_tag_name((JceMemTag)i);
        st.rows[st.row_count].s    = s;
        st.row_count++;
    }
    jce_mem_profile_get_total(&st.total_cur, &st.total_peak);
}

void apply_sort(PanelState &st, ImGuiTableSortSpecs *specs)
{
    if (!specs || specs->SpecsCount == 0) return;
    const ImGuiTableColumnSortSpecs &spec = specs->Specs[0];
    const int  col = spec.ColumnIndex;
    const bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
    auto key = [col](const TagRow &r) -> uint64_t {
        switch (col) {
            case 1: return r.s.current_bytes;
            case 2: return r.s.peak_bytes;
            case 3: return (uint64_t)r.s.live_alloc_count;
            case 4: return r.s.total_bytes_allocated;
            case 5: return (uint64_t)r.s.total_alloc_count;
            default: return 0;
        }
    };
    /* Tiny insertion sort: at most JCE_MEM_TAG_COUNT_MAX (=64) rows. */
    for (int i = 1; i < st.row_count; ++i) {
        TagRow x = st.rows[i];
        int    j = i - 1;
        while (j >= 0) {
            bool swap;
            if (col == 0) {
                int c = std::strcmp(st.rows[j].name, x.name);
                swap = asc ? (c > 0) : (c < 0);
            } else {
                uint64_t a = key(st.rows[j]), b = key(x);
                swap = asc ? (a > b) : (a < b);
            }
            if (!swap) break;
            st.rows[j + 1] = st.rows[j];
            --j;
        }
        st.rows[j + 1] = x;
    }
}

void write_snapshot(PanelState &st)
{
    char path[160];
    time_t now = std::time(nullptr);
    struct tm tm_local{};
#ifdef _MSC_VER
    localtime_s(&tm_local, &now);
#else
    /* localtime_r unavailable on some embedded toolchains; localtime is
     * thread-unsafe but acceptable for a one-shot editor button. */
    struct tm *p = std::localtime(&now);
    if (p) tm_local = *p;
#endif
    std::snprintf(path, sizeof(path),
                  "mem_snapshot_%04d%02d%02d_%02d%02d%02d.json",
                  tm_local.tm_year + 1900, tm_local.tm_mon + 1,
                  tm_local.tm_mday, tm_local.tm_hour,
                  tm_local.tm_min, tm_local.tm_sec);

    /* Build the JSON in a stack buffer; cap per-row ~120 bytes, plus
     * preamble. 64 rows * 120 + 512 ~= 8 KB. */
    char   body[12000];
    size_t off = 0;
    int n = std::snprintf(body + off, sizeof(body) - off,
                          "{\n  \"process\": {\n"
                          "    \"rss_bytes\": %llu,\n"
                          "    \"committed_bytes\": %llu,\n"
                          "    \"mimalloc_reserved_bytes\": %llu,\n"
                          "    \"mimalloc_committed_bytes\": %llu\n"
                          "  },\n  \"tags\": [\n",
                          (unsigned long long)st.proc.process_rss_bytes,
                          (unsigned long long)st.proc.process_committed_bytes,
                          (unsigned long long)st.proc.mimalloc_reserved_bytes,
                          (unsigned long long)st.proc.mimalloc_committed_bytes);
    if (n > 0) off += (size_t)n;

    for (int i = 0; i < st.row_count && off < sizeof(body) - 256; ++i) {
        const TagRow &r = st.rows[i];
        n = std::snprintf(body + off, sizeof(body) - off,
                          "    {\"name\":\"%s\",\"current\":%llu,"
                          "\"peak\":%llu,\"live\":%u,"
                          "\"total_alloc\":%llu,\"total_count\":%u}%s\n",
                          r.name,
                          (unsigned long long)r.s.current_bytes,
                          (unsigned long long)r.s.peak_bytes,
                          r.s.live_alloc_count,
                          (unsigned long long)r.s.total_bytes_allocated,
                          r.s.total_alloc_count,
                          (i + 1 < st.row_count) ? "," : "");
        if (n > 0) off += (size_t)n;
    }
    n = std::snprintf(body + off, sizeof(body) - off, "  ]\n}\n");
    if (n > 0) off += (size_t)n;

    bool ok = jce_fs_host_write_all(path, body, (uint64_t)off);
    if (ok) {
        std::snprintf(st.snapshot_msg, sizeof(st.snapshot_msg),
                      "snapshot -> %s", path);
        LOG_INFO(LOG_TAG, "memory snapshot written: %s (%zu bytes)",
                 path, (size_t)off);
    } else {
        std::snprintf(st.snapshot_msg, sizeof(st.snapshot_msg),
                      "snapshot FAILED (%s)", path);
        LOG_WARN(LOG_TAG, "memory snapshot write failed: %s", path);
    }
    st.snapshot_msg_time = ImGui::GetTime();
}

void format_mb(char *out, size_t cap, uint64_t bytes)
{
    std::snprintf(out, cap, "%.2f MB", bytes_to_mb(bytes));
}

} // namespace

extern "C" void jce_editor_panel_memory_profiler_content(void)
{
    PanelState &st = state();

    /* Refresh-rate combo. */
    {
        const char *labels[] = { "0.1 s", "0.5 s", "1 s", "5 s" };
        const float rates[]  = { 0.1f,    0.5f,    1.0f,  5.0f };
        int sel = 1;
        for (int i = 0; i < 4; ++i)
            if (st.refresh_rate == rates[i]) sel = i;
        ImGui::TextUnformatted(jce_editor_i18n("memoryProfiler.refreshRate"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96.0f);
        if (ImGui::Combo("##memprof_rate", &sel, labels, 4))
            st.refresh_rate = rates[sel];
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("memoryProfiler.resetPeaks")))
            jce_mem_profile_reset_peaks();
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("memoryProfiler.snapshot")))
            write_snapshot(st);
        if (st.snapshot_msg_time > 0.0
            && ImGui::GetTime() - st.snapshot_msg_time < 4.0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", st.snapshot_msg);
        }
    }

    /* Refresh on schedule (or first frame). */
    const double now = ImGui::GetTime();
    if (st.last_refresh < 0.0 || (now - st.last_refresh) >= st.refresh_rate) {
        refresh(st);
        st.last_refresh = now;
    }

    ImGui::Separator();

    /* Top strip: process / mimalloc. */
    char buf[64];
    ImGui::TextUnformatted(jce_editor_i18n("memoryProfiler.process"));
    ImGui::Indent();
    format_mb(buf, sizeof(buf), st.proc.process_rss_bytes);
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.processRss"), buf);
    format_mb(buf, sizeof(buf), st.proc.process_committed_bytes);
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.processCommitted"), buf);
    format_mb(buf, sizeof(buf), st.proc.mimalloc_reserved_bytes);
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.mimallocReserved"), buf);
    format_mb(buf, sizeof(buf), st.proc.mimalloc_committed_bytes);
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.mimallocCommitted"), buf);
    ImGui::Unindent();

    ImGui::Separator();

    /* Baseline bar (tagged total vs 512 MB). */
    {
        const float fraction = (float)((double)st.total_cur
                                       / (double)kBaselineBytes);
        const float clamped  = fraction > 1.0f ? 1.0f : fraction;
        char ov[96];
        std::snprintf(ov, sizeof(ov), "%.2f / 512.00 MB  (%.0f%%)",
                      bytes_to_mb(st.total_cur), fraction * 100.0f);
        ImVec4 col = (fraction > 1.0f)
                         ? ImVec4(0.85f, 0.20f, 0.20f, 1.0f)
                         : (fraction > 0.75f)
                               ? ImVec4(0.85f, 0.65f, 0.10f, 1.0f)
                               : ImVec4(0.30f, 0.65f, 0.30f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
        ImGui::ProgressBar(clamped, ImVec2(-FLT_MIN, 0), ov);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s",
                            jce_editor_i18n("memoryProfiler.baselineMarker"));
    }

    ImGui::Separator();

    /* Tag table. */
    const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH
                                | ImGuiTableFlags_BordersOuter
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_Sortable
                                | ImGuiTableFlags_SizingStretchProp
                                | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##memprof_tags", 6, flags,
                          ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.tag"),
                                ImGuiTableColumnFlags_DefaultSort, 1.4f);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.currentMb"),
                                ImGuiTableColumnFlags_None, 1.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.peakMb"),
                                ImGuiTableColumnFlags_None, 1.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.liveCount"),
                                ImGuiTableColumnFlags_None, 0.8f);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.totalMb"),
                                ImGuiTableColumnFlags_None, 1.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("memoryProfiler.col.totalCount"),
                                ImGuiTableColumnFlags_None, 0.9f);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty) {
                apply_sort(st, specs);
                specs->SpecsDirty = false;
            }
        }

        for (int i = 0; i < st.row_count; ++i) {
            const TagRow &r = st.rows[i];
            ImGui::TableNextRow();
            if (r.s.current_bytes > kHotTagBytes) {
                ImU32 bg = ImGui::GetColorU32(ImVec4(0.45f, 0.10f, 0.10f, 0.55f));
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bg);
            }
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.2f", bytes_to_mb(r.s.current_bytes));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.2f", bytes_to_mb(r.s.peak_bytes));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", r.s.live_alloc_count);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f", bytes_to_mb(r.s.total_bytes_allocated));
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%u", r.s.total_alloc_count);
        }
        ImGui::EndTable();
    }
}

/* Shim: Memory Profiler has been merged into the Profiler "Profiling"
 * workbench as a tab.  Activating this panel now redirects to that
 * workbench and requests the Memory tab.  Symbol kept so menu/hotkey
 * entries registered against JCE_PANEL_MEMORY_PROFILER keep working. */
extern "C" void jce_editor_panel_memory_profiler(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_MEMORY_PROFILER);
    if (!vis || !*vis) return;
    *vis = false;

    bool *pf_vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER);
    if (pf_vis) *pf_vis = true;

    char title[96];
    snprintf(title, sizeof(title), "%s###profiler",
             jce_editor_i18n("panel.profiler"));
    ImGui::SetWindowFocus(title);
    jce_panel_profiler_request_tab(1);
}
