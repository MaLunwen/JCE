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
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_toast.h"
#include "scene/jce_editor_scene_render.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"
#include "ui/jce_theme_palette.h"

#include <jce/renderer/jce_lowlevel.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/resource/jce_world_streamer.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/tools/jce_imgui.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
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
    int view_rows_frame = -1;   /* ImGui frame the snapshot was taken on */
    int      sort_col   = 2;   /* 0=id 1=name 2=gpu 3=cpu 4=draws */
    bool     sort_desc  = true;
};

ProfilerState s_prof;

/* Rebuild the per-view row snapshot from a fresh stats capture.  Shared
 * by the CPU tab's sampler and the Frame Debugger tab (whose own copy of
 * this table was removed — single implementation, see
 * jce_panel_profiler_draw_view_table). */
void refill_view_rows(const JceFrameStats *st)
{
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
    s_prof.view_rows_frame = ImGui::GetFrameCount();
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

        /* True CPU work: bgfx cpuTimeFrame is WALL time between frames
         * (includes GPU waits) — subtracting the waits is what lets the
         * panel distinguish CPU-bound from GPU-bound. */
        {
            int64_t cpu_work = s_prof.cpu_time_frame
                             - s_prof.wait_render - s_prof.wait_submit;
            if (cpu_work < 0) cpu_work = 0;
            cpu_ms = (float)((double)cpu_work * 1000.0 / (double)s_prof.cpu_freq);
        }
        gpu_ms = (float)((double)s_prof.gpu_time_total * 1000.0 / (double)s_prof.gpu_freq);
        wait_ms = (float)((double)(s_prof.wait_render + s_prof.wait_submit) * 1000.0
                          / (double)s_prof.cpu_freq);
        int64_t vram = s_prof.vram_used > 0
                          ? s_prof.vram_used
                          : (s_prof.tex_mem + s_prof.rt_mem);
        vram_mb = (float)((double)vram / (1024.0 * 1024.0));

        /* View rows snapshot (rebuilt each frame, then sorted on demand) */
        refill_view_rows(st);
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
    bar(jce_editor_i18n("profiler.bar.cpu"),   cpu,   JCE_COL32_STATUS_OK);
    bar(jce_editor_i18n("profiler.bar.gpu"),   gpu,   IM_COL32( 80, 160, 230, 255));
    bar(jce_editor_i18n("profiler.bar.wait"),  wait,  IM_COL32(220, 160,  60, 255));
    ImGui::PopID();
}

/* Modern sparkline: a rounded panel with a gradient-filled area under the line,
 * an optional dashed budget guide, a live end-point dot, and inline
 * label / current / avg·max stats.  The clean replacement for the flat
 * ImGui::PlotLines look (no axis chrome, no boxed overlay).  `get` walks the
 * history ring chronologically (same callbacks PlotLines used). */
void draw_area_chart(const char *label, float (*get)(void *, int), int count,
                     float cur, float avg, float peak, float scale_max,
                     const char *unit, ImU32 line_col, float budget, float height)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (w < 80.0f) w = 80.0f;
    ImVec2 p1 = ImVec2(p0.x + w, p0.y + height);
    if (scale_max < 1e-3f) scale_max = 1.0f;

    const float pad  = 5.0f;
    const float lblH = ImGui::GetTextLineHeight();
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg(), 5.0f);
    dl->AddRect(p0, p1, jce_theme::grid_minor(), 5.0f);

    float gx0 = p0.x + pad, gx1 = p1.x - pad;
    float gy0 = p0.y + pad + lblH;
    float gy1 = p1.y - pad;
    float gh = (gy1 - gy0) < 6.0f ? 6.0f : (gy1 - gy0);
    float gw = (gx1 - gx0) < 6.0f ? 6.0f : (gx1 - gx0);
    (void)gy0;
    auto yof = [&](float v) {
        float t = v / scale_max; t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        return gy1 - t * gh;
    };

    /* budget guide (dashed) — e.g. the 16.6 ms / 60 FPS frame line. */
    if (budget > 0.0f && budget < scale_max) {
        float by = yof(budget);
        for (float x = gx0; x < gx1; x += 7.0f) {
            float xe = (x + 3.5f < gx1) ? x + 3.5f : gx1;
            dl->AddLine(ImVec2(x, by), ImVec2(xe, by), jce_theme::grid_minor(), 1.0f);
        }
    }

    if (count >= 2) {
        ImU32 fill = (line_col & IM_COL32(255, 255, 255, 0)) | IM_COL32(0, 0, 0, 46);
        float step = gw / (float)(count - 1);
        for (int i = 0; i < count - 1; ++i) {
            float x0 = gx0 + step * i, x1 = gx0 + step * (i + 1);
            float y0 = yof(get(nullptr, i)), y1 = yof(get(nullptr, i + 1));
            ImVec2 quad[4] = { ImVec2(x0, y0), ImVec2(x1, y1),
                               ImVec2(x1, gy1), ImVec2(x0, gy1) };
            dl->AddConvexPolyFilled(quad, 4, fill);
        }
        for (int i = 0; i < count - 1; ++i) {
            float x0 = gx0 + step * i, x1 = gx0 + step * (i + 1);
            dl->AddLine(ImVec2(x0, yof(get(nullptr, i))),
                        ImVec2(x1, yof(get(nullptr, i + 1))), line_col, 1.7f);
        }
        dl->AddCircleFilled(ImVec2(gx1, yof(get(nullptr, count - 1))), 2.6f, line_col);
    }

    /* label + current (left), avg·max (right) */
    char buf[80];
    ImVec2 lp = ImVec2(p0.x + 7.0f, p0.y + 3.0f);
    dl->AddText(lp, jce_theme::text_secondary(), label);
    float lw = ImGui::CalcTextSize(label).x;
    std::snprintf(buf, sizeof buf, "%.2f%s", cur, unit);
    dl->AddText(ImVec2(lp.x + lw + 8.0f, lp.y), line_col, buf);
    std::snprintf(buf, sizeof buf, "%s %.1f  \xC2\xB7  %s %.1f",
                  jce_editor_i18n("profiler.stat.avg"), avg,
                  jce_editor_i18n("profiler.stat.max"), peak);
    float rw = ImGui::CalcTextSize(buf).x;
    dl->AddText(ImVec2(p1.x - rw - 7.0f, lp.y), jce_theme::text_secondary(), buf);

    ImGui::Dummy(ImVec2(w, height));
}

