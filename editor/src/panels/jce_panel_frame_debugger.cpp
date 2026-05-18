/*
 * jce_panel_frame_debugger.cpp  Frame Debugger window (Sprint 3 / 0.8.22)
 *
 * Mirrors Unity's Window > Analysis > Frame Debugger. Two complementary
 * data sources:
 *
 *   1. bgfx_get_stats()  live, per-frame snapshot:
 *        - Frame totals: numDraw, numCompute, numBlit, GPU/CPU times,
 *          gpu memory used, draw resource counts.
 *        - viewStats[]: per-bgfx-view name + GPU time interval. We
 *          render this as a table sorted by view ID, the same axis on
 *          which jce_render_graph assigns passes.
 *
 *   2. jce_rg_frame_debug_*()  declarative render-graph capture:
 *        - "Capture next frame" button calls
 *          jce_rg_frame_debug_request_capture(); the next call to
 *          jce_rg_execute() fills a static buffer with each pass's
 *          name, view ID, read/write resource names, and culled flag.
 *        - The capture survives until overwritten so the user can pan
 *          through it after pausing or after switching panels.
 *
 * No engine consumer wires the render graph in production play yet, so
 * the second section will read "no capture available" until a game does
 * so. The first section always works.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>
#include <bgfx/c99/bgfx.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/renderer/jce_render_graph.h>
}

#define MAX_CAPTURED_PASSES 64

static JceRGFrameDebugPass s_passes[MAX_CAPTURED_PASSES];
static uint32_t            s_pass_count = 0;
static int                 s_selected   = -1;

/* ── Helpers ────────────────────────────────────────────────────────── */

static double ticks_to_ms(int64_t delta, int64_t freq)
{
    if (freq <= 0) return 0.0;
    return (double)delta * 1000.0 / (double)freq;
}

static void draw_frame_totals(const bgfx_stats_t *st)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.totals"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (!st) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.noStats"));
        return;
    }
    double cpu_ms     = ticks_to_ms(st->cpuTimeEnd  - st->cpuTimeBegin, st->cpuTimerFreq);
    double cpu_frame  = ticks_to_ms(st->cpuTimeFrame,                    st->cpuTimerFreq);
    double gpu_ms     = ticks_to_ms(st->gpuTimeEnd  - st->gpuTimeBegin, st->gpuTimerFreq);
    double wait_sub   = ticks_to_ms(st->waitSubmit,                      st->cpuTimerFreq);
    double wait_rnd   = ticks_to_ms(st->waitRender,                      st->cpuTimerFreq);

    ImGui::Text("%s: %u  |  %s: %u  |  %s: %u",
        jce_editor_i18n("frameDebugger.numDraw"),    st->numDraw,
        jce_editor_i18n("frameDebugger.numCompute"), st->numCompute,
        jce_editor_i18n("frameDebugger.numBlit"),    st->numBlit);
    ImGui::Text("%s: %.3f ms  (%s: %.3f ms)",
        jce_editor_i18n("frameDebugger.cpuFrame"),  cpu_frame,
        jce_editor_i18n("frameDebugger.cpuSubmit"), cpu_ms);
    ImGui::Text("%s: %.3f ms",
        jce_editor_i18n("frameDebugger.gpu"), gpu_ms);
    ImGui::Text("%s: %.3f ms  |  %s: %.3f ms",
        jce_editor_i18n("frameDebugger.waitSubmit"), wait_sub,
        jce_editor_i18n("frameDebugger.waitRender"), wait_rnd);
    ImGui::Separator();
    ImGui::Text("%s: %ux%u  |  %s: %u",
        jce_editor_i18n("frameDebugger.backbuffer"), st->width, st->height,
        jce_editor_i18n("frameDebugger.maxLatency"), st->maxGpuLatency);
    ImGui::Text("%s: tex=%u fb=%u prog=%u shd=%u  |  rt=%lld KB tex=%lld KB",
        jce_editor_i18n("frameDebugger.resources"),
        st->numTextures, st->numFrameBuffers, st->numPrograms, st->numShaders,
        (long long)(st->rtMemoryUsed      / 1024),
        (long long)(st->textureMemoryUsed / 1024));
    if (st->gpuMemoryMax > 0) {
        ImGui::Text("%s: %lld / %lld MB",
            jce_editor_i18n("frameDebugger.gpuMem"),
            (long long)(st->gpuMemoryUsed / (1024 * 1024)),
            (long long)(st->gpuMemoryMax  / (1024 * 1024)));
    }
}

