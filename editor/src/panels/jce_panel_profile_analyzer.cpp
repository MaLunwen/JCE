/*
 * jce_panel_profile_analyzer.cpp  Profile Analyzer editor panel (P4-D.4).
 *
 * Provides post-hoc analysis on top of the lightweight profiler data:
 *   - Rolling 256-frame ring buffer of frame-time, CPU, GPU, wait.
 *   - Category breakdown bar (CPU frame / render submit / GPU / wait).
 *   - Spike detector: frames exceeding a configurable threshold are
 *     highlighted in red and listed in a table.
 *   - Complete CSV export, atomically published under the editor profile.
 *
 * Data is sampled from ImGui DeltaTime + jce_gfx_stats_capture() so the
 * panel works with or without Tracy.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_ui_state.h"
#include "jce_profiler_observation.h"
#include "jce_panel_profile_analyzer.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

/* -------------------------------------------------------------------- */
/* Constants and ring buffer                                            */
/* -------------------------------------------------------------------- */

constexpr int   kBufLen     = JCE_EDITOR_PROFILE_ANALYZER_CAPACITY;
constexpr float kDefaultMs  = 33.3f;   /* spike threshold default (~30 fps) */

using FrameRecord = JceEditorProfileAnalyzerRecord;

struct State {
    FrameRecord buf[kBufLen]  = {};
    int         head          = 0;
    int         filled        = 0;
    float       spike_thresh  = kDefaultMs;
    bool        paused        = false;
    bool        show_spikes   = true;
    uint64_t    next_frame_no = 1;
    bool        export_attempted = false;
    JceEditorProfileAnalyzerExportResult export_result =
        JceEditorProfileAnalyzerExportResult::Empty;
    char        export_path[1024] = {};
};

State s;

/* -------------------------------------------------------------------- */
/* Ring buffer helpers                                                   */
/* -------------------------------------------------------------------- */

void push_record(const FrameRecord &r)
{
    s.buf[s.head] = r;
    s.head        = (s.head + 1) % kBufLen;
    if (s.filled < kBufLen) ++s.filled;
}

/* Iterator: index 0 = oldest, s.filled-1 = newest. */
const FrameRecord &at(int idx)
{
    int i = (s.head - s.filled + idx + kBufLen * 2) % kBufLen;
    return s.buf[i];
}

/* Thin bridge for ImGui PlotLines getter callback. */
static float plot_frame_cb(void *, int idx)  { return at(idx).frame_ms; }
static float plot_cpu_cb  (void *, int idx)  { return at(idx).cpu_ms;   }
static float plot_gpu_cb  (void *, int idx)  { return at(idx).gpu_ms;   }
static float plot_wait_cb (void *, int idx)  { return at(idx).wait_ms;  }

/* -------------------------------------------------------------------- */
/* Per-frame sample (call every frame from draw)                        */
/* -------------------------------------------------------------------- */

void sample_frame(void)
{
    if (s.paused) return;

    FrameRecord r;
    r.frame_ms = ImGui::GetIO().DeltaTime * 1000.0f;

    const JceFrameStats *st = jce_gfx_stats_capture();
    JceGpuTimingObservation gpu{};
    const bool observed=jce_renderer_get_gpu_timing_observation(&gpu,sizeof(gpu));
    const auto sample=jce_profiler_observe(st,observed ? &gpu : nullptr);
    /* Actual backend CPU interval and separate API-thread wait. Neither is
     * GPU time, and subtracting waits from other threads invents CPU work. */
    r.cpu_available=sample.cpu_available; r.gpu_available=sample.gpu_available;
    r.wait_available=sample.api_wait_available;
    r.cpu_ms=static_cast<float>(sample.cpu_interval_ms);
    r.gpu_ms=static_cast<float>(sample.gpu_ms);
    r.wait_ms=static_cast<float>(sample.api_wait_ms);
    r.frame_no = s.next_frame_no++;
    push_record(r);
}

/* -------------------------------------------------------------------- */
/* Export to CSV                                                         */
/* -------------------------------------------------------------------- */

void export_csv(void)
{
    s.export_attempted = true;
    s.export_result = jce_editor_profile_analyzer_publish(
        s.buf, s.head, s.filled, s.export_path, sizeof s.export_path);
    jce_log_write(s.export_result == JceEditorProfileAnalyzerExportResult::Written
                      ? JCE_LOG_LEVEL_INFO : JCE_LOG_LEVEL_WARN,
                  "profile_analyzer", __FILE__, __LINE__,
                  "CSV export: result=%d rows=%d path=%s",
                  static_cast<int>(s.export_result), s.filled, s.export_path);
}