void draw_view_table()
{
    /* One-time restore of the persisted hot-list sort (user-global —
     * consistent with the imgui.ini-persisted memory-tab table). */
    static bool s_sort_loaded = false;
    if (!s_sort_loaded) {
        s_sort_loaded = true;
        s_prof.sort_col  = jce_editor_ui_state_load_int(
                               "profiler.viewsort.col", s_prof.sort_col, 0, 4);
        s_prof.sort_desc = jce_editor_ui_state_load_int(
                               "profiler.viewsort.desc",
                               s_prof.sort_desc ? 1 : 0, 0, 1) != 0;
    }

    if (s_prof.view_rows.empty()) {
        ImGui::TextDisabled("%s", jce_editor_i18n("profiler.empty.bgfxStats"));
        ImGui::TextDisabled("%s", jce_editor_i18n_or(
            "profiler.empty.bgfxStatsHint",
            "per-view GPU timing needs the bgfx profiler: start with "
            "JCE_PERF_LOG=1 (enables BGFX_DEBUG_PROFILER)"));
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

        /* Click headers → toggle sort.  TableSetColumnIndex submits no item,
         * so IsItemClicked() tested the LAST header for every column (only
         * the last header worked, and it fired for all four).  Use the
         * column hover flag + mouse click instead. */
        for (int c = 0; c < 4; ++c) {
            if ((ImGui::TableGetColumnFlags(c) & ImGuiTableColumnFlags_IsHovered) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (s_prof.sort_col == c) s_prof.sort_desc = !s_prof.sort_desc;
                else { s_prof.sort_col = c; s_prof.sort_desc = (c >= 2); }
                jce_editor_ui_state_save_int("profiler.viewsort.col",
                                             s_prof.sort_col);
                jce_editor_ui_state_save_int("profiler.viewsort.desc",
                                             s_prof.sort_desc ? 1 : 0);
            }
        }

        for (const auto &r : s_prof.view_rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", r.id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name[0] ? r.name
                : jce_editor_i18n("profiler.unnamedView"));
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

/* ── Clipboard snapshot ─────────────────────────────────────────────────── */

void append_fmt(std::string &out, const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) out.append(buf, std::min<size_t>((size_t)n, sizeof(buf) - 1));
}

std::string build_clipboard_snapshot(float dt_ms,
                                     float mn,  float avg, float mx,
                                     float cpu_avg, float gpu_avg, float w_avg)
{
    std::string s;
    s.reserve(4096);

    ImGuiIO &io = ImGui::GetIO();
    float fps_avg = (avg > 0.0f) ? (1000.0f / avg) : 0.0f;

    append_fmt(s, "── JCE Profiler snapshot ─────────────────────\n");
    append_fmt(s, "Uptime           : %.1f s\n", s_prof.uptime_s);
    append_fmt(s, "Backbuffer       : %u x %u\n",
               (unsigned)s_prof.bb_w, (unsigned)s_prof.bb_h);
    append_fmt(s, "Samples          : %d / %d\n", s_prof.filled, kHistoryLen);
    s += "\n[Frame timing]\n";
    append_fmt(s, "  Current        : %.2f ms (%.1f FPS)\n", dt_ms,
               dt_ms > 0.0f ? 1000.0f / dt_ms : 0.0f);
    append_fmt(s, "  Frame min/avg/max : %.2f / %.2f / %.2f ms (%.1f FPS avg)\n",
               mn, avg, mx, fps_avg);
    append_fmt(s, "  CPU avg        : %.2f ms\n", cpu_avg);
    append_fmt(s, "  GPU avg        : %.2f ms\n", gpu_avg);
    append_fmt(s, "  Wait avg       : %.2f ms\n", w_avg);
    append_fmt(s, "  ImGui FPS      : %.1f\n", io.Framerate);

    /* Engine CPU-phase breakdown — the panel's headline table, so it must ride
     * along in the copied snapshot too (was missing). */
    s += "\n[CPU phases (engine)]\n";
    {
        struct PR { const char *n; double ms; } pr[64]; int prn = 0;
        int pc = jce_perf_phase_count();
        for (int i = 0; i < pc && prn < 64; ++i) {
            const char *nm = NULL; double ms = 0.0;
            if (jce_perf_phase_peek_frame(i, &nm, &ms) && nm && ms >= 0.01) {
                pr[prn].n = nm; pr[prn].ms = ms; prn++;
            }
        }
        std::sort(pr, pr + prn, [](const PR &a, const PR &b) { return a.ms > b.ms; });
        if (prn == 0) s += "  (accumulating)\n";
        for (int i = 0; i < prn; ++i)
            append_fmt(s, "  %-16s : %.2f ms\n", pr[i].n, pr[i].ms);
    }

    s += "\n[Renderer (bgfx)]\n";
    append_fmt(s, "  Draw calls     : %u\n", s_prof.num_draw);
    append_fmt(s, "  Compute        : %u\n", s_prof.num_compute);
    append_fmt(s, "  Blits          : %u\n", s_prof.num_blit);
    append_fmt(s, "  Textures       : %u\n", s_prof.num_textures);
    append_fmt(s, "  Programs       : %u\n", s_prof.num_programs);
    append_fmt(s, "  Shaders        : %u\n", s_prof.num_shaders);
    append_fmt(s, "  Uniforms       : %u\n", s_prof.num_uniforms);
    append_fmt(s, "  Frame buffers  : %u\n", s_prof.num_fbs);
    append_fmt(s, "  Vertex bufs    : %u\n", s_prof.num_vbs);
    append_fmt(s, "  Index bufs     : %u\n", s_prof.num_ibs);
    char b[32];
    format_bytes(b, sizeof(b), s_prof.tex_mem);  append_fmt(s, "  Texture mem    : %s\n", b);
    format_bytes(b, sizeof(b), s_prof.rt_mem);   append_fmt(s, "  RT mem         : %s\n", b);
    format_bytes(b, sizeof(b), s_prof.vram_used);append_fmt(s, "  VRAM used      : %s\n", b);
    format_bytes(b, sizeof(b), s_prof.vram_max); append_fmt(s, "  VRAM max       : %s\n", b);

    s += "\n[ImGui]\n";
    append_fmt(s, "  Vertices       : %d\n", io.MetricsRenderVertices);
    append_fmt(s, "  Indices        : %d\n", io.MetricsRenderIndices);
    append_fmt(s, "  Draw lists     : %d\n", io.MetricsRenderWindows);
    append_fmt(s, "  Active windows : %d\n", io.MetricsActiveWindows);

    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    if (sr) {
        JceSceneCullStats cs = {0,0,0,false};
        jce_scene_renderer_get_cull_stats(sr, &cs);
        s += "\n[Scene culling (frustum)]\n";
        append_fmt(s, "  Mode           : %s\n", cs.enabled ? "On" : "Off");
        append_fmt(s, "  Total / vis / cull : %u / %u / %u\n",
                   cs.total, cs.visible, cs.culled);
        if (cs.total > 0) {
            float p = 100.0f * (float)cs.culled / (float)cs.total;
            append_fmt(s, "  Cull ratio     : %.1f%%\n", p);
        }
        /* Persistent extent-sized broad-phase (large-world-opt P1 #4). */
        append_fmt(s, "  Grid res       : %ux%ux%u (%u cells, %u occupied)\n",
                   cs.grid_res[0], cs.grid_res[1], cs.grid_res[2],
                   cs.grid_cells, cs.grid_occupied);
        append_fmt(s, "  Grid objects   : %u\n", cs.grid_objects);
        append_fmt(s, "  Churn (ins/upd/rem): %u / %u / %u\n",
                   cs.inserted, cs.updated, cs.removed);

        JceSceneLodStats ls = {};
        jce_scene_renderer_get_lod_stats(sr, &ls);
        s += "\n[Scene LOD]\n";
        append_fmt(s, "  Mode           : %s\n", ls.enabled ? "Global LOD group" : "Disabled");
        if (ls.enabled) {
            for (int i = 0; i < ls.level_count && i < JCE_SCENE_LOD_MAX_LEVELS; i++)
                append_fmt(s, "  L%d picks       : %u\n", i, ls.picks[i]);
            append_fmt(s, "  LOD culled     : %u\n", ls.culled);
        }

        JceSceneRqStats rqs = {};
        jce_scene_renderer_get_rq_stats(sr, &rqs);
        s += "\n[Render queue]\n";
        append_fmt(s, "  Mode           : %s\n", rqs.enabled ? "On (default; JCE_USE_RQ=0 disables)" : "Off (JCE_USE_RQ=0)");
        if (rqs.enabled) {
            append_fmt(s, "  Commands in    : %u\n", rqs.commands_in);
            append_fmt(s, "  bgfx submits   : %u\n", rqs.submits_out);
            append_fmt(s, "  Instanced batch: %u\n", rqs.batches_merged);
            append_fmt(s, "  Instances total: %u\n", rqs.instances_total);
            if (rqs.commands_in > 0 && rqs.submits_out > 0) {
                float r = (float)rqs.commands_in / (float)rqs.submits_out;
                append_fmt(s, "  Merge ratio    : %.1fx\n", r);
            }
        }

        JceSceneOcclusionStats ocs = {};
        jce_scene_renderer_get_occlusion_stats(sr, &ocs);
        s += "\n[Occlusion culling]\n";
        append_fmt(s, "  Mode           : %s\n", ocs.enabled ? "On (GPU queries)" : "Off");
        if (ocs.enabled) {
            append_fmt(s, "  Tested         : %u\n", ocs.total);
            append_fmt(s, "  Visible        : %u\n", ocs.visible);
            append_fmt(s, "  Occluded       : %u\n", ocs.occluded);
            append_fmt(s, "  Warm-up        : %u\n", ocs.warm_up);
            if (ocs.total > 0) {
                float p = 100.0f * (float)ocs.occluded / (float)ocs.total;
                append_fmt(s, "  Cull rate      : %.1f%%\n", p);
            }
        }
    }

    JceWorldStreamer *ws = jce_editor_get_world_streamer();
    s += "\n[World streaming]\n";
    if (!ws) {
        s += "  Status         : Inactive\n";
    } else {
        uint32_t loaded  = jce_world_streamer_loaded_count(ws);
        uint32_t pending = jce_world_streamer_pending_count(ws);
        uint32_t total   = jce_world_streamer_chunk_count(ws);
        uint64_t mem     = jce_world_streamer_memory_used(ws);
        uint32_t ents    = jce_world_streamer_entity_count(ws);
        append_fmt(s, "  Chunks loaded  : %u / %u\n", loaded, total);
        append_fmt(s, "  Pending loads  : %u\n", pending);
        append_fmt(s, "  Memory         : %.2f MB\n",
                   (double)mem / (1024.0 * 1024.0));
        append_fmt(s, "  Entities       : %u\n", ents);
    }

    JceMemStats ms = {};
    if (jce_mem_stats(&ms)) {
        const double MB = 1024.0 * 1024.0;
        s += "\n[Process memory]\n";
        append_fmt(s, "  RSS curr/peak  : %.1f / %.1f MB\n",
                   (double)ms.current_rss / MB, (double)ms.peak_rss / MB);
        append_fmt(s, "  Commit c/p     : %.1f / %.1f MB\n",
                   (double)ms.current_commit / MB, (double)ms.peak_commit / MB);
        append_fmt(s, "  Page faults    : %zu\n", ms.page_faults);
    }

    s += "\n──────────────────────────────────────────────\n";
    return s;
}

/* Reset the rolling history buffers without losing the singleton. */
void reset_history(void)
{
    std::memset(s_prof.frame_ms, 0, sizeof(s_prof.frame_ms));
    std::memset(s_prof.cpu_ms,   0, sizeof(s_prof.cpu_ms));
    std::memset(s_prof.gpu_ms,   0, sizeof(s_prof.gpu_ms));
    std::memset(s_prof.wait_ms,  0, sizeof(s_prof.wait_ms));
    std::memset(s_prof.vram_mb,  0, sizeof(s_prof.vram_mb));
    s_prof.head   = 0;
    s_prof.filled = 0;
}

void draw_content(void)
{
    ImGuiIO &io = ImGui::GetIO();

    /* Keep engine CPU-phase capture live while the panel is open so the
     * phase table below has data (no-op cost when idle). */
    jce_perf_phase_set_enabled(1);

    /* History epoch guard: if the tab was hidden for a while, the rings
     * hold stale frames — mixing them into min/avg/max lies.  Reset. */
    {
        static double s_last_draw = -1.0;
        double t_now = ImGui::GetTime();
        if (s_last_draw >= 0.0 && (t_now - s_last_draw) > 2.0) {
            s_prof.head = 0; s_prof.filled = 0; s_prof.uptime_s = 0.0;
        }
        s_last_draw = t_now;
    }

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

    /* ── Summary header: FPS · frame time · bottleneck verdict · actions ── */
    {
        float fps_now = (dt_ms > 0.0f) ? 1000.0f / dt_ms : 0.0f;
        ImU32 fps_col;
        if (fps_now >= 55.0f)      fps_col = JCE_COL32_STATUS_OK;
        else if (fps_now >= 30.0f) fps_col = JCE_COL32_STATUS_WARN;
        else                       fps_col = IM_COL32(230, 90, 90, 255);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        float py = ImGui::GetStyle().FramePadding.y;   /* pills = frame height → align with buttons */
        auto pill = [&](const char *txt, ImU32 bg) {
            ImVec2 sz = ImGui::CalcTextSize(txt);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            const float px = 8.0f;
            ImVec2 p1 = ImVec2(p0.x + sz.x + px * 2, p0.y + sz.y + py * 2);
            dl->AddRectFilled(p0, p1, bg, (p1.y - p0.y) * 0.5f);
            dl->AddText(ImVec2(p0.x + px, p0.y + py), IM_COL32(255, 255, 255, 255), txt);
            ImGui::Dummy(ImVec2(sz.x + px * 2, sz.y + py * 2));
        };

        char b[48];
        std::snprintf(b, sizeof b, "%.0f FPS", fps_now);
        pill(b, fps_col);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        std::snprintf(b, sizeof b, "%.2f ms", dt_ms);
        ImGui::TextUnformatted(b);
        ImGui::SameLine();
        std::snprintf(b, sizeof b, "%s %.1f  \xC2\xB7  %s %.1f",
                      jce_editor_i18n("profiler.stat.avg"), avg,
                      jce_editor_i18n("profiler.stat.max"), mx);
        ImGui::TextDisabled("%s", b);

        /* Bottleneck verdict — the panel's headline answer (CPU vs GPU work,
         * waits excluded).  Amber = balanced, green = CPU-bound, blue = GPU. */
        ImGui::SameLine();
        const char *verdict; ImU32 vcol;
        if (cpu_avg + gpu_avg < 0.05f) {
            verdict = jce_editor_i18n("profiler.verdict.measuring"); vcol = jce_theme::track_even();
        } else if (cpu_avg > gpu_avg * 1.25f) {
            verdict = jce_editor_i18n("profiler.verdict.cpuBound");  vcol = IM_COL32(70, 130, 72, 255);
        } else if (gpu_avg > cpu_avg * 1.25f) {
            verdict = jce_editor_i18n("profiler.verdict.gpuBound");  vcol = IM_COL32(58, 112, 168, 255);
        } else {
            verdict = jce_editor_i18n("profiler.verdict.balanced");  vcol = IM_COL32(150, 122, 52, 255);
        }
        pill(verdict, vcol);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("profiler.verdict.tip"));

        /* right-aligned actions */
        const char *copy_lbl  = jce_editor_i18n("profiler.action.copyAll");
        const char *reset_lbl = jce_editor_i18n("profiler.action.resetHistory");
        float copy_w  = ImGui::CalcTextSize(copy_lbl).x  + ImGui::GetStyle().FramePadding.x * 2;
        float reset_w = ImGui::CalcTextSize(reset_lbl).x + ImGui::GetStyle().FramePadding.x * 2;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(std::max(0.0f, ImGui::GetContentRegionAvail().x
                                          - copy_w - reset_w - gap), 1));
        ImGui::SameLine();
        if (ImGui::Button(reset_lbl)) {
            reset_history();
            jce_toast_success("%s", jce_editor_i18n("profiler.toast.historyReset"));
        }
        jce_editor::help_tip(jce_editor_i18n("profiler.tooltip.resetHistory"));
        ImGui::SameLine();
        if (ImGui::Button(copy_lbl)) {
            std::string snap = build_clipboard_snapshot(
                dt_ms, mn, avg, mx, cpu_avg, gpu_avg, w_avg);
            ImGui::SetClipboardText(snap.c_str());
            jce_toast_success("%s (%zu B)",
                              jce_editor_i18n("profiler.toast.copied"),
                              snap.size());
        }
        jce_editor::help_tip(jce_editor_i18n("profiler.tooltip.copyAll"));
        ImGui::Spacing();
    }

    /* ── Frame-time graph (modern sparkline) ───────────────────── */
    (void)fps_avg;
    draw_area_chart(jce_editor_i18n("profiler.bar.frame"), ring_frame,
                    s_prof.filled, dt_ms, avg, mx, plot_max, " ms",
                    jce_theme::text_primary(), 16.6f, 84.0f);

    /* ── Frame budget (avg cpu/gpu/wait/frame over the window) ─── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.frameBudget"));
    draw_bars_block("avg_bars", cpu_avg, gpu_avg, w_avg, avg);

    /* ── Per-series mini-plots ─────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.series"));
    {
        float cur_cpu  = s_prof.filled > 0 ? ring_cpu (nullptr, s_prof.filled - 1) : 0.0f;
        float cur_gpu  = s_prof.filled > 0 ? ring_gpu (nullptr, s_prof.filled - 1) : 0.0f;
        float cur_wait = s_prof.filled > 0 ? ring_wait(nullptr, s_prof.filled - 1) : 0.0f;
        float cur_vram = s_prof.filled > 0 ? ring_vram(nullptr, s_prof.filled - 1) : 0.0f;
        const ImU32 c_cpu  = JCE_COL32_STATUS_OK;
        const ImU32 c_gpu  = IM_COL32( 80, 160, 230, 255);
        const ImU32 c_wait = IM_COL32(220, 160,  60, 255);
        const ImU32 c_vram = IM_COL32(170, 130, 230, 255);
        if (ImGui::BeginTable("prof_series", 2,
                              ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            draw_area_chart(jce_editor_i18n("profiler.bar.cpu"), ring_cpu, s_prof.filled,
                            cur_cpu, cpu_avg, cpu_mx, std::max(8.0f, cpu_mx * 1.2f),
                            " ms", c_cpu, 16.6f, 64.0f);
            ImGui::TableNextColumn();
            draw_area_chart(jce_editor_i18n("profiler.bar.gpu"), ring_gpu, s_prof.filled,
                            cur_gpu, gpu_avg, gpu_mx, std::max(8.0f, gpu_mx * 1.2f),
                            " ms", c_gpu, 16.6f, 64.0f);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            draw_area_chart(jce_editor_i18n("profiler.bar.wait"), ring_wait, s_prof.filled,
                            cur_wait, w_avg, w_mx, std::max(4.0f, w_mx * 1.2f),
                            " ms", c_wait, 0.0f, 64.0f);
            ImGui::TableNextColumn();
            draw_area_chart(jce_editor_i18n("profiler.label.vram"), ring_vram, s_prof.filled,
                            cur_vram, v_avg, v_mx, std::max(64.0f, v_mx * 1.1f),
                            " MB", c_vram, 0.0f, 64.0f);
            ImGui::EndTable();
        }
    }

    /* ── Engine CPU phases (per frame, from jce_perf_phase) — a plain
     * always-visible breakdown below the charts + budget (NOT collapsed). */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.cpuPhases"));
    {
        struct PhaseRow { const char *name; double ms; };
        PhaseRow rows[64]; int rn = 0;
        int pc = jce_perf_phase_count();
        for (int i = 0; i < pc && rn < 64; ++i) {
            const char *nm = NULL; double ms = 0.0;
            if (!jce_perf_phase_peek_frame(i, &nm, &ms) || !nm) continue;
            if (ms < 0.01) continue;
            rows[rn].name = nm; rows[rn].ms = ms; rn++;
        }
        std::sort(rows, rows + rn,
                  [](const PhaseRow &a, const PhaseRow &b) { return a.ms > b.ms; });
        if (rn == 0) {
            ImGui::TextDisabled("%s", jce_editor_i18n("profiler.phases.accumulating"));
        } else {
            /* Clean horizontal bar-list: each phase a rounded bar whose LENGTH is
             * its share of the biggest phase, name inset left, "ms · %" right.
             * Top row (the bottleneck) amber, the rest CPU-green — one coherent
             * breakdown, cohesive with the series charts. */
            const int shown = rn < 14 ? rn : 14;
            double total = 0.0;
            for (int i = 0; i < rn; ++i) total += rows[i].ms;
            double maxms = rows[0].ms > 0.0 ? rows[0].ms : 1.0;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float rowh = ImGui::GetTextLineHeight() + 7.0f;
            const ImU32 top_col = IM_COL32(232, 156, 68, 160);
            const ImU32 col     = (JCE_COL32_STATUS_OK & IM_COL32(255, 255, 255, 0))
                                | IM_COL32(0, 0, 0, 150);
            for (int i = 0; i < shown; ++i) {
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                float w = ImGui::GetContentRegionAvail().x;
                if (w < 80.0f) w = 80.0f;
                ImVec2 p1 = ImVec2(p0.x + w, p0.y + rowh);
                dl->AddRectFilled(p0, p1, jce_theme::track_even(), 3.0f);
                float fw = w * (float)(rows[i].ms / maxms);
                if (fw < 2.0f) fw = 2.0f;
                dl->AddRectFilled(p0, ImVec2(p0.x + fw, p1.y),
                                  i == 0 ? top_col : col, 3.0f);
                dl->AddText(ImVec2(p0.x + 7.0f, p0.y + 3.5f),
                            jce_theme::text_primary(), rows[i].name);
                char b[64];
                std::snprintf(b, sizeof b, "%.2f ms  \xC2\xB7  %.0f%%",
                              rows[i].ms,
                              total > 0.0 ? 100.0 * rows[i].ms / total : 0.0);
                float tw = ImGui::CalcTextSize(b).x;
                dl->AddText(ImVec2(p1.x - tw - 7.0f, p0.y + 3.5f),
                            jce_theme::text_secondary(), b);
                ImGui::Dummy(ImVec2(w, rowh));
            }
        }
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

    /* ── Scene culling ─────────────────────────────────────────── */
    {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        JceSceneCullStats cs = { 0, 0, 0, false };
        if (sr) jce_scene_renderer_get_cull_stats(sr, &cs);

        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("profiler.section.sceneCulling"));
        if (ImGui::BeginTable("prof_cull", 2,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
            auto row = [](const char *k, const char *v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
            };
            char buf[64];

            row(jce_editor_i18n("profiler.row.cullMode"),
                cs.enabled ? jce_editor_i18n("profiler.value.cullModeOn")
                        : jce_editor_i18n("profiler.value.cullModeOff"));
            snprintf(buf, sizeof(buf), "%u", cs.total);
            row(jce_editor_i18n("profiler.row.cullTotal"), buf);
            snprintf(buf, sizeof(buf), "%u", cs.visible);
            row(jce_editor_i18n("profiler.row.cullVisible"), buf);
            snprintf(buf, sizeof(buf), "%u", cs.culled);
            row(jce_editor_i18n("profiler.row.cullCulled"), buf);
            const float pct = cs.total > 0
                                ? 100.0f * (float)cs.culled / (float)cs.total
                                : 0.0f;
            snprintf(buf, sizeof(buf), "%.1f %%", pct);
            row(jce_editor_i18n("profiler.row.cullRatio"), buf);
            /* Persistent extent-sized broad-phase (large-world-opt P1 #4).
            * Developer diagnostics — kept as literal labels (not i18n keys) so
            * they don't fan out across the 13 shipped locales. */
            snprintf(buf, sizeof(buf), "%ux%ux%u",
                    cs.grid_res[0], cs.grid_res[1], cs.grid_res[2]);
            row("Grid res", buf);
            snprintf(buf, sizeof(buf), "%u / %u", cs.grid_occupied, cs.grid_cells);
            row("Grid occupied / total", buf);
            snprintf(buf, sizeof(buf), "%u", cs.grid_objects);
            row("Grid objects", buf);
            snprintf(buf, sizeof(buf), "%u / %u / %u",
                    cs.inserted, cs.updated, cs.removed);
            row("Churn ins/upd/rem", buf);
            ImGui::EndTable();
        }
        if (cs.enabled && cs.total > 0) {
            float frac = (float)cs.culled / (float)cs.total;
            ImU32 c = (frac >= 0.5f) ? JCE_COL32_STATUS_OK
                                    : IM_COL32(160, 200, 240, 255);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
            ImGui::ProgressBar(frac, ImVec2(-1, 6.0f), "");
            ImGui::PopStyleColor();
        }
    }

    /* ── Scene LOD ─────────────────────────────────────────────── */
    {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        JceSceneLodStats ls = {};
        if (sr) jce_scene_renderer_get_lod_stats(sr, &ls);

        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("profiler.section.sceneLod"));
        if (ImGui::BeginTable("prof_lod", 2,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
            auto row = [](const char *k, const char *v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
            };
            char buf[96];
            row(jce_editor_i18n("profiler.row.lodMode"),
                ls.enabled ? jce_editor_i18n("profiler.value.lodModeOn")
                           : jce_editor_i18n("profiler.value.lodModeOff"));
            if (ls.enabled) {
                for (int i = 0; i < ls.level_count && i < JCE_SCENE_LOD_MAX_LEVELS; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "L%d", i);
                    snprintf(buf, sizeof(buf), "%u", ls.picks[i]);
                    row(key, buf);
                }
                snprintf(buf, sizeof(buf), "%u", ls.culled);
                row(jce_editor_i18n("profiler.row.lodCulled"), buf);
            }
            ImGui::EndTable();
        }
    }

    /* ── Render queue (GPU instancing) ─────────────────────────── */
    {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        JceSceneRqStats rqs = {};
        if (sr) jce_scene_renderer_get_rq_stats(sr, &rqs);

        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("profiler.section.renderQueue"));
        if (ImGui::BeginTable("prof_rq", 2,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
            auto row = [](const char *k, const char *v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
            };
            char buf[96];
            row(jce_editor_i18n("profiler.row.rqMode"),
                rqs.enabled ? jce_editor_i18n("profiler.value.rqOn")
                            : jce_editor_i18n("profiler.value.rqOff"));
            if (rqs.enabled) {
                snprintf(buf, sizeof(buf), "%u", rqs.commands_in);
                row(jce_editor_i18n("profiler.row.rqCommandsIn"), buf);
                snprintf(buf, sizeof(buf), "%u", rqs.submits_out);
                row(jce_editor_i18n("profiler.row.rqSubmits"), buf);
                snprintf(buf, sizeof(buf), "%u", rqs.batches_merged);
                row(jce_editor_i18n("profiler.row.rqInstancedBatches"), buf);
                snprintf(buf, sizeof(buf), "%u", rqs.instances_total);
                row(jce_editor_i18n("profiler.row.rqInstancesTotal"), buf);
                if (rqs.commands_in > 0 && rqs.submits_out > 0) {
                    float ratio = (float)rqs.commands_in / (float)rqs.submits_out;
                    snprintf(buf, sizeof(buf), "%.1fx", ratio);
                    row(jce_editor_i18n("profiler.row.rqMergeRatio"), buf);
                }
            }
            ImGui::EndTable();
        }
    }

    /* ── World streaming ───────────────────────────────────────── */
    {
        JceWorldStreamer *ws = jce_editor_get_world_streamer();

        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("profiler.section.worldStreaming"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", jce_editor_i18n("profiler.tooltip.wsBudget"));
        if (ImGui::BeginTable("prof_ws", 2,
                            ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
            auto row = [](const char *k, const char *v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
            };
            /* Pressure-level row: OK green / SOFT yellow / HARD red. */
            auto pressure_color = [](JceStreamingPressure p) -> ImVec4 {
                switch (p) {
                case JCE_STREAM_PRESSURE_HARD:
                    return ImVec4(0.95f, 0.35f, 0.30f, 1.0f);
                case JCE_STREAM_PRESSURE_SOFT:
                    return ImVec4(0.95f, 0.85f, 0.30f, 1.0f);
                case JCE_STREAM_PRESSURE_OK:
                default:
                    return ImVec4(0.35f, 0.90f, 0.35f, 1.0f);
                }
            };
            auto row_pressure = [&pressure_color](const char *k,
                                                JceStreamingPressure p) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn();
                ImGui::TextColored(pressure_color(p), "%s",
                                jce_streaming_pressure_name(p));
            };
            char buf[96];
            if (!ws) {
                row(jce_editor_i18n("profiler.row.wsStatus"),
                    jce_editor_i18n("profiler.value.wsInactive"));
            } else {
                uint32_t loaded  = jce_world_streamer_loaded_count(ws);
                uint32_t pending = jce_world_streamer_pending_count(ws);
                uint32_t total   = jce_world_streamer_chunk_count(ws);
                uint64_t mem     = jce_world_streamer_memory_used(ws);
                uint32_t ents    = jce_world_streamer_entity_count(ws);
                uint32_t evicted = jce_world_streamer_evicted_count(ws);
                uint32_t refused = jce_world_streamer_refused_loads(ws);
                JceWorldStreamConfig cfg = jce_world_streamer_get_config(ws);

                snprintf(buf, sizeof(buf), "%u / %u", loaded, total);
                row(jce_editor_i18n("profiler.row.wsChunksLoaded"), buf);
                snprintf(buf, sizeof(buf), "%u", pending);
                row(jce_editor_i18n("profiler.row.wsPending"), buf);
                snprintf(buf, sizeof(buf), "%.2f MB",
                        (double)mem / (1024.0 * 1024.0));
                row(jce_editor_i18n("profiler.row.wsMemory"), buf);
                snprintf(buf, sizeof(buf), "%u", ents);
                row(jce_editor_i18n("profiler.row.wsEntities"), buf);

                row_pressure(jce_editor_i18n("profiler.row.wsPressure"),
                            jce_world_streamer_pressure(ws));
                row_pressure(jce_editor_i18n("profiler.row.wsPressureHighWater"),
                            jce_world_streamer_pressure_high_water(ws));

                /* Budget counts raw chunk-file bytes (see section tooltip). */
                snprintf(buf, sizeof(buf), "%.2f / %u MB",
                        (double)mem / (1024.0 * 1024.0), cfg.budget_mb);
                row(jce_editor_i18n("profiler.row.wsBudget"), buf);
                snprintf(buf, sizeof(buf), "%u", evicted);
                row(jce_editor_i18n("profiler.row.wsEvicted"), buf);
                snprintf(buf, sizeof(buf), "%u", refused);
                row(jce_editor_i18n("profiler.row.wsRefused"), buf);
            }
            ImGui::EndTable();
        }
    }

    /* ── Occlusion culling ─────────────────────────────────────── */
    {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        JceSceneOcclusionStats ocs = {};
        if (sr) jce_scene_renderer_get_occlusion_stats(sr, &ocs);

        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("profiler.section.occlusion"));
        if (ImGui::BeginTable("prof_oc", 2,
                              ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg)) {
            auto row = [](const char *k, const char *v) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
            };
            char buf[96];
            row(jce_editor_i18n("profiler.row.ocMode"),
                ocs.enabled ? jce_editor_i18n("profiler.value.ocOn")
                            : jce_editor_i18n("profiler.value.ocOff"));
            if (ocs.enabled) {
                snprintf(buf, sizeof(buf), "%u", ocs.total);
                row(jce_editor_i18n("profiler.row.ocTested"), buf);
                snprintf(buf, sizeof(buf), "%u", ocs.visible);
                row(jce_editor_i18n("profiler.row.ocVisible"), buf);
                snprintf(buf, sizeof(buf), "%u", ocs.occluded);
                row(jce_editor_i18n("profiler.row.ocOccluded"), buf);
                snprintf(buf, sizeof(buf), "%u", ocs.warm_up);
                row(jce_editor_i18n("profiler.row.ocWarmup"), buf);
                if (ocs.total > 0) {
                    float pct = 100.0f * (float)ocs.occluded / (float)ocs.total;
                    snprintf(buf, sizeof(buf), "%.1f%%", (double)pct);
                    row(jce_editor_i18n("profiler.row.ocCullRate"), buf);
                }
            }
            ImGui::EndTable();
        }
        if (ocs.enabled && ocs.total > 0) {
            float frac = (float)ocs.occluded / (float)ocs.total;
            ImU32 c = (frac >= 0.3f) ? JCE_COL32_STATUS_OK
                                    : IM_COL32(160, 200, 240, 255);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
            ImGui::ProgressBar(frac, ImVec2(-1, 6.0f), "");
            ImGui::PopStyleColor();
        }
    }

    /* ── Process memory (mimalloc) ─────────────────────────────── */
    {
        JceMemStats ms = {};
        if (jce_mem_stats(&ms)) {
            ImGui::Spacing();
            ImGui::SeparatorText(jce_editor_i18n("profiler.section.processMemory"));
            if (ImGui::BeginTable("prof_mem", 2,
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_RowBg)) {
                auto row = [](const char *k, const char *v) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(k);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(v);
                };
                char buf[96];
                const double MB = 1024.0 * 1024.0;
                snprintf(buf, sizeof(buf), "%.1f MB", (double)ms.current_rss / MB);
                row(jce_editor_i18n("profiler.row.memRssCurrent"), buf);
                snprintf(buf, sizeof(buf), "%.1f MB", (double)ms.peak_rss / MB);
                row(jce_editor_i18n("profiler.row.memRssPeak"), buf);
                snprintf(buf, sizeof(buf), "%.1f MB", (double)ms.current_commit / MB);
                row(jce_editor_i18n("profiler.row.memCommitCurrent"), buf);
                snprintf(buf, sizeof(buf), "%.1f MB", (double)ms.peak_commit / MB);
                row(jce_editor_i18n("profiler.row.memCommitPeak"), buf);
                snprintf(buf, sizeof(buf), "%zu", ms.page_faults);
                row(jce_editor_i18n("profiler.row.memPageFaults"), buf);
                ImGui::EndTable();
            }
            if (ms.peak_rss > 0) {
                float frac = (float)((double)ms.current_rss / (double)ms.peak_rss);
                ImU32 c = IM_COL32(206, 147, 216, 255);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
                ImGui::ProgressBar(frac, ImVec2(-1, 6.0f), "");
                ImGui::PopStyleColor();
            }
        }
    }

    /* ── View hot-list ─────────────────────────────────────────── */
    ImGui::Spacing();
    ImGui::SeparatorText(jce_editor_i18n("profiler.section.viewHotspots"));
    draw_view_table();
}

} /* anonymous namespace */

