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

/* ── Intermediate render targets ──────────────────────────────────────
 *
 * The engine owns a dozen of these -- the depth pre-pass, the
 * normal/albedo/velocity G-buffer, the shadow maps, the sky LUTs -- and until
 * now nothing in the editor could look at one, so a wrong G-buffer was only
 * ever visible as a wrong final image with every downstream pass a suspect.
 *
 * The engine refuses to list a target that does not hold real pixels THIS
 * frame, so the list changes length as features turn on.  That is why the
 * selection below is remembered by NAME: an index would slide onto a
 * different target at exactly the moment someone is watching one. */
static void draw_render_targets(void)
{
    if (!ImGui::CollapsingHeader(
            jce_editor_i18n_id("frameDebugger.targets.section",
                               "Render targets")))
        return;

    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    const int n = sr ? jce_scene_renderer_debug_target_count(sr) : 0;

    if (n <= 0) {
        /* Naming the reason, because an empty section and "the renderer has
         * not drawn a frame with any of these enabled" are the same picture
         * and two different facts. */
        ImGui::TextDisabled("%s", jce_editor_i18n_id(
            "frameDebugger.targets.none",
            "No intermediate target holds real pixels this frame."));
        ImGui::TextDisabled("%s", jce_editor_i18n_id(
            "frameDebugger.targets.noneHint",
            "Entries appear as features turn on: the depth pre-pass with "
            "SSAO or SSR, a shadow map with a shadow-casting light, velocity "
            "with TAA."));
        return;
    }

    /* THE SELECTION LIVES IN IMGUI'S OWN PER-WINDOW STORAGE, not in a static
     * here.  Three reasons, and only the first is about tidiness:
     *
     *   - a file static is process-global state, which this tree counts and
     *     is right to; the state already has an owner, so storing it again
     *     is a second copy of one fact;
     *   - two docked copies of this panel would share one static and fight
     *     over the selection;
     *   - ImGui already persists this storage across frames and docking, so
     *     the static bought nothing it does not already provide.
     *
     * The key is ImGui::GetID(name) rather than the name itself: the storage
     * holds ints, and a hash of the NAME keeps the property that matters --
     * the list changes length as features turn on, so an index would slide
     * onto a different target at exactly the moment someone is watching one.
     *
     * A remembered target that is absent this frame is not an error, it is
     * the feature being off: the selection falls back to the first entry and
     * re-selects itself when the feature returns. */
    ImGuiStorage *store = ImGui::GetStateStorage();
    const ImGuiID key   = ImGui::GetID("jce_rt_selected");
    const int want      = store->GetInt(key, 0);

    int sel = 0;
    JceRenderTargetInfo info = {};
    for (int i = 0; i < n; ++i) {
        JceRenderTargetInfo probe = {};
        if (!jce_scene_renderer_debug_target_get(sr, i, &probe)) continue;
        if (want && probe.name &&
            (int)ImGui::GetID(probe.name) == want) { sel = i; break; }
    }
    if (!jce_scene_renderer_debug_target_get(sr, sel, &info)) return;

    if (ImGui::BeginCombo(jce_editor_i18n_id("frameDebugger.targets.pick",
                                             "Target"), info.name)) {
        for (int i = 0; i < n; ++i) {
            JceRenderTargetInfo it = {};
            if (!jce_scene_renderer_debug_target_get(sr, i, &it)) continue;
            const bool is_sel = (i == sel);
            if (ImGui::Selectable(it.name, is_sel) && !is_sel) {
                store->SetInt(key, (int)ImGui::GetID(it.name));
                info = it;
                sel  = i;
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    if (info.width && info.height)
        ImGui::TextDisabled("%s: %u x %u",
            jce_editor_i18n_id("frameDebugger.targets.size", "Size"),
            (unsigned)info.width, (unsigned)info.height);
    else
        ImGui::TextDisabled("%s: %s",
            jce_editor_i18n_id("frameDebugger.targets.size", "Size"),
            jce_editor_i18n_id("frameDebugger.targets.unknownSize",
                               "not recorded by the renderer"));

    /* The note is not decoration.  Most of these are not pictures of a
     * scene, and a viewer that shows a velocity buffer with no explanation
     * has shown a flat grey rectangle and said nothing. */
    if (info.note && info.note[0]) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", info.note);
        ImGui::PopTextWrapPos();
    }

    if (info.texture.idx == UINT16_MAX) return;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = avail.x;
    if (w < 64.0f) w = 64.0f;
    float h = w;
    if (info.width && info.height)
        h = w * ((float)info.height / (float)info.width);
    if (h > 512.0f) { w *= 512.0f / h; h = 512.0f; }
    /* +1 so a valid bgfx idx 0 is not ImTextureID_Invalid, the same encoding
     * the Game and Scene views use. */
    ImGui::Image((ImTextureID)(uintptr_t)((uint32_t)info.texture.idx + 1u),
                 ImVec2(w, h));
}

/* ── Public entry points ────────────────────────────────────────────── */

extern "C" void jce_editor_panel_frame_debugger_content(void)
{
    JceGpuStats stats;
    bool ok = jce_renderer_get_gpu_stats(&stats);
    draw_frame_totals(ok ? &stats : nullptr);
    draw_view_stats();
    draw_renderer_breakdown();
    draw_render_targets();
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