const char *export_result_key(JceEditorProfileAnalyzerExportResult result)
{
    using Result = JceEditorProfileAnalyzerExportResult;
    switch (result) {
    case Result::Written: return "profileAnalyzer.export.saved";
    case Result::Empty: return "profileAnalyzer.export.empty";
    case Result::Incomplete: return "profileAnalyzer.export.incomplete";
    case Result::ProfileUnavailable: return "profileAnalyzer.export.pathUnavailable";
    case Result::AllocationFailed: return "profileAnalyzer.export.allocationFailed";
    case Result::WriteFailed: return "profileAnalyzer.export.writeFailed";
    }
    return "profileAnalyzer.export.incomplete";
}

/* -------------------------------------------------------------------- */
/* Panel draw                                                            */
/* -------------------------------------------------------------------- */

void draw_stats_summary(void)
{
    if (s.filled == 0) return;

    float sum_f = 0, sum_c = 0, sum_g = 0, sum_w = 0;
    float mx_f  = 0, mx_c  = 0, mx_g  = 0, mx_w=0;
    int cpu_count=0,gpu_count=0,wait_count=0;
    int   spikes = 0;
    for (int i = 0; i < s.filled; ++i) {
        const FrameRecord &r = at(i);
        sum_f += r.frame_ms; mx_f = std::fmax(mx_f, r.frame_ms);
        if(r.cpu_available) {sum_c+=r.cpu_ms;mx_c=std::fmax(mx_c,r.cpu_ms);++cpu_count;}
        if(r.gpu_available) {sum_g+=r.gpu_ms;mx_g=std::fmax(mx_g,r.gpu_ms);++gpu_count;}
        if(r.wait_available) {sum_w+=r.wait_ms;mx_w=std::fmax(mx_w,r.wait_ms);++wait_count;}
        if (r.frame_ms > s.spike_thresh) ++spikes;
    }
    float n = (float)s.filled;

    if (ImGui::BeginTable("##pa_stats", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.metric",  "Metric"),  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.avg",     "Avg ms"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.max",     "Max ms"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        auto row = [&](const char *label, float avg, float peak, bool available=true) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            if(available) ImGui::Text("%.2f",avg); else ImGui::TextDisabled("%s",jce_editor_i18n("profiler.value.unavailable"));
            ImGui::TableSetColumnIndex(2);
            if(available) ImGui::Text("%.2f",peak); else ImGui::TextDisabled("%s",jce_editor_i18n("profiler.value.unavailable"));
        };

        row(jce_editor_i18n_or("profileAnalyzer.row.frame",  "Frame"),       sum_f / n, mx_f);
        row(jce_editor_i18n("profiler.bar.cpu"),cpu_count ? sum_c/cpu_count : 0,mx_c,cpu_count>0);
        row(jce_editor_i18n("profiler.bar.gpu"),gpu_count ? sum_g/gpu_count : 0,mx_g,gpu_count>0);
        row(jce_editor_i18n("profiler.bar.wait"),wait_count ? sum_w/wait_count : 0,mx_w,wait_count>0);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("%s: %d / %d",
                jce_editor_i18n_or("profileAnalyzer.spikes", "Spikes"),
                spikes, s.filled);
}

void draw_plots(void)
{
    if (s.filled == 0) return;
    ImVec2 plot_sz(-FLT_MIN, 60.0f);
    ImGui::PlotLines(jce_editor_i18n_id("profileAnalyzer.plot.frame", "pa_frame"),
                     plot_frame_cb, nullptr, s.filled, 0,
                     nullptr, 0.0f, 50.0f, plot_sz);
    bool cpu_all=true,gpu_all=true,wait_all=true;
    for(int i=0;i<s.filled;++i) {
        cpu_all=cpu_all && at(i).cpu_available;
        gpu_all=gpu_all && at(i).gpu_available;
        wait_all=wait_all && at(i).wait_available;
    }
    if(cpu_all) ImGui::PlotLines(jce_editor_i18n("profiler.bar.cpu"),plot_cpu_cb,nullptr,s.filled,0,nullptr,0.0f,33.3f,plot_sz);
    else ImGui::TextDisabled("%s: %s",jce_editor_i18n("profiler.bar.cpu"),jce_editor_i18n("profiler.value.unavailable"));
    if(gpu_all) ImGui::PlotLines(jce_editor_i18n("profiler.bar.gpu"),plot_gpu_cb,nullptr,s.filled,0,nullptr,0.0f,33.3f,plot_sz);
    else ImGui::TextDisabled("%s: %s",jce_editor_i18n("profiler.bar.gpu"),jce_editor_i18n("profiler.value.unavailable"));
    if(wait_all) ImGui::PlotLines(jce_editor_i18n("profiler.bar.wait"),plot_wait_cb,nullptr,s.filled,0,nullptr,0.0f,16.7f,plot_sz);
    else ImGui::TextDisabled("%s: %s",jce_editor_i18n("profiler.bar.wait"),jce_editor_i18n("profiler.value.unavailable"));
    if(ImGui::IsItemHovered()) ImGui::SetTooltip("%s",jce_editor_i18n("profiler.verdict.tip"));
}

void draw_spike_table(void)
{
    if (!s.show_spikes || s.filled == 0) return;

    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n_or("profileAnalyzer.section.spikes",
                           "Spike Frames"));
    ImGui::Separator();

    if (ImGui::BeginTable("##pa_spikes", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit,
                          ImVec2(0, 120.0f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("profileAnalyzer.col.frameMs"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("profileAnalyzer.col.cpuMs"),   ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("profileAnalyzer.col.gpuMs"),   ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("profileAnalyzer.col.waitMs"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < s.filled; ++i) {
            const FrameRecord &r = at(i);
            if (r.frame_ms <= s.spike_thresh) continue;
            ImGui::TableNextRow();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", (unsigned long long)r.frame_no);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", r.frame_ms);
            ImGui::TableSetColumnIndex(2); if(r.cpu_available) ImGui::Text("%.2f",r.cpu_ms); else ImGui::TextDisabled("%s",jce_editor_i18n("profiler.value.unavailable"));
            ImGui::TableSetColumnIndex(3); if(r.gpu_available) ImGui::Text("%.2f",r.gpu_ms); else ImGui::TextDisabled("%s",jce_editor_i18n("profiler.value.unavailable"));
            ImGui::TableSetColumnIndex(4); if(r.wait_available) ImGui::Text("%.2f",r.wait_ms); else ImGui::TextDisabled("%s",jce_editor_i18n("profiler.value.unavailable"));
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

void draw_toolbar(void)
{
    /* One-time restore of the persisted knobs (user-global, so the analyzer
     * setup carries across projects and restarts). */
    static bool s_knobs_loaded = false;
    if (!s_knobs_loaded) {
        s_knobs_loaded = true;
        s.spike_thresh = jce_editor_ui_state_load_float(
                             "profana.spike_thresh", s.spike_thresh,
                             1.0f, 200.0f);
        s.paused       = jce_editor_ui_state_load_int(
                             "profana.paused", s.paused ? 1 : 0, 0, 1) != 0;
    }

    if (ImGui::Checkbox(jce_editor_i18n_id("profileAnalyzer.pause", "pa_pause"),  &s.paused))
        jce_editor_ui_state_save_int("profana.paused", s.paused ? 1 : 0);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("profileAnalyzer.showSpikes", "pa_sp"), &s.show_spikes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::DragFloat(jce_editor_i18n_id("profileAnalyzer.spikeThresh", "pa_thresh"),
                         &s.spike_thresh, 0.5f, 1.0f, 200.0f, "%.1f ms"))
        jce_editor_ui_state_save_float("profana.spike_thresh", s.spike_thresh);
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("profileAnalyzer.clear", "pa_clear")))
        s.filled = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("profileAnalyzer.exportCsv", "pa_csv")))
        export_csv();
    if (s.export_attempted) {
        ImGui::TextWrapped("%s", jce_editor_i18n(export_result_key(s.export_result)));
        if (s.export_path[0]) ImGui::TextWrapped("%s", s.export_path);
    }
}

} /* namespace */

extern "C" void jce_editor_panel_profile_analyzer_content(void)
{
    sample_frame();
    draw_toolbar();
    ImGui::Separator();
    draw_stats_summary();
    ImGui::Spacing();
    draw_plots();
    draw_spike_table();
}

/* Shim: Profile Analyzer has been merged into the Profiler
 * "Profiling" workbench as a tab.  Activating this panel now redirects
 * to that workbench and requests the Analyzer tab.  Symbol kept so
 * menu/hotkey entries registered against JCE_PANEL_PROFILE_ANALYZER
 * keep working. */
extern "C" void jce_editor_panel_profile_analyzer(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_PROFILE_ANALYZER,
                                        JCE_PANEL_PROFILER,
                                        "panel.profiler", "profiler"))
        jce_panel_profiler_request_tab(2);
}