/* ──────────────────────────────────────────────────────────────────
 * Profiling Workbench tabs
 *
 * Profiler hosts Memory Profiler / Profile Analyzer / Frame Debugger
 * as sibling tabs of an outer TabBar.  Sibling panels remain registered
 * (JCE_PANEL_MEMORY_PROFILER / _PROFILE_ANALYZER / _FRAME_DEBUGGER) and
 * route here via jce_panel_profiler_request_tab().
 * ────────────────────────────────────────────────────────────────── */

extern "C" void jce_editor_panel_memory_profiler_content(void);
extern "C" void jce_editor_panel_profile_analyzer_content(void);
extern "C" void jce_editor_panel_frame_debugger_content(void);
extern "C" void jce_editor_panel_benchmark_content(void);

namespace {

int g_request_tab = -1;
int g_current_tab = 0;  /* mirror of active profiling TabItem for menu markers */
bool g_tab_state_loaded = false;

static const char *k_tab_state_key = "panel.profiler.current_tab";

bool valid_tab(int idx)
{
    return idx >= 0 && idx <= 4;
}

void ensure_tab_state_loaded(void)
{
    if (g_tab_state_loaded)
        return;
    /* Clamp must span all 5 tabs (0..4, see valid_tab) — a tighter max
     * silently remaps a persisted Benchmark tab onto Frame Debugger. */
    g_current_tab = jce_editor_ui_state_load_int(k_tab_state_key, 0, 0, 4);
    g_request_tab = g_current_tab;
    g_tab_state_loaded = true;
}

void set_current_tab(int idx)
{
    if (!valid_tab(idx) || g_current_tab == idx)
        return;
    g_current_tab = idx;
    if (g_tab_state_loaded)
        jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

void draw_workbench(void)
{
    ensure_tab_state_loaded();
    if (!ImGui::BeginTabBar("##profiling_tabs"))
        return;

    ImGuiTabItemFlags cpu_flags = (g_request_tab == 0) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags mem_flags = (g_request_tab == 1) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags ana_flags = (g_request_tab == 2) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags fd_flags  = (g_request_tab == 3) ? ImGuiTabItemFlags_SetSelected : 0;
    ImGuiTabItemFlags bm_flags  = (g_request_tab == 4) ? ImGuiTabItemFlags_SetSelected : 0;

    char cpu_label[96];
    char mem_label[96];
    char ana_label[96];
    char fd_label [96];
    char bm_label [96];
    std::snprintf(cpu_label, sizeof(cpu_label), "%s###pf_tab_cpu",
                  jce_editor_i18n("profiler.title"));
    std::snprintf(mem_label, sizeof(mem_label), "%s###pf_tab_memory",
                  jce_editor_i18n("memoryProfiler.title"));
    std::snprintf(ana_label, sizeof(ana_label), "%s###pf_tab_analyzer",
                  jce_editor_i18n("profileAnalyzer.title"));
    std::snprintf(fd_label,  sizeof(fd_label),  "%s###pf_tab_framedbg",
                  jce_editor_i18n("frameDebugger.title"));
    std::snprintf(bm_label,  sizeof(bm_label),  "%s###pf_tab_benchmark",
                  jce_editor_i18n("benchmark.title"));

    if (ImGui::BeginTabItem(cpu_label, nullptr, cpu_flags)) {
        set_current_tab(0);
        draw_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(mem_label, nullptr, mem_flags)) {
        set_current_tab(1);
        jce_editor_panel_memory_profiler_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(ana_label, nullptr, ana_flags)) {
        set_current_tab(2);
        jce_editor_panel_profile_analyzer_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(fd_label, nullptr, fd_flags)) {
        set_current_tab(3);
        jce_editor_panel_frame_debugger_content();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(bm_label, nullptr, bm_flags)) {
        set_current_tab(4);
        jce_editor_panel_benchmark_content();
        ImGui::EndTabItem();
    }

    ImGui::EndTabBar();
    g_request_tab = -1;
}

} /* anonymous namespace */

/* Shared per-view GPU/CPU table — the single implementation, consumed by
 * both the CPU tab and the Frame Debugger tab (which used to carry its
 * own duplicate).  The CPU tab's sampler only runs while that tab is
 * active, so refresh the snapshot here when this frame hasn't sampled. */
extern "C" void jce_panel_profiler_draw_view_table(void)
{
    if (s_prof.view_rows_frame != ImGui::GetFrameCount()) {
        if (const JceFrameStats *st = jce_gfx_stats_capture()) {
            s_prof.cpu_freq = st->cpu_timer_freq > 0 ? st->cpu_timer_freq : 1;
            s_prof.gpu_freq = st->gpu_timer_freq > 0 ? st->gpu_timer_freq : 1;
            refill_view_rows(st);
        }
    }
    draw_view_table();
}

extern "C" void jce_panel_profiler_request_tab(int idx)
{
    if (!valid_tab(idx))
        return;
    g_request_tab = idx;
    g_current_tab = idx;
    jce_editor_ui_state_save_int(k_tab_state_key, idx);
}

extern "C" int jce_panel_profiler_current_tab(void)
{
    ensure_tab_state_loaded();
    return g_current_tab;
}

extern "C" void jce_editor_panel_profiler_content(void)
{
    draw_workbench();
}
