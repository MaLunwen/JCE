/*
 * jce_panel_profile_analyzer.cpp  Profile Analyzer editor panel (P4-D.4).
 *
 * Provides post-hoc analysis on top of the lightweight profiler data:
 *   - Rolling 256-frame ring buffer of frame-time, CPU, GPU, wait.
 *   - Category breakdown bar (CPU frame / render submit / GPU / wait).
 *   - Spike detector: frames exceeding a configurable threshold are
 *     highlighted in red and listed in a table.
 *   - Export-to-CSV button (writes to host FS via jce_fs_host_write_all).
 *
 * Data is sampled from ImGui DeltaTime + jce_gfx_stats_capture() so the
 * panel works with or without Tracy.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/renderer/jce_lowlevel.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

/* -------------------------------------------------------------------- */
/* Constants and ring buffer                                            */
/* -------------------------------------------------------------------- */

constexpr int   kBufLen     = 256;
constexpr float kDefaultMs  = 33.3f;   /* spike threshold default (~30 fps) */

struct FrameRecord {
    float frame_ms  = 0.0f;
    float cpu_ms    = 0.0f;
    float gpu_ms    = 0.0f;
    float wait_ms   = 0.0f;
};

struct State {
    FrameRecord buf[kBufLen]  = {};
    int         head          = 0;
    int         filled        = 0;
    float       spike_thresh  = kDefaultMs;
    bool        paused        = false;
    bool        show_spikes   = true;
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
    if (st) {
        int64_t cpu_freq = st->cpu_timer_freq > 0 ? st->cpu_timer_freq : 1;
        int64_t gpu_freq = st->gpu_timer_freq > 0 ? st->gpu_timer_freq : 1;
        r.cpu_ms  = (float)((double)st->cpu_time_frame * 1000.0 / (double)cpu_freq);
        r.gpu_ms  = (float)((double)(st->gpu_time_end - st->gpu_time_begin)
                            * 1000.0 / (double)gpu_freq);
        r.wait_ms = (float)((double)(st->wait_render + st->wait_submit)
                            * 1000.0 / (double)cpu_freq);
    }
    push_record(r);
}

/* -------------------------------------------------------------------- */
/* Export to CSV                                                         */
/* -------------------------------------------------------------------- */

void export_csv(void)
{
    if (s.filled == 0) return;
    /* Build CSV in a heap-allocated buffer (avoid large stack). */
    const int line_est = 64;
    int cap = s.filled * line_est + 256;
    char *out = (char *)jce_malloc((size_t)cap);
    if (!out) return;

    int pos = 0;
    pos += snprintf(out + pos, (size_t)(cap - pos),
                    "frame,frame_ms,cpu_ms,gpu_ms,wait_ms\n");
    for (int i = 0; i < s.filled && pos < cap - 80; ++i) {
        const FrameRecord &r = at(i);
        pos += snprintf(out + pos, (size_t)(cap - pos),
                        "%d,%.3f,%.3f,%.3f,%.3f\n",
                        i, r.frame_ms, r.cpu_ms, r.gpu_ms, r.wait_ms);
    }

    jce_fs_host_write_all("profile_export.csv", out, (uint64_t)pos);
    jce_free(out);
}

/* -------------------------------------------------------------------- */
/* Panel draw                                                            */
/* -------------------------------------------------------------------- */

void draw_stats_summary(void)
{
    if (s.filled == 0) return;

    float sum_f = 0, sum_c = 0, sum_g = 0, sum_w = 0;
    float mx_f  = 0, mx_c  = 0, mx_g  = 0;
    int   spikes = 0;
    for (int i = 0; i < s.filled; ++i) {
        const FrameRecord &r = at(i);
        sum_f += r.frame_ms; mx_f = std::fmax(mx_f, r.frame_ms);
        sum_c += r.cpu_ms;   mx_c = std::fmax(mx_c, r.cpu_ms);
        sum_g += r.gpu_ms;   mx_g = std::fmax(mx_g, r.gpu_ms);
        sum_w += r.wait_ms;
        if (r.frame_ms > s.spike_thresh) ++spikes;
    }
    float n = (float)s.filled;

    if (ImGui::BeginTable("##pa_stats", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.metric",  "Metric"),  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.avg",     "Avg ms"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(jce_editor_i18n_or("profileAnalyzer.col.max",     "Max ms"),  ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        auto row = [&](const char *label, float avg, float peak) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", avg);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", peak);
        };

        row(jce_editor_i18n_or("profileAnalyzer.row.frame",  "Frame"),       sum_f / n, mx_f);
        row(jce_editor_i18n_or("profileAnalyzer.row.cpu",    "CPU"),          sum_c / n, mx_c);
        row(jce_editor_i18n_or("profileAnalyzer.row.gpu",    "GPU"),          sum_g / n, mx_g);
        row(jce_editor_i18n_or("profileAnalyzer.row.wait",   "Wait"),         sum_w / n, -1.0f);

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
    ImGui::PlotLines(jce_editor_i18n_id("profileAnalyzer.plot.cpu", "pa_cpu"),
                     plot_cpu_cb,   nullptr, s.filled, 0,
                     nullptr, 0.0f, 33.3f, plot_sz);
    ImGui::PlotLines(jce_editor_i18n_id("profileAnalyzer.plot.gpu", "pa_gpu"),
                     plot_gpu_cb,   nullptr, s.filled, 0,
                     nullptr, 0.0f, 33.3f, plot_sz);
    ImGui::PlotLines(jce_editor_i18n_id("profileAnalyzer.plot.wait", "pa_wait"),
                     plot_wait_cb,  nullptr, s.filled, 0,
                     nullptr, 0.0f, 16.7f, plot_sz);
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
            ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", r.frame_ms);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", r.cpu_ms);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f", r.gpu_ms);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f", r.wait_ms);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
}

void draw_toolbar(void)
{
    ImGui::Checkbox(jce_editor_i18n_id("profileAnalyzer.pause", "pa_pause"),  &s.paused);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("profileAnalyzer.showSpikes", "pa_sp"), &s.show_spikes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat(jce_editor_i18n_id("profileAnalyzer.spikeThresh", "pa_thresh"),
                     &s.spike_thresh, 0.5f, 1.0f, 200.0f, "%.1f ms");
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("profileAnalyzer.clear", "pa_clear")))
        s.filled = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton(jce_editor_i18n_id("profileAnalyzer.exportCsv", "pa_csv")))
        export_csv();
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
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROFILE_ANALYZER);
    if (!vis || !*vis) return;
    *vis = false;

    bool *pf_vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER);
    if (pf_vis) *pf_vis = true;

    char title[96];
    snprintf(title, sizeof(title), "%s###profiler",
             jce_editor_i18n_or("panel.profiler", "Profiler"));
    ImGui::SetWindowFocus(title);
    jce_panel_profiler_request_tab(2);
}
