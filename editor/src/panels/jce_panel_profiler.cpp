/*
 * jce_panel_profiler.cpp  Lightweight in-editor performance panel.
 *
 * Visualises:
 *   - rolling frame-time / FPS history (PlotLines)
 *   - min / max / average frame-time over the window
 *   - ImGui IO counters (vertices / indices / draw lists)
 *   - editor uptime, current display & viewport size
 *   - GPU memory used (when bgfx exposes it via the engine renderer
 *     stats hook — currently a placeholder until that API lands)
 *
 * Deliberately lightweight: samples DeltaTime each frame instead of
 * hooking the Tracy/profile API, so the panel works in shipping
 * builds where Tracy is disabled.  Once the engine exposes a stable
 * stats query (`jce_renderer_get_stats(...)` etc.), the empty
 * "Renderer" section below is the place to wire it in.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"

#include <jce/renderer/jce_lowlevel.h>
#include <jce/tools/jce_imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int   kHistoryLen = 240;        /* ~4 s at 60 fps */

struct ProfilerState {
    float   frame_ms[kHistoryLen] = {};
    float   cpu_ms  [kHistoryLen] = {};
    float   gpu_ms  [kHistoryLen] = {};
    float   wait_ms [kHistoryLen] = {};
    float   vram_mb [kHistoryLen] = {};
    int     head      = 0;
    int     filled    = 0;
    double  uptime_s  = 0.0;
    /* Stats snapshot (captured each frame for tables/hotlist) */
    int64_t  cpu_freq         = 1;
    int64_t  gpu_freq         = 1;
    int64_t  cpu_time_frame   = 0;
    int64_t  cpu_submit       = 0;
    int64_t  gpu_time_total   = 0;
    int64_t  wait_render      = 0;
    int64_t  wait_submit      = 0;
    int64_t  vram_used        = 0;
    int64_t  vram_max         = 0;
    int64_t  tex_mem          = 0;
    int64_t  rt_mem           = 0;
    uint32_t num_draw         = 0;
    uint32_t num_compute      = 0;
    uint32_t num_blit         = 0;
    uint16_t num_textures     = 0;
    uint16_t num_programs     = 0;
    uint16_t num_shaders      = 0;
    uint16_t num_uniforms     = 0;
    uint16_t num_fbs          = 0;
    uint16_t num_vbs          = 0;
    uint16_t num_ibs          = 0;
    uint16_t bb_w             = 0;
    uint16_t bb_h             = 0;
    /* View stats */
    struct ViewRow {
        int     id;
        char    name[64];
        float   cpu_ms;
        float   gpu_ms;
        uint32_t draws;
    };
    std::vector<ViewRow> view_rows;
    int      sort_col   = 2;   /* 0=id 1=name 2=gpu 3=cpu 4=draws */
    bool     sort_desc  = true;
};

ProfilerState s_prof;

template <typename T>
void push_ring(T *ring, T v)
{
    ring[s_prof.head] = v;
}

