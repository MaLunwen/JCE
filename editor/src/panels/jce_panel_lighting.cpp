/*
 * jce_panel_lighting.cpp  Lighting settings window (Sprint 2 / 0.8.16)
 *
 * Mirrors Unity's Window > Rendering > Lighting. Surfaces scene-wide
 * lighting / environment / fog parameters in one place rather than
 * forcing the user to hunt for individual light entities or open the
 * Skybox component on the right entity.
 *
 * Behavior:
 *   - Environment section: lists Skybox component if any (allows
 *     editing hdr_path / rotation / exposure / use_as_ibl), and exposes
 *     a global ambient color + intensity stored on the editor side.
 *   - Lights section: lists every Directional / Point / Spot light in
 *     the scene with quick-edit color, intensity, ping-to-select.
 *   - Fog section: editor-side density / color / height-falloff sliders
 *     intended to drive the volumetric fog pipeline once the scene
 *     renderer exposes a setter; today they only persist within the
 *     editor session.
 *
 * The panel deliberately avoids touching engine APIs that don't yet
 * exist; missing wiring is shown as a TextDisabled "Not yet wired"
 * note so the user understands the surface vs. the runtime state.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_volumetric_fog.h>
}

/* ── Editor-side persistent settings (per session). ─────────────────── */

static struct {
    /* Environment */
    float ambient_color[3] = { 0.1f, 0.1f, 0.12f };
    float ambient_intensity = 1.0f;
    /* Fog */
    bool  fog_enabled = false;
    float fog_color[3] = { 0.7f, 0.75f, 0.85f };
    float fog_density = 0.02f;
    float fog_height_falloff = 0.05f;
    float fog_height_origin = 0.0f;
} s_light;

/* ── Helpers ────────────────────────────────────────────────────────── */

struct LightCollect {
    uint32_t dir[16];   int n_dir;
    uint32_t point[64]; int n_point;
    uint32_t spot[64];  int n_spot;
    uint32_t skybox;    bool has_skybox;
};

static void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    LightCollect *c = (LightCollect *)ud;
    if (jce_scene_has_dir_light(s, e) && c->n_dir < 16)
        c->dir[c->n_dir++] = (uint32_t)e;
    if (jce_scene_has_point_light(s, e) && c->n_point < 64)
        c->point[c->n_point++] = (uint32_t)e;
    if (jce_scene_has_spot_light(s, e) && c->n_spot < 64)
        c->spot[c->n_spot++] = (uint32_t)e;
    if (!c->has_skybox && jce_scene_has_skybox(s, e)) {
        c->has_skybox = true;
        c->skybox = (uint32_t)e;
    }
}

static void ping_entity(uint32_t id)
{
    jce_state_select_entity(id, false);
}

/* ── Section drawers ────────────────────────────────────────────────── */

static void draw_env_section(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("lighting.section.env"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::TextDisabled("%s", jce_editor_i18n("lighting.skybox"));
    if (c.has_skybox) {
        JceSkyboxComponent *sky = jce_scene_get_skybox(scene, (JceEntity)c.skybox);
        if (sky) {
            const char *name = jce_scene_entity_name(scene, (JceEntity)c.skybox);
            ImGui::PushID((int)c.skybox);
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.skybox"));
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("lighting.ping")))
                ping_entity(c.skybox);
            ImGui::InputText(jce_editor_i18n("skybox.hdrPath"),
                             sky->hdr_path, sizeof(sky->hdr_path));
            ImGui::DragFloat(jce_editor_i18n("skybox.rotation"),
                             &sky->rotation, 1.0f, 0.0f, 360.0f, "%.1f deg");
            ImGui::DragFloat(jce_editor_i18n("skybox.exposure"),
                             &sky->exposure, 0.01f, 0.0f, 8.0f);
            ImGui::Checkbox(jce_editor_i18n("skybox.useAsIbl"),
                            &sky->use_as_ibl);
            ImGui::PopID();
        }
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("lighting.noSkybox"));
    }

    ImGui::Spacing();
    ImGui::TextDisabled("%s", jce_editor_i18n("lighting.ambient"));
    ImGui::ColorEdit3(jce_editor_i18n("lighting.ambientColor"),
                      s_light.ambient_color);
    ImGui::DragFloat(jce_editor_i18n("lighting.ambientIntensity"),
                     &s_light.ambient_intensity, 0.01f, 0.0f, 4.0f);
}

