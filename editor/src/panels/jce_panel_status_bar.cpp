/*
 * jce_panel_status_bar.cpp  Bottom-of-window status bar.
 *
 * Shows: FPS | current scene name | * dirty marker | selection count |
 *        play state | (right-aligned) build status placeholder.
 *
 * Anchored to the bottom of the main viewport via SetNextWindowPos/Size,
 * styled like a slim toolbar (no title bar, no resize).
 */

#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_tip.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

static const char *play_state_label(JcePlayState ps)
{
    switch (ps) {
        case JCE_PLAY_PLAYING: return jce_editor_i18n("statusBar.play.playing");
        case JCE_PLAY_PAUSED:  return jce_editor_i18n("statusBar.play.paused");
        case JCE_PLAY_STOPPED:
        default:               return jce_editor_i18n("statusBar.play.stopped");
    }
}

static const char *short_scene_name(const char *path)
{
    if (!path || !*path) return jce_editor_i18n("statusBar.untitled");
    return jce_editor_path_basename_view(path);
}

/* GPU tier widget — colored label + click popup to override the
 * renderer-detected tier (editor-session only; never persisted). */
static const char *tier_i18n_key(JceGpuTier t)
{
    switch (t) {
        case JCE_GPU_TIER_LOW:    return "statusBar.gpuTier.low";
        case JCE_GPU_TIER_MEDIUM: return "statusBar.gpuTier.mid";
        case JCE_GPU_TIER_HIGH:   return "statusBar.gpuTier.high";
        case JCE_GPU_TIER_ULTRA:  return "statusBar.gpuTier.ultra";
        default:                  return "statusBar.gpuTier.low";
    }
}

static ImVec4 tier_color(JceGpuTier t)
{
    switch (t) {
        case JCE_GPU_TIER_LOW:    return ImVec4(0.95f, 0.35f, 0.35f, 1.0f); /* red    */
        case JCE_GPU_TIER_MEDIUM: return ImVec4(0.95f, 0.80f, 0.25f, 1.0f); /* yellow */
        case JCE_GPU_TIER_HIGH:   return ImVec4(0.40f, 0.85f, 0.45f, 1.0f); /* green  */
        case JCE_GPU_TIER_ULTRA:  return ImVec4(0.45f, 0.85f, 0.95f, 1.0f); /* cyan   */
        default:                  return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
    }
}

/* Format a JceGraphicsApiVersion, or the i18n "not reported" string when the
 * backend did not tell us.  The engine sets *_verified false and leaves the
 * struct zeroed in that case, and a zeroed version must never be shown as
 * "0.0" -- that reads like a real answer. */
static void api_version_text(char *buf, size_t cap,
                             JceGraphicsApiVersion v, bool verified)
{
    if (!verified) {
        std::snprintf(buf, cap, "%s",
                      jce_editor_i18n("statusBar.gfxApi.unreported"));
        return;
    }
    if (v.patch)
        std::snprintf(buf, cap, "%u.%u.%u", (unsigned)v.major,
                      (unsigned)v.minor, (unsigned)v.patch);
    else
        std::snprintf(buf, cap, "%u.%u", (unsigned)v.major, (unsigned)v.minor);
}

/* Graphics-API section of the GPU-tier popup.
 *
 * The BUILD FLOOR and the NEGOTIATED version are two different things and
 * conflating them has already cost this tree a day: the desktop OpenGL floor
 * sat at 2.1 for the life of the project because bgfx's
 * BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION defaults to 1 when nothing sets it,
 * and nothing surfaced that -- a too-old floor renders fine, just on the old
 * paths.  This is where it becomes visible. */
static void draw_graphics_api_section(void)
{
    const JceRendererApiInfo info = jce_renderer_get_api_info();

    ImGui::TextUnformatted(jce_editor_i18n("statusBar.gfxApi.header"));
    ImGui::Separator();

    char minimum[32], runtime[32], shading[32];
    api_version_text(minimum, sizeof(minimum), info.minimum_version, true);
    api_version_text(runtime, sizeof(runtime), info.runtime_version,
                     info.runtime_version_verified);
    api_version_text(shading, sizeof(shading), info.shader_language_version,
                     info.shader_language_version_verified);

    ImGui::Text("%s  %s", jce_editor_i18n("statusBar.gfxApi.backend"),
                jce_renderer_backend_name(info.backend));
    ImGui::Text("%s  %s (%s)", jce_editor_i18n("statusBar.gfxApi.floor"),
                jce_graphics_api_tier_name(info.build_tier), minimum);
    ImGui::Text("%s  %s", jce_editor_i18n("statusBar.gfxApi.negotiated"),
                runtime);
    ImGui::Text("%s  %s", jce_editor_i18n("statusBar.gfxApi.shading"),
                shading);

    /* The whole ladder, so the floor above reads as one rung of a set rather
     * than an arbitrary number.  Rebuilding is what moves it, hence the tip. */
    char ladder[192];
    int  used = 0;
    for (int t = 0; t < JCE_GRAPHICS_API_TIER_COUNT; ++t) {
        const JceGraphicsApiTier    tier = (JceGraphicsApiTier)t;
        const JceGraphicsApiVersion v =
            jce_renderer_api_tier_minimum(info.backend, tier);
        if (!v.major) continue;   /* backend has no versioned floor policy */
        used += std::snprintf(ladder + used, sizeof(ladder) - (size_t)used,
                              "%s%s %u.%u", used ? "   " : "",
                              jce_graphics_api_tier_name(tier),
                              (unsigned)v.major, (unsigned)v.minor);
        if (used >= (int)sizeof(ladder)) break;
    }
    if (used > 0) {
        ImGui::Separator();
        ImGui::TextDisabled("%s", ladder);
        jce_editor::help_tip(jce_editor_i18n("statusBar.gfxApi.ladderTip"));
    }
}