void push_sample(float dt_ms)
{
    /* Capture stats first (in head slot), then advance head. */
    const JceFrameStats *st = jce_gfx_stats_capture();
    float cpu_ms = 0.0f, gpu_ms = 0.0f, wait_ms = 0.0f, vram_mb = 0.0f;
    if (st) {
        s_prof.cpu_freq = st->cpu_timer_freq > 0 ? st->cpu_timer_freq : 1;
        s_prof.gpu_freq = st->gpu_timer_freq > 0 ? st->gpu_timer_freq : 1;
        s_prof.cpu_time_frame = st->cpu_time_frame;
        s_prof.cpu_submit     = st->cpu_time_frame; /* aggregate; submit-only no longer surfaced */
        s_prof.gpu_time_total = st->gpu_time_end - st->gpu_time_begin;
        s_prof.wait_render    = st->wait_render;
        s_prof.wait_submit    = st->wait_submit;
        s_prof.vram_used      = st->gpu_memory_used;
        s_prof.vram_max       = st->gpu_memory_max;
        s_prof.tex_mem        = st->texture_memory_used;
        s_prof.rt_mem         = st->rt_memory_used;
        s_prof.num_draw       = st->num_draw;
        s_prof.num_compute    = st->num_compute;
        s_prof.num_blit       = st->num_blit;
        s_prof.num_textures   = st->num_textures;
        s_prof.num_programs   = st->num_programs;
        s_prof.num_shaders    = st->num_shaders;
        s_prof.num_uniforms   = st->num_uniforms;
        s_prof.num_fbs        = st->num_frame_buffers;
        s_prof.num_vbs        = st->num_vertex_buffers;
        s_prof.num_ibs        = st->num_index_buffers;
        s_prof.bb_w           = st->backbuffer_width;
        s_prof.bb_h           = st->backbuffer_height;

        cpu_ms = (float)((double)s_prof.cpu_submit * 1000.0 / (double)s_prof.cpu_freq);
        gpu_ms = (float)((double)s_prof.gpu_time_total * 1000.0 / (double)s_prof.gpu_freq);
        wait_ms = (float)((double)(s_prof.wait_render + s_prof.wait_submit) * 1000.0
                          / (double)s_prof.cpu_freq);
        int64_t vram = s_prof.vram_used > 0
                          ? s_prof.vram_used
                          : (s_prof.tex_mem + s_prof.rt_mem);
        vram_mb = (float)((double)vram / (1024.0 * 1024.0));

        /* View rows snapshot (rebuilt each frame, then sorted on demand) */
        s_prof.view_rows.clear();
        s_prof.view_rows.reserve(st->view_stats_count);
        for (int i = 0; i < st->view_stats_count; ++i) {
            const JceViewStats *v = &st->view_stats[i];
            ProfilerState::ViewRow r;
            r.id = v->view_id;
            std::snprintf(r.name, sizeof(r.name), "%s", v->name);
            r.cpu_ms = (float)((double)(v->cpu_time_end - v->cpu_time_begin) * 1000.0 / (double)s_prof.cpu_freq);
            r.gpu_ms = (float)((double)(v->gpu_time_end - v->gpu_time_begin) * 1000.0 / (double)s_prof.gpu_freq);
            r.draws  = 0; /* per-view draw count not in this stats struct */
            s_prof.view_rows.push_back(r);
        }
    }

    s_prof.frame_ms[s_prof.head] = dt_ms;
    s_prof.cpu_ms  [s_prof.head] = cpu_ms;
    s_prof.gpu_ms  [s_prof.head] = gpu_ms;
    s_prof.wait_ms [s_prof.head] = wait_ms;
    s_prof.vram_mb [s_prof.head] = vram_mb;
    s_prof.head = (s_prof.head + 1) % kHistoryLen;
    if (s_prof.filled < kHistoryLen) ++s_prof.filled;
}

