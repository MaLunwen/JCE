/*
 * jce_panel_frame_debugger.cpp  Frame Debugger tab (Profiling workbench)
 *
 * Mirrors Unity's Window > Analysis > Frame Debugger, on three REAL
 * per-frame data sources:
 *
 *   1. bgfx_get_stats() totals: draw/compute/blit counts, CPU/GPU times,
 *      backbuffer + resource counts, GPU memory.
 *   2. The per-view GPU/CPU table — the Profiling workbench's single
 *      shared implementation (jce_panel_profiler_draw_view_table); this
 *      file used to carry a duplicate copy.
 *   3. Scene-renderer submission breakdown: render-queue flush stats
 *      (commands → submits, instance merging) and LOD pick counts from
 *      the engine's per-frame counters.
 *
 * The former "render-graph capture" section was removed: jce_rg_execute
 * has no production caller (the scene renderer drives bgfx views
 * directly), so its capture button could never produce data.  The
 * engine-side jce_rg debug API remains for future graph adoption.
 */

#include "ui/jce_editor_colors.h"
#include "jce_panel_common.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_scene_renderer.h>
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static void draw_frame_totals(const JceGpuStats *st)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.totals"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (!st || !st->valid) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.noStats"));
        return;
    }

    ImGui::Text("%s: %u  |  %s: %u  |  %s: %u",
        jce_editor_i18n("frameDebugger.numDraw"),    st->num_draw,
        jce_editor_i18n("frameDebugger.numCompute"), st->num_compute,
        jce_editor_i18n("frameDebugger.numBlit"),    st->num_blit);
    ImGui::Text("%s: %.3f ms  (%s: %.3f ms)",
        jce_editor_i18n("frameDebugger.cpuFrame"),  st->cpu_frame_ms,
        jce_editor_i18n("frameDebugger.cpuSubmit"), st->cpu_submit_ms);
    ImGui::Text("%s: %.3f ms",
        jce_editor_i18n("frameDebugger.gpu"), st->gpu_ms);
    ImGui::Text("%s: %.3f ms  |  %s: %.3f ms",
        jce_editor_i18n("frameDebugger.waitSubmit"), st->wait_submit_ms,
        jce_editor_i18n("frameDebugger.waitRender"), st->wait_render_ms);
    ImGui::Separator();
    ImGui::Text("%s: %ux%u  |  %s: %u",
        jce_editor_i18n("frameDebugger.backbuffer"),
        st->backbuffer_width, st->backbuffer_height,
        jce_editor_i18n("frameDebugger.maxLatency"), st->max_gpu_latency);
    ImGui::Text("%s: tex=%u fb=%u prog=%u shd=%u  |  rt=%lld KB tex=%lld KB",
        jce_editor_i18n("frameDebugger.resources"),
        st->num_textures, st->num_frame_buffers, st->num_programs, st->num_shaders,
        (long long)(st->rt_memory_used      / 1024),
        (long long)(st->texture_memory_used / 1024));
    if (st->gpu_memory_max > 0) {
        ImGui::Text("%s: %lld / %lld MB",
            jce_editor_i18n("frameDebugger.gpuMem"),
            (long long)(st->gpu_memory_used / (1024 * 1024)),
            (long long)(st->gpu_memory_max  / (1024 * 1024)));
    }
}

/* Per-view GPU/CPU table — the shared Profiling-workbench widget; this
 * file used to carry its own duplicate copy of the table. */
static void draw_view_stats(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.views"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;
    jce_panel_profiler_draw_view_table();
}

/* Scene-renderer submission breakdown — real per-frame counters from the
 * engine (render-queue flush + LOD picks).  Replaces the former
 * render-graph capture section whose button could never produce data
 * (jce_rg_execute has no production caller). */
static void draw_renderer_breakdown(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("frameDebugger.section.renderer"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    if (!sr) {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.noStats"));
        return;
    }

    JceSceneRqStats rq = {};
    jce_scene_renderer_get_rq_stats(sr, &rq);
    if (rq.enabled) {
        ImGui::Text("%s: %u  \xE2\x86\x92  %s: %u",
            jce_editor_i18n("frameDebugger.rq.commands"),  rq.commands_in,
            jce_editor_i18n("frameDebugger.rq.submits"),   rq.submits_out);
        ImGui::Text("%s: %u  |  %s: %u",
            jce_editor_i18n("frameDebugger.rq.batches"),   rq.batches_merged,
            jce_editor_i18n("frameDebugger.rq.instances"), rq.instances_total);
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.rq.off"));
    }

    ImGui::Separator();

    JceSceneLodStats lod = {};
    jce_scene_renderer_get_lod_stats(sr, &lod);
    if (lod.enabled && lod.level_count > 0) {
        ImGui::TextUnformatted(jce_editor_i18n("frameDebugger.lod.header"));
        ImGui::SameLine();
        for (int i = 0; i < lod.level_count && i < JCE_SCENE_LOD_MAX_LEVELS; i++) {
            ImGui::SameLine();
            ImGui::Text("L%d: %u", i, lod.picks[i]);
        }
        if (lod.culled > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s: %u",
                jce_editor_i18n("frameDebugger.lod.culled"), lod.culled);
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("frameDebugger.lod.off"));
    }
}

/* ── Public entry points ────────────────────────────────────────────── */

extern "C" void jce_editor_panel_frame_debugger_content(void)
{
    JceGpuStats stats;
    bool ok = jce_renderer_get_gpu_stats(&stats);
    draw_frame_totals(ok ? &stats : nullptr);
    draw_view_stats();
    draw_renderer_breakdown();
}

/* Shim: Frame Debugger has been merged into the Profiler "Profiling"
 * workbench as a tab.  Activating this panel now redirects to that
 * workbench and requests the Frame Debugger tab.  Symbol kept so
 * menu/hotkey entries registered against JCE_PANEL_FRAME_DEBUGGER
 * keep working. */
extern "C" void jce_editor_panel_frame_debugger(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_FRAME_DEBUGGER,
                                        JCE_PANEL_PROFILER,
                                        "panel.profiler", "profiler"))
        jce_panel_profiler_request_tab(3);
}
