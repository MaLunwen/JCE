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
    bar(jce_editor_i18n("profiler.bar.cpu"),   cpu,   JCE_COL32_STATUS_OK);
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

    /* ── Top toolbar: status chip + Copy / Reset ───────────────── */
    {
        float fps_now = (dt_ms > 0.0f) ? 1000.0f / dt_ms : 0.0f;
        ImU32 chip_col;
        if (fps_now >= 55.0f)      chip_col = JCE_COL32_STATUS_OK;
        else if (fps_now >= 30.0f) chip_col = JCE_COL32_STATUS_WARN;
        else                       chip_col = IM_COL32(230,  90,  90, 255);

        ImGui::PushStyleColor(ImGuiCol_Text, chip_col);
        ImGui::Text(jce_editor_i18n("profiler.fpsChipFmt"), fps_now);   /* ● %.1f FPS */
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("%.2f ms (avg %.2f / max %.2f)", dt_ms, avg, mx);

        ImGui::SameLine();
        float region = ImGui::GetContentRegionAvail().x;
        const char *copy_lbl  = jce_editor_i18n("profiler.action.copyAll");
        const char *reset_lbl = jce_editor_i18n("profiler.action.resetHistory");
        float copy_w  = ImGui::CalcTextSize(copy_lbl).x  + ImGui::GetStyle().FramePadding.x * 2;
        float reset_w = ImGui::CalcTextSize(reset_lbl).x + ImGui::GetStyle().FramePadding.x * 2;
        float gap = ImGui::GetStyle().ItemSpacing.x;
        ImGui::Dummy(ImVec2(std::max(0.0f, region - copy_w - reset_w - gap * 2), 1));
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
        ImGui::Separator();
    }

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

namespace {

int g_request_tab = -1;
int g_current_tab = 0;  /* mirror of active profiling TabItem for menu markers */
bool g_tab_state_loaded = false;

static const char *k_tab_state_key = "panel.profiler.current_tab";

bool valid_tab(int idx)
{
    return idx >= 0 && idx <= 3;
}

void ensure_tab_state_loaded(void)
{
    if (g_tab_state_loaded)
        return;
    g_current_tab = jce_editor_ui_state_load_int(k_tab_state_key, 0, 0, 3);
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

    char cpu_label[96];
    char mem_label[96];
    char ana_label[96];
    char fd_label [96];
    std::snprintf(cpu_label, sizeof(cpu_label), "%s###pf_tab_cpu",
                  jce_editor_i18n("profiler.title"));
    /* No memoryProfiler.title key exists; layout historically uses a
     * literal "Memory Profiler" label too. */
    std::snprintf(mem_label, sizeof(mem_label), "Memory###pf_tab_memory");
    std::snprintf(ana_label, sizeof(ana_label), "%s###pf_tab_analyzer",
                  jce_editor_i18n("profileAnalyzer.title"));
    std::snprintf(fd_label,  sizeof(fd_label),  "%s###pf_tab_framedbg",
                  jce_editor_i18n("frameDebugger.title"));

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

    ImGui::EndTabBar();
    g_request_tab = -1;
}

} /* anonymous namespace */

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