void compute_stats(const float *ring, float &out_min, float &out_max, float &out_avg)
{
    if (s_prof.filled == 0) {
        out_min = out_max = out_avg = 0.0f;
        return;
    }
    float mn = ring[0];
    float mx = ring[0];
    double sum = 0.0;
    for (int i = 0; i < s_prof.filled; ++i) {
        float v = ring[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum += v;
    }
    out_min = mn;
    out_max = mx;
    out_avg = static_cast<float>(sum / s_prof.filled);
}

/* PlotLines callbacks per series. Walk the ring chronologically. */
float ring_idx(int idx)
{
    int start = (s_prof.head - s_prof.filled + kHistoryLen) % kHistoryLen;
    return (float)((start + idx) % kHistoryLen);
}
float ring_frame (void *, int idx) { return s_prof.frame_ms[(int)ring_idx(idx)]; }
float ring_cpu   (void *, int idx) { return s_prof.cpu_ms  [(int)ring_idx(idx)]; }
float ring_gpu   (void *, int idx) { return s_prof.gpu_ms  [(int)ring_idx(idx)]; }
float ring_wait  (void *, int idx) { return s_prof.wait_ms [(int)ring_idx(idx)]; }
float ring_vram  (void *, int idx) { return s_prof.vram_mb [(int)ring_idx(idx)]; }

void format_bytes(char *buf, size_t n, int64_t b)
{
    if (b <= 0) { std::snprintf(buf, n, "—"); return; }
    double mb = (double)b / (1024.0 * 1024.0);
    if (mb >= 1024.0) std::snprintf(buf, n, "%.2f GB", mb / 1024.0);
    else              std::snprintf(buf, n, "%.1f MB", mb);
}

void draw_bars_block(const char *id, float cpu, float gpu, float wait, float frame)
{
    /* Stacked bars: cpu / gpu / wait / frame_total. */
    float maxv = std::max({cpu, gpu, wait, frame, 16.7f}) * 1.2f;
    if (maxv < 1.0f) maxv = 1.0f;

    auto bar = [&](const char *label, float ms, ImU32 col) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float full_w = ImGui::GetContentRegionAvail().x - 120.0f;
        if (full_w < 40.0f) full_w = 40.0f;
        float w = full_w * (ms / maxv);
        if (w < 1.0f && ms > 0.0f) w = 1.0f;
        float h = ImGui::GetTextLineHeight() + 4.0f;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + full_w, p0.y + h),
                          jce_theme::track_even());
        dl->AddRectFilled(p0, ImVec2(p0.x + w,      p0.y + h), col);
        ImGui::Dummy(ImVec2(full_w, h));
        ImGui::SameLine();
        ImGui::Text("%-6s %6.2f ms", label, ms);
    };

    ImGui::PushID(id);
    bar(jce_editor_i18n("profiler.bar.frame"), frame, IM_COL32(220, 220, 220, 255));
    bar(jce_editor_i18n("profiler.bar.cpu"),   cpu,   IM_COL32( 80, 200, 120, 255));
    bar(jce_editor_i18n("profiler.bar.gpu"),   gpu,   IM_COL32( 80, 160, 230, 255));
    bar(jce_editor_i18n("profiler.bar.wait"),  wait,  IM_COL32(220, 160,  60, 255));
    ImGui::PopID();
}

void draw_view_table()
{
    if (s_prof.view_rows.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("profiler.empty.bgfxStats"));
        return;
    }
    /* Sort rows by current sort column. */
    auto cmp = [](const ProfilerState::ViewRow &a,
                  const ProfilerState::ViewRow &b) {
        switch (s_prof.sort_col) {
            case 0: return a.id < b.id;
            case 1: return std::strcmp(a.name, b.name) < 0;
            case 2: return a.gpu_ms < b.gpu_ms;
            case 3: return a.cpu_ms < b.cpu_ms;
            default: return a.draws < b.draws;
        }
    };
    std::sort(s_prof.view_rows.begin(), s_prof.view_rows.end(),
              [&](const auto &a, const auto &b) {
                  return s_prof.sort_desc ? cmp(b, a) : cmp(a, b);
              });

    if (ImGui::BeginTable("prof_views", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInner |
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 180)))
    {
        const char *cols[] = {
            jce_editor_i18n("profiler.col.view"),
            jce_editor_i18n("profiler.col.name"),
            jce_editor_i18n("profiler.col.gpuMs"),
            jce_editor_i18n("profiler.col.cpuMs"),
        };
        ImGui::TableSetupScrollFreeze(0, 1);
        for (int c = 0; c < 4; ++c)
            ImGui::TableSetupColumn(cols[c]);
        ImGui::TableHeadersRow();

        /* Click headers → toggle sort. */
        for (int c = 0; c < 4; ++c) {
            ImGui::TableSetColumnIndex(c);
            if (ImGui::IsItemClicked()) {
                if (s_prof.sort_col == c) s_prof.sort_desc = !s_prof.sort_desc;
                else { s_prof.sort_col = c; s_prof.sort_desc = (c >= 2); }
            }
        }

        for (const auto &r : s_prof.view_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", r.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.name[0] ? r.name : "(unnamed)");
            ImGui::TableNextColumn();
            ImU32 c_gpu = (r.gpu_ms > 5.0f) ? IM_COL32(255, 120, 80, 255)
                                             : IM_COL32(160, 200, 240, 255);
            ImGui::PushStyleColor(ImGuiCol_Text, c_gpu);
            ImGui::Text("%.3f", r.gpu_ms);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn(); ImGui::Text("%.3f", r.cpu_ms);
        }
        ImGui::EndTable();
    }
}

