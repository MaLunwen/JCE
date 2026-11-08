/*
 * jce_panel_memory_profiler.cpp  Memory Profiler panel (P3-A.5 rewrite).
 *
 *   Top strip:     process RSS / committed + mimalloc reserved / committed
 *   Tag table:     per-tag current / peak / live count / totals (sortable,
 *                  rows highlighted red when current > 64 MB)
 *   Baseline bar:  process RSS vs 512 MiB process target; tags are partial
 *   Buttons:       Reset Peaks · Snapshot (writes a timestamped JSON)
 *   Refresh combo: 0.1 / 0.5 / 1 / 5 s (default 0.5 s)
 *
 * Backed entirely by <jce/os/core/jce_mem_profile.h>; no platform calls,
 * no bgfx, no raw libc.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_ui_state.h"
#include "ui/jce_editor_colors.h"
#include "jce_panel_memory_snapshot.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>
#include <cfloat>

extern "C" {
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
}

namespace {

constexpr uint64_t kBaselineBytes = JCE_EDITOR_MEMORY_TARGET_BYTES;
constexpr uint64_t kHotTagBytes   =  64ull * 1024ull * 1024ull;
constexpr const char *LOG_TAG     = "MemProfiler";

using TagRow = JceEditorMemoryTagRow;
struct PanelState : JceEditorMemorySnapshot {
    double last_refresh=-1.0;
    float refresh_rate=0.5f;
    char snapshot_path[1024]={0};
    bool snapshot_written=false;
    double snapshot_msg_time=-1.0;
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
    st.proc={};
    st.process_valid=jce_mem_profile_get_process_stats(&st.proc);
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
    jce_mem_profile_get_aggregate_totals(&st.total_cur, &st.total_peak);
    jce_mem_profile_get_total(nullptr, &st.tag_peak_sum);
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
    refresh(st); /* Snapshot the current observation, including availability. */
    const auto result=jce_editor_memory_snapshot_publish(st,st.snapshot_path,sizeof st.snapshot_path);
    st.snapshot_written=result==JceEditorMemorySnapshotResult::Written;
    if (st.snapshot_written)
        LOG_INFO(LOG_TAG,"complete memory snapshot written: %s",st.snapshot_path);
    else
        LOG_WARN(LOG_TAG,"memory snapshot failed: result=%d path=%s",static_cast<int>(result),st.snapshot_path);
    st.snapshot_msg_time=ImGui::GetTime();
}

void format_mb(char *out, size_t cap, uint64_t bytes)
{
    std::snprintf(out, cap, "%.2f MiB", bytes_to_mb(bytes));
}

} // namespace

extern "C" void jce_editor_panel_memory_profiler_content(void)
{
    PanelState &st = state();

    /* Refresh-rate combo. */
    {
        const char *labels[] = { "0.1 s", "0.5 s", "1 s", "5 s" };
        const float rates[]  = { 0.1f,    0.5f,    1.0f,  5.0f };

        /* One-time restore of the persisted rate (user-global; stored as
         * the combo index so it always maps to a valid rate). */
        static bool s_rate_loaded = false;
        if (!s_rate_loaded) {
            s_rate_loaded = true;
            st.refresh_rate = rates[jce_editor_ui_state_load_int(
                                        "memprof.refresh_idx", 1, 0, 3)];
        }

        int sel = 1;
        for (int i = 0; i < 4; ++i)
            if (st.refresh_rate == rates[i]) sel = i;
        ImGui::TextUnformatted(jce_editor_i18n("memoryProfiler.refreshRate"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(96.0f);
        if (ImGui::Combo("##memprof_rate", &sel, labels, 4)) {
            st.refresh_rate = rates[sel];
            jce_editor_ui_state_save_int("memprof.refresh_idx", sel);
        }
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("memoryProfiler.resetPeaks")))
            jce_mem_profile_reset_peaks();
        ImGui::SameLine();
        if (ImGui::Button(jce_editor_i18n("memoryProfiler.snapshot")))
            write_snapshot(st);
        if (st.snapshot_msg_time > 0.0
            && ImGui::GetTime() - st.snapshot_msg_time < 4.0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s: %s",jce_editor_i18n(st.snapshot_written?
                "memoryProfiler.snapshotWritten":"memoryProfiler.snapshotFailed"),st.snapshot_path);
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
    if (st.process_valid && st.proc.process_rss_bytes)
        format_mb(buf,sizeof buf,st.proc.process_rss_bytes);
    else std::snprintf(buf,sizeof buf,"%s",jce_editor_i18n("memoryProfiler.unavailable"));
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.processRss"), buf);
    if (st.process_valid && st.proc.process_committed_bytes)
        format_mb(buf,sizeof buf,st.proc.process_committed_bytes);
    else std::snprintf(buf,sizeof buf,"%s",jce_editor_i18n("memoryProfiler.unavailable"));
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.processCommitted"), buf);
    if (st.process_valid && st.proc.mimalloc_reserved_bytes)
        format_mb(buf, sizeof(buf), st.proc.mimalloc_reserved_bytes);
    else
        std::snprintf(buf,sizeof buf,"%s",jce_editor_i18n("memoryProfiler.unavailable"));
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.mimallocReserved"), buf);
    if (st.process_valid && st.proc.mimalloc_committed_bytes)
        format_mb(buf, sizeof(buf), st.proc.mimalloc_committed_bytes);
    else
        std::snprintf(buf,sizeof buf,"%s",jce_editor_i18n("memoryProfiler.unavailable"));
    ImGui::Text("%s: %s",
                jce_editor_i18n("memoryProfiler.mimallocCommitted"), buf);
    ImGui::Unindent();

    ImGui::Separator();

    /* This target has process RSS scope. Voluntary tags are not device usage. */
    {
        const uint64_t rss=jce_editor_memory_budget_rss(st);
        if (rss) {
            const float fraction=static_cast<float>(static_cast<double>(rss)/static_cast<double>(kBaselineBytes));
            char overlay[128];
            std::snprintf(overlay,sizeof overlay,jce_editor_i18n("memoryProfiler.processTargetFormat"),
                bytes_to_mb(rss),fraction*100.0f);
            ImVec4 color=ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);
            if (fraction>1) color=JCE_COLOR_TEXT_ERROR;
            else if (fraction>.75f) color=JCE_COLOR_TEXT_WARNING;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,color);
            ImGui::ProgressBar(fraction>1?1:fraction,ImVec2(-FLT_MIN,0),overlay);
            ImGui::PopStyleColor();
        } else ImGui::TextDisabled("%s",jce_editor_i18n("memoryProfiler.processTargetUnavailable"));
        ImGui::TextDisabled("%s",jce_editor_i18n("memoryProfiler.baselineMarker"));
        ImGui::Text("%s: %.2f MiB",jce_editor_i18n("memoryProfiler.taggedPartial"),bytes_to_mb(st.total_cur));
        if (st.row_count==0)
            ImGui::TextDisabled("%s",jce_editor_i18n("memoryProfiler.noTaggedRows"));
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
            /* Re-apply EVERY frame: refresh() rebuilds rows in tag order on
             * its 0.5s cadence, silently reverting a dirty-only sort. */
            apply_sort(st, specs);
            specs->SpecsDirty = false;
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
    if (jce_panel_redirect_to_workbench(JCE_PANEL_MEMORY_PROFILER,
                                        JCE_PANEL_PROFILER,
                                        "panel.profiler", "profiler"))
        jce_panel_profiler_request_tab(1);
}
