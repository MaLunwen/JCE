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
#include "ui/jce_editor_ui_state.h"

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
    float    frame_ms  = 0.0f;
    float    cpu_ms    = 0.0f;
    float    gpu_ms    = 0.0f;
    float    wait_ms   = 0.0f;
    uint64_t frame_no  = 0;     /* stable id (ring index shifts every frame) */
};

struct State {
    FrameRecord buf[kBufLen]  = {};
    int         head          = 0;
    int         filled        = 0;
    float       spike_thresh  = kDefaultMs;
    bool        paused        = false;
    bool        show_spikes   = true;
    uint64_t    next_frame_no = 1;
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
        /* CPU work = wall frame time minus GPU waits (else CPU==Frame and
         * the panel cannot tell CPU-bound from GPU-bound). */
        int64_t cpu_work = st->cpu_time_frame - st->wait_render - st->wait_submit;
        if (cpu_work < 0) cpu_work = 0;
        r.cpu_ms  = (float)((double)cpu_work * 1000.0 / (double)cpu_freq);
        r.gpu_ms  = (float)((double)(st->gpu_time_end - st->gpu_time_begin)
                            * 1000.0 / (double)gpu_freq);
        r.wait_ms = (float)((double)(st->wait_render + st->wait_submit)
                            * 1000.0 / (double)cpu_freq);
    }
    r.frame_no = s.next_frame_no++;
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
    {
        int rem = cap - pos;
        int n = snprintf(out + pos, (size_t)rem,
                         "frame,frame_ms,cpu_ms,gpu_ms,wait_ms\n");
        if (n > 0 && n < rem) pos += n;
    }
    for (int i = 0; i < s.filled; ++i) {
        const FrameRecord &r = at(i);
        int rem = cap - pos;
        if (rem <= 1) break;
        /* Clamp: snprintf returns the WOULD-HAVE length on truncation —
         * adding it blindly walked pos past the buffer (heap OOB on write). */
        int n = snprintf(out + pos, (size_t)rem,
                         "%llu,%.3f,%.3f,%.3f,%.3f\n",
                         (unsigned long long)r.frame_no,
                         r.frame_ms, r.cpu_ms, r.gpu_ms, r.wait_ms);
        if (n <= 0 || n >= rem) break;
        pos += n;
    }

    bool ok = jce_fs_host_write_all("profile_export.csv", out, (uint64_t)pos);
    (void)ok;
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
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%llu", (unsigned long long)r.frame_no);
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
