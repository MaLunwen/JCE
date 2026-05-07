/*
 * jce_panel_memory_profiler.cpp  Memory Profiler panel.
 *
 * Lightweight Unity-style memory snapshot:
 *   - Process working set + private bytes (Win32 PSAPI)
 *   - bgfx renderer stats (textures, buffers, GPU mem if backend reports)
 *   - Scene entity count
 *
 * Refreshes once per second to avoid PSAPI overhead in the UI thread.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_sysinfo.h>
#include <bgfx/c99/bgfx.h>
}

namespace {

struct MemSnapshot {
    double   last_refresh = 0.0;
    uint64_t working_set  = 0;
    uint64_t private_bytes= 0;
    uint64_t peak_ws      = 0;
    int      bgfx_textures= 0;
    int      bgfx_buffers = 0;
    int64_t  gpu_used     = -1;  /* -1 = unknown */
    int64_t  gpu_total    = -1;
    int      entity_count = 0;
} g_mem;

void refresh_now()
{
    jce_sysinfo_process_mem(&g_mem.working_set,
                            &g_mem.private_bytes,
                            &g_mem.peak_ws);
    const bgfx_stats_t *st = bgfx_get_stats();
    if (st) {
        g_mem.bgfx_textures = (int)st->numTextures;
        g_mem.bgfx_buffers  = (int)(st->numVertexBuffers + st->numIndexBuffers);
        g_mem.gpu_used  = st->gpuMemoryUsed;
        g_mem.gpu_total = st->gpuMemoryMax;
    }
}

const char *fmt_bytes(uint64_t b, char *out, size_t n)
{
    const char *u[] = {"B","KB","MB","GB","TB"};
    double v = (double)b; int k = 0;
    while (v >= 1024.0 && k < 4) { v /= 1024.0; k++; }
    snprintf(out, n, "%.2f %s", v, u[k]);
    return out;
}

static void count_cb(JceScene *, JceEntity, void *ud) { (*(int *)ud)++; }
} /* namespace */

extern "C" void jce_editor_panel_memory_profiler_content(void)
{
    double now = ImGui::GetTime();
    if (now - g_mem.last_refresh > 1.0) {
        refresh_now();
        JceScene *scene = jce_state_get_scene();
        g_mem.entity_count = 0;
        if (scene) jce_scene_each_entity(scene, count_cb, &g_mem.entity_count);
        g_mem.last_refresh = now;
    }

    char b1[32], b2[32], b3[32];

    if (ImGui::CollapsingHeader(jce_editor_i18n("memoryProfiler.process"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s:   %s", jce_editor_i18n("memoryProfiler.workingSet"),  fmt_bytes(g_mem.working_set,  b1, sizeof(b1)));
        ImGui::Text("%s: %s", jce_editor_i18n("memoryProfiler.privateBytes"), fmt_bytes(g_mem.private_bytes,b2, sizeof(b2)));
        ImGui::Text("%s:       %s", jce_editor_i18n("memoryProfiler.peakWS"), fmt_bytes(g_mem.peak_ws,      b3, sizeof(b3)));
    }

    if (ImGui::CollapsingHeader(jce_editor_i18n("memoryProfiler.gpuRenderer"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s:      %d", jce_editor_i18n("memoryProfiler.textures"), g_mem.bgfx_textures);
        ImGui::Text("%s:       %d", jce_editor_i18n("memoryProfiler.buffers"), g_mem.bgfx_buffers);
        if (g_mem.gpu_used >= 0 && g_mem.gpu_total > 0) {
            ImGui::Text("%s:       %s / %s", jce_editor_i18n("memoryProfiler.gpuMem"),
                        fmt_bytes((uint64_t)g_mem.gpu_used,  b1, sizeof(b1)),
                        fmt_bytes((uint64_t)g_mem.gpu_total, b2, sizeof(b2)));
            float frac = (float)g_mem.gpu_used / (float)g_mem.gpu_total;
            ImGui::ProgressBar(frac, ImVec2(-1, 0));
        } else {
            ImGui::TextDisabled("%s", jce_editor_i18n("memoryProfiler.gpuNotReported"));
        }
    }

    if (ImGui::CollapsingHeader(jce_editor_i18n("memoryProfiler.scene"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("%s:      %d", jce_editor_i18n("memoryProfiler.entities"), g_mem.entity_count);
    }

    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n("memoryProfiler.refreshNow"))) refresh_now();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("memoryProfiler.autoRefresh"));
}