static void draw_view_stats(const bgfx_stats_t *st)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.views"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (!st || st->numViews == 0 || !st->viewStats) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.noViews"));
        return;
    }

    if (ImGui::BeginTable("##views", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, 220))) {
        ImGui::TableSetupColumn(jce_editor_i18n("frameDebugger.col.viewId"),
                                ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("frameDebugger.col.viewName"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("frameDebugger.col.gpuMs"),
                                ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(jce_editor_i18n("frameDebugger.col.cpuMs"),
                                ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();

        for (uint16_t i = 0; i < st->numViews; i++) {
            const bgfx_view_stats_t *vs = &st->viewStats[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::Text("%u", (uint32_t)vs->view);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(vs->name[0] ? vs->name : "(unnamed)");
            ImGui::TableSetColumnIndex(2);
            double gpu_ms = ticks_to_ms(vs->gpuTimeEnd - vs->gpuTimeBegin, st->gpuTimerFreq);
            ImGui::Text("%.3f", gpu_ms);
            ImGui::TableSetColumnIndex(3);
            double cpu_ms = ticks_to_ms(vs->cpuTimeEnd - vs->cpuTimeBegin, st->cpuTimerFreq);
            ImGui::Text("%.3f", cpu_ms);
        }
        ImGui::EndTable();
    }
}

static void draw_rg_capture(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.rg"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (ImGui::Button(jce_editor_i18n("frameDebugger.capture"))) {
        jce_rg_frame_debug_request_capture();
    }
    ImGui::SameLine();
    if (jce_rg_frame_debug_pending()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "%s",
                           jce_editor_i18n("frameDebugger.pending"));
    } else if (s_pass_count > 0) {
        ImGui::TextDisabled("%s: %u", jce_editor_i18n("frameDebugger.passes"),
                            (uint32_t)s_pass_count);
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.notCaptured"));
    }

    /* Pull the latest capture each frame; cheap (memcpy ≤ 64 entries). */
    uint32_t fresh_n = 0;
    if (jce_rg_frame_debug_get(s_passes, MAX_CAPTURED_PASSES, &fresh_n)) {
        s_pass_count = fresh_n;
        if (s_selected >= (int)s_pass_count) s_selected = -1;
    }

    if (s_pass_count == 0) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.noRgHint"));
        return;
    }

    ImGui::BeginChild("##fd_pass_list", ImVec2(260, 240), true);
    for (uint32_t i = 0; i < s_pass_count; i++) {
        const JceRGFrameDebugPass *p = &s_passes[i];
        char lbl[160];
        snprintf(lbl, sizeof(lbl), "[%u] %s%s",
                 (uint32_t)p->view_id, p->name,
                 p->culled ? " (culled)" : "");
        bool sel = (int)i == s_selected;
        if (ImGui::Selectable(lbl, sel)) s_selected = (int)i;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##fd_pass_detail", ImVec2(0, 240), true);
    if (s_selected < 0 || s_selected >= (int)s_pass_count) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.selectHint"));
    } else {
        const JceRGFrameDebugPass *p = &s_passes[s_selected];
        ImGui::Text("%s: %s", jce_editor_i18n("frameDebugger.detail.name"), p->name);
        ImGui::Text("%s: %u",  jce_editor_i18n("frameDebugger.detail.view"), (uint32_t)p->view_id);
        ImGui::Text("%s: %s",  jce_editor_i18n("frameDebugger.detail.culled"),
                    p->culled ? "yes" : "no");
        ImGui::Separator();
        ImGui::Text("%s (%u):", jce_editor_i18n("frameDebugger.detail.reads"),
                    (uint32_t)p->read_count);
        uint16_t rn = p->read_count > 8 ? 8 : p->read_count;
        for (uint16_t k = 0; k < rn; k++) ImGui::BulletText("%s", p->read_names[k]);
        if (p->read_count > rn) ImGui::TextDisabled(jce_editor_i18n("frameDebugger.moreFmt"), p->read_count - rn);
        ImGui::Separator();
        ImGui::Text("%s (%u):", jce_editor_i18n("frameDebugger.detail.writes"),
                    (uint32_t)p->write_count);
        uint16_t wn = p->write_count > 8 ? 8 : p->write_count;
        for (uint16_t k = 0; k < wn; k++) ImGui::BulletText("%s", p->write_names[k]);
        if (p->write_count > wn) ImGui::TextDisabled(jce_editor_i18n("frameDebugger.moreFmt"), p->write_count - wn);
    }
    ImGui::EndChild();
}

/* ── Public entry points ────────────────────────────────────────────── */

extern "C" void jce_editor_panel_frame_debugger_content(void)
{
    const bgfx_stats_t *st = bgfx_get_stats();
    draw_frame_totals(st);
    draw_view_stats(st);
    draw_rg_capture();
}

extern "C" void jce_editor_panel_frame_debugger(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_FRAME_DEBUGGER);
    if (!vis || !*vis) return;
    char lbl[128];
    snprintf(lbl, sizeof(lbl), "%s###frame_debugger",
             jce_editor_i18n("frameDebugger.title"));
    if (ImGui::Begin(lbl, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_frame_debugger_content();
    ImGui::End();
}