void draw_content(void)
{
    ImGuiIO &io = ImGui::GetIO();

    /* Sample current frame. */
    float dt_ms = io.DeltaTime * 1000.0f;
    push_sample(dt_ms);
    s_prof.uptime_s += io.DeltaTime;

    float mn, mx, avg;
    compute_stats(s_prof.frame_ms, mn, mx, avg);
    float fps_avg = (avg > 0.0f) ? (1000.0f / avg) : 0.0f;

    float cpu_mn, cpu_mx, cpu_avg;
    float gpu_mn, gpu_mx, gpu_avg;
    float w_mn,   w_mx,   w_avg;
    float v_mn,   v_mx,   v_avg;
    compute_stats(s_prof.cpu_ms,  cpu_mn, cpu_mx, cpu_avg);
    compute_stats(s_prof.gpu_ms,  gpu_mn, gpu_mx, gpu_avg);
    compute_stats(s_prof.wait_ms, w_mn,   w_mx,   w_avg);
    compute_stats(s_prof.vram_mb, v_mn,   v_mx,   v_avg);

    float plot_max = std::max(33.4f, mx * 1.2f);

    /* ── Frame-time graph ──────────────────────────────────────── */
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%.2f ms (%.1f FPS)", avg, fps_avg);
    ImGui::PlotLines("##frame_ms", &ring_frame, nullptr,
                     s_prof.filled > 0 ? s_prof.filled : 1,
                     0, overlay, 0.0f, plot_max,
                     ImVec2(-1.0f, 80.0f));

    /* ── Stacked bars ──────────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.frameBudget"));
    draw_bars_block("avg_bars", cpu_avg, gpu_avg, w_avg, avg);

    /* ── Per-series mini-plots ─────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.series"));
    if (ImGui::BeginTable("prof_series", 2,
                          ImGuiTableFlags_SizingStretchSame |
                          ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        std::snprintf(overlay, sizeof(overlay), "%s %.2f ms",
                      jce_editor_i18n("profiler.bar.cpu"), cpu_avg);
        ImGui::PlotLines("##cpu", &ring_cpu, nullptr, s_prof.filled,
                         0, overlay, 0.0f, std::max(8.0f, cpu_mx * 1.2f),
                         ImVec2(-1, 60));
        ImGui::TableNextColumn();
        std::snprintf(overlay, sizeof(overlay), "%s %.2f ms",
                      jce_editor_i18n("profiler.bar.gpu"), gpu_avg);
        ImGui::PlotLines("##gpu", &ring_gpu, nullptr, s_prof.filled,
                         0, overlay, 0.0f, std::max(8.0f, gpu_mx * 1.2f),
                         ImVec2(-1, 60));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        std::snprintf(overlay, sizeof(overlay), "%s %.2f ms",
                      jce_editor_i18n("profiler.bar.wait"), w_avg);
        ImGui::PlotLines("##wait", &ring_wait, nullptr, s_prof.filled,
                         0, overlay, 0.0f, std::max(4.0f, w_mx * 1.2f),
                         ImVec2(-1, 60));
        ImGui::TableNextColumn();
        std::snprintf(overlay, sizeof(overlay), "%s %.1f MB",
                      jce_editor_i18n("profiler.label.vram"), v_avg);
        ImGui::PlotLines("##vram", &ring_vram, nullptr, s_prof.filled,
                         0, overlay, 0.0f, std::max(64.0f, v_mx * 1.1f),
                         ImVec2(-1, 60));
        ImGui::EndTable();
    }

    /* ── Numeric stats ─────────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.frameStats"));
    if (ImGui::BeginTable("prof_stats", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg))
    {
        auto row = [](const char *k, const char *v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
        };

        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.2f ms", dt_ms);
        row(jce_editor_i18n("profiler.row.current"),   buf);
        std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f ms", mn, avg, mx);
        row(jce_editor_i18n("profiler.row.frameMinAvgMax"), buf);
        std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f ms", cpu_mn, cpu_avg, cpu_mx);
        row(jce_editor_i18n("profiler.row.cpuMinAvgMax"), buf);
        std::snprintf(buf, sizeof(buf), "%.2f / %.2f / %.2f ms", gpu_mn, gpu_avg, gpu_mx);
        row(jce_editor_i18n("profiler.row.gpuMinAvgMax"), buf);
        std::snprintf(buf, sizeof(buf), "%.1f", io.Framerate);
        row(jce_editor_i18n("profiler.row.imguiFps"), buf);
        std::snprintf(buf, sizeof(buf), "%d / %d", s_prof.filled, kHistoryLen);
        row(jce_editor_i18n("profiler.row.samples"),   buf);
        std::snprintf(buf, sizeof(buf), "%.1f s", s_prof.uptime_s);
        row(jce_editor_i18n("profiler.row.uptime"),    buf);
        ImGui::EndTable();
    }

    /* ── Renderer (bgfx live stats) ────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.renderer"));
    if (ImGui::BeginTable("prof_renderer", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg))
    {
        auto row_u = [](const char *k, uint64_t v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)v);
        };
        auto row_b = [](const char *k, int64_t bytes) {
            char b[32]; format_bytes(b, sizeof(b), bytes);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b);
        };

        row_u(jce_editor_i18n("profiler.row.drawCalls"),   s_prof.num_draw);
        row_u(jce_editor_i18n("profiler.row.compute"),     s_prof.num_compute);
        row_u(jce_editor_i18n("profiler.row.blits"),       s_prof.num_blit);
        row_u(jce_editor_i18n("profiler.row.textures"),    s_prof.num_textures);
        row_u(jce_editor_i18n("profiler.row.programs"),    s_prof.num_programs);
        row_u(jce_editor_i18n("profiler.row.shaders"),     s_prof.num_shaders);
        row_u(jce_editor_i18n("profiler.row.uniforms"),    s_prof.num_uniforms);
        row_u(jce_editor_i18n("profiler.row.frameBuffers"),s_prof.num_fbs);
        row_u(jce_editor_i18n("profiler.row.vertexBufs"),  s_prof.num_vbs);
        row_u(jce_editor_i18n("profiler.row.indexBufs"),   s_prof.num_ibs);
        row_b(jce_editor_i18n("profiler.row.textureMem"),  s_prof.tex_mem);
        row_b(jce_editor_i18n("profiler.row.rtMem"),       s_prof.rt_mem);
        row_b(jce_editor_i18n("profiler.row.vramUsed"),    s_prof.vram_used);
        row_b(jce_editor_i18n("profiler.row.vramMax"),     s_prof.vram_max);
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(jce_editor_i18n("profiler.label.backbuffer"));
        ImGui::TableNextColumn();
        ImGui::Text("%u x %u", (unsigned)s_prof.bb_w, (unsigned)s_prof.bb_h);
        ImGui::EndTable();
    }

    /* ── ImGui ──────────────────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.imgui"));
    if (ImGui::BeginTable("prof_imgui", 2,
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_RowBg))
    {
        auto row_i = [](const char *k, int v) {
            char b[32]; std::snprintf(b, sizeof(b), "%d", v);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(b);
        };
        row_i(jce_editor_i18n("profiler.row.vertices"),       io.MetricsRenderVertices);
        row_i(jce_editor_i18n("profiler.row.indices"),        io.MetricsRenderIndices);
        row_i(jce_editor_i18n("profiler.row.drawLists"),      io.MetricsRenderWindows);
        row_i(jce_editor_i18n("profiler.row.activeWindows"),  io.MetricsActiveWindows);
        ImGui::EndTable();
    }

    /* ── View hot-list ─────────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.viewHotspots"));
    draw_view_table();
}

} /* anonymous namespace */

extern "C" void jce_editor_panel_profiler_content(void)
{
    draw_content();
}

extern "C" void jce_editor_panel_profiler(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROFILER);
    if (!vis || !*vis) return;

    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_profiler", jce_editor_i18n("profiler.title"));
    if (ImGui::Begin(_wt, vis)) {
        draw_content();
    }
    ImGui::End();
}
