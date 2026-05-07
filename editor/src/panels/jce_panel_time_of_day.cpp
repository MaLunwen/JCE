/*
 * jce_panel_time_of_day.cpp  Time-of-Day control panel.
 *
 * Drives the renderer's day/night override. Provides:
 *   - Hour-of-day slider (0..24) with auto-advance toggle
 *   - Latitude / dawn / dusk configuration
 *   - Live read-out of computed sun direction / colour / fog
 *   - "Apply" pushes the snapshot via jce_scene_renderer_set_time_of_day
 *
 * The override stays active as long as Auto-Apply is enabled or the user
 * clicks Apply each frame.
 */

#include "ui/jce_editor_panels.h"
#include "scene/jce_editor_scene_render.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>

extern "C" {
#include <jce/renderer/jce_time_of_day.h>
#include <jce/renderer/jce_scene_renderer.h>
}

namespace {

JceTimeOfDayConfig g_cfg = jce_time_of_day_default_config();
float              g_hour       = 12.0f;
bool               g_auto_apply = false;
bool               g_advance    = false;
float              g_advance_rate = 0.5f; /* hours per second */
double             g_last_t     = 0.0;

} /* namespace */

extern "C" void jce_editor_panel_time_of_day_content(void)
{
    double now = ImGui::GetTime();
    if (g_advance && g_last_t > 0.0) {
        g_hour += (float)((now - g_last_t)) * g_advance_rate;
        while (g_hour >= 24.0f) g_hour -= 24.0f;
        while (g_hour < 0.0f)   g_hour += 24.0f;
    }
    g_last_t = now;

    ImGui::SliderFloat(jce_editor_i18n("timeOfDay.hourOfDay"), &g_hour, 0.0f, 24.0f, "%.2f h");
    ImGui::Checkbox(jce_editor_i18n("timeOfDay.autoAdvance"), &g_advance);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat(jce_editor_i18n("timeOfDay.rate"), &g_advance_rate, 0.05f, 0.0f, 24.0f, "%.2f");

    ImGui::Separator();
    if (ImGui::CollapsingHeader(jce_editor_i18n("timeOfDay.configuration"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.dawnHour"),  &g_cfg.dawn_hour,        0.05f, 0.0f, 24.0f, "%.2f");
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.duskHour"),  &g_cfg.dusk_hour,        0.05f, 0.0f, 24.0f, "%.2f");
        ImGui::DragFloat(jce_editor_i18n("timeOfDay.latitude"),  &g_cfg.latitude_degrees, 0.5f, -90.0f, 90.0f, "%.1f");
    }

    JceTimeOfDayState st;
    jce_time_of_day_evaluate(&g_cfg, g_hour, &st);

    ImGui::Separator();
    ImGui::Text("%s   : %+.2f %+.2f %+.2f", jce_editor_i18n("timeOfDay.sunDir"),
                st.sun_direction.x, st.sun_direction.y, st.sun_direction.z);
    ImGui::Text("%s : %.2f %.2f %.2f  (%s %.2f)", jce_editor_i18n("timeOfDay.sunColor"),
                st.sun_color.x, st.sun_color.y, st.sun_color.z,
                jce_editor_i18n("timeOfDay.intensity"), st.sun_intensity);
    ImGui::Text("%s   : %.2f %.2f %.2f", jce_editor_i18n("timeOfDay.ambient"),
                st.ambient_color.x, st.ambient_color.y, st.ambient_color.z);
    ImGui::Text("%s       : %.2f %.2f %.2f  %s %.4f", jce_editor_i18n("timeOfDay.fog"),
                st.fog_color.x, st.fog_color.y, st.fog_color.z,
                jce_editor_i18n("timeOfDay.density"), st.fog_density);
    ImGui::Text("%s  : %.2f%s", jce_editor_i18n("timeOfDay.exposure"),
                st.exposure, st.is_night ? jce_editor_i18n("timeOfDay.night") : "");

    ImGui::Separator();
    ImGui::Checkbox(jce_editor_i18n("timeOfDay.autoApplyFrame"), &g_auto_apply);
    ImGui::SameLine();
    bool apply = ImGui::Button(jce_editor_i18n("timeOfDay.applyNow"));

    if (apply || g_auto_apply) {
        JceSceneRenderer *sr = jce_editor_get_scene_renderer();
        if (sr) jce_scene_renderer_set_time_of_day(sr, &st);
    }
}