static void draw_gpu_tier_segment(void)
{
    JceGpuTier  tier        = jce_renderer_get_tier();
    bool        overridden  = jce_renderer_tier_is_overridden();
    const char *label       = jce_editor_i18n("statusBar.gpuTier.label");
    const char *name        = jce_editor_i18n(tier_i18n_key(tier));
    const char *over_suffix = overridden
        ? jce_editor_i18n("statusBar.gpuTier.overridden")
        : "";

    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Text, tier_color(tier));
    /* Use a Selectable so the whole segment is clickable. */
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s%s%s##jce_gpu_tier",
                  name, overridden ? " " : "", over_suffix);
    ImVec2 sz = ImGui::CalcTextSize(buf);
    if (ImGui::Selectable(buf, false, ImGuiSelectableFlags_DontClosePopups,
                          ImVec2(sz.x, 0))) {
        ImGui::OpenPopup("##jce_gpu_tier_menu");
    }
    ImGui::PopStyleColor();

    jce_editor::help_tip(jce_editor_i18n("statusBar.gpuTier.tooltip"));

    if (ImGui::BeginPopup("##jce_gpu_tier_menu")) {
        draw_graphics_api_section();
        ImGui::Separator();
        bool tier_changed = false;
        if (ImGui::MenuItem(jce_editor_i18n("statusBar.gpuTier.menu.setLow")))
            { jce_renderer_set_tier_override(JCE_GPU_TIER_LOW);    tier_changed = true; }
        if (ImGui::MenuItem(jce_editor_i18n("statusBar.gpuTier.menu.setMid")))
            { jce_renderer_set_tier_override(JCE_GPU_TIER_MEDIUM); tier_changed = true; }
        if (ImGui::MenuItem(jce_editor_i18n("statusBar.gpuTier.menu.setHigh")))
            { jce_renderer_set_tier_override(JCE_GPU_TIER_HIGH);   tier_changed = true; }
        if (ImGui::MenuItem(jce_editor_i18n("statusBar.gpuTier.menu.setUltra")))
            { jce_renderer_set_tier_override(JCE_GPU_TIER_ULTRA);  tier_changed = true; }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("statusBar.gpuTier.menu.clearOverride"),
                            nullptr, false, overridden))
            { jce_renderer_clear_tier_override(); tier_changed = true; }
        if (tier_changed) {
            /* The override changes the EFFECTIVE tier, but the active render
             * pipeline (s_active) is only set once at engine boot — re-derive +
             * apply it now so the new tier's feature gates (toon / bloom / stylized
             * look + sky) actually take effect this session.  Without this re-apply
             * the menu changed the reported tier but nothing visible happened. */
            JceRenderPipelineDesc d;
            jce_render_pipeline_preset_for_current_tier(&d);
            jce_render_pipeline_apply(&d);
        }
        ImGui::EndPopup();
    }
}

void jce_editor_panel_status_bar(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_STATUS_BAR);
    if (!*vis) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y;

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x,
                                   vp->WorkPos.y + vp->WorkSize.y - h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    if (ImGui::Begin("##jce_status_bar", nullptr, flags)) {
        const float fps = ImGui::GetIO().Framerate;
        ImGui::Text("%s %5.1f", jce_editor_i18n("statusBar.fps"), fps);

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        const char *path = jce_state_get_current_scene_path();
        bool dirty = jce_state_is_scene_modified();
        ImGui::Text("%s %s%s", jce_editor_i18n("statusBar.scene"),
                    short_scene_name(path), dirty ? " *" : "");

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        int sel_count = 0;
        jce_state_get_selection(&sel_count);
        ImGui::Text("%s %d", jce_editor_i18n("statusBar.selected"), sel_count);

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        ImGui::Text("%s", play_state_label(jce_state_get_play_state()));

        ImGui::SameLine(); ImGui::TextUnformatted(" | ");
        ImGui::SameLine();
        draw_gpu_tier_segment();

        /* Right side: build/status placeholder. Hooks in once a build
         * subsystem reports progress globally. */
        const char *right = jce_editor_i18n("statusBar.ready");
        float right_w = ImGui::CalcTextSize(right).x;
        float avail   = ImGui::GetContentRegionAvail().x;
        if (avail > right_w + 8) {
            ImGui::SameLine(0, avail - right_w - 8);
            ImGui::TextUnformatted(right);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