static void draw_lights_section(JceScene *scene, const LightCollect &c)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("lighting.section.lights"),
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    char lbl[160];
    /* Directional */
    snprintf(lbl, sizeof(lbl), "%s (%d)",
             jce_editor_i18n("lighting.directional"), c.n_dir);
    if (ImGui::TreeNodeEx(lbl, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < c.n_dir; i++) {
            uint32_t id = c.dir[i];
            ImGui::PushID((int)id);
            JceDirectionalLight *l = jce_scene_get_dir_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("→")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_dir == 0)
            ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }

    /* Point */
    snprintf(lbl, sizeof(lbl), "%s (%d)",
             jce_editor_i18n("lighting.point"), c.n_point);
    if (ImGui::TreeNodeEx(lbl)) {
        for (int i = 0; i < c.n_point; i++) {
            uint32_t id = c.point[i];
            ImGui::PushID((int)id + 0x10000);
            JcePointLight *l = jce_scene_get_point_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("→")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_point == 0)
            ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }

    /* Spot */
    snprintf(lbl, sizeof(lbl), "%s (%d)",
             jce_editor_i18n("lighting.spot"), c.n_spot);
    if (ImGui::TreeNodeEx(lbl)) {
        for (int i = 0; i < c.n_spot; i++) {
            uint32_t id = c.spot[i];
            ImGui::PushID((int)id + 0x20000);
            JceSpotLight *l = jce_scene_get_spot_light(scene, (JceEntity)id);
            const char *name = jce_scene_entity_name(scene, (JceEntity)id);
            if (ImGui::SmallButton("→")) ping_entity(id);
            ImGui::SameLine();
            ImGui::Text("%s", name ? name : jce_editor_i18n("lighting.fallback.light"));
            if (l) {
                float col[3] = { l->color.x, l->color.y, l->color.z };
                if (ImGui::ColorEdit3("##col", col)) {
                    l->color.x = col[0]; l->color.y = col[1]; l->color.z = col[2];
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                ImGui::DragFloat("##i", &l->intensity, 0.05f, 0.0f, 100.0f);
            }
            ImGui::PopID();
        }
        if (c.n_spot == 0)
            ImGui::TextDisabled("(none)");
        ImGui::TreePop();
    }
}

static void draw_fog_section(void)
{
    if (!ImGui::CollapsingHeader(jce_editor_i18n("lighting.section.fog")))
        return;
    ImGui::Checkbox(jce_editor_i18n("lighting.fog.enabled"), &s_light.fog_enabled);
    ImGui::BeginDisabled(!s_light.fog_enabled);
    ImGui::ColorEdit3(jce_editor_i18n("lighting.fog.color"), s_light.fog_color);
    ImGui::DragFloat(jce_editor_i18n("lighting.fog.density"),
                     &s_light.fog_density, 0.001f, 0.0f, 1.0f, "%.4f");
    ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightFalloff"),
                     &s_light.fog_height_falloff, 0.001f, 0.0f, 1.0f, "%.4f");
    ImGui::DragFloat(jce_editor_i18n("lighting.fog.heightOrigin"),
                     &s_light.fog_height_origin, 0.1f);
    ImGui::EndDisabled();
    ImGui::TextDisabled("(%s)", jce_editor_i18n("lighting.fog.note"));
}

/* ── Public entry points ────────────────────────────────────────────── */

extern "C" void jce_editor_panel_lighting_content(void)
{
    JceScene *scene = jce_state_get_scene();
    LightCollect c{};
    if (scene) jce_scene_each_entity(scene, collect_cb, &c);

    draw_env_section(scene, c);
    draw_lights_section(scene, c);
    draw_fog_section();
}

extern "C" void jce_editor_panel_lighting(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING);
    if (!vis || !*vis) return;
    char lbl[128];
    snprintf(lbl, sizeof(lbl), "%s###lighting", jce_editor_i18n("lighting.title"));
    if (ImGui::Begin(lbl, vis))
        jce_editor_panel_lighting_content();
    ImGui::End();
}

/* ── Renderer-facing accessors (consumed by jce_editor_scene_render) ── */

extern "C" bool jce_editor_lighting_get_fog_enabled(void)
{
    return s_light.fog_enabled;
}

extern "C" void jce_editor_lighting_get_fog_params(JceVolumetricFogParams *out)
{
    if (!out) return;
    *out = jce_volumetric_fog_default_params();
    out->color_r        = s_light.fog_color[0];
    out->color_g        = s_light.fog_color[1];
    out->color_b        = s_light.fog_color[2];
    out->density        = s_light.fog_density;
    out->height_falloff = s_light.fog_height_falloff;
    out->height_origin  = s_light.fog_height_origin;
}

extern "C" void jce_editor_lighting_get_ambient(float out_color_rgb[3], float *out_intensity)
{
    if (out_color_rgb) {
        out_color_rgb[0] = s_light.ambient_color[0];
        out_color_rgb[1] = s_light.ambient_color[1];
        out_color_rgb[2] = s_light.ambient_color[2];
    }
    if (out_intensity) *out_intensity = s_light.ambient_intensity;
}
