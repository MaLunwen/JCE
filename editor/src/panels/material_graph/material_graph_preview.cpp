/*
 * material_graph_preview.cpp — live PBR sphere preview pane + compile-
 * feedback log readout.  Phase D upgrade: replaces the stylised ImGui
 * draw-list disk with a real offscreen bgfx render using the same PBR
 * shader path as the runtime scene viewport, including support for
 * graph-generated programs bound by compile_and_bind().
 *
 * Rendering path:
 *   1. lazy-create JceOffscreenTarget @ JCE_VIEW_EDITOR_PREVIEW
 *   2. build fixed orbit camera (eye 0,0.6,3 → origin, fov 45°)
 *   3. offscreen_target_prepare(w, h, view, proj, clear)
 *   4. fill scratch JcePbrMaterial from s_prev (factors + custom_program)
 *   5. jce_render_preview_sphere(...) — engine helper does the actual draw
 *   6. ImGui::Image() the colour texture into the pane
 *
 * Falls back to the legacy ImGui circles if the renderer is unavailable
 * or the offscreen target cannot be created (e.g. very early frames
 * before scene render init).
 */
#include "panels/material_graph/material_graph_state.h"

#include "core/jce_editor_i18n.h"
#include "scene/jce_scene_render_internal.h"      /* s_sr.renderer */
#include "ui/jce_theme_palette.h"

extern "C" {
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_pbr_material.h>
#include <jce/renderer/jce_render_preview.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_views.h>
}

#include <cmath>
#include <cstdio>

namespace jce_mgp {

/* ------------------------------------------------------------------ */
/* File-static preview-target cache (process lifetime).                */
/* ------------------------------------------------------------------ */

static JceOffscreenTarget *s_preview_target = nullptr;
static uint32_t            s_preview_w      = 0;
static uint32_t            s_preview_h      = 0;

/* Orbit camera state — driven by mouse drag / wheel on the preview
 * image item. Defaults match the canonical material-preview pose. */
static float s_orbit_yaw   = 0.0f;     /* radians, around world Y    */
static float s_orbit_pitch = 0.20f;    /* radians, around side axis  */
static float s_orbit_dist  = 3.0f;     /* eye distance from origin   */

static void
preview_reset_camera(void)
{
    s_orbit_yaw   = 0.0f;
    s_orbit_pitch = 0.20f;
    s_orbit_dist  = 3.0f;
}

/* Legacy ImGui-disk fallback (used when renderer unavailable). */
static void
draw_preview_fallback(ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg(), 6.0f);

    ImVec2 c = ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    float  r = (size.x < size.y ? size.x : size.y) * 0.42f;

    float bc[4] = { s_prev.base_color[0], s_prev.base_color[1],
                    s_prev.base_color[2], s_prev.base_color[3] };
    float em[3] = { s_prev.emissive[0], s_prev.emissive[1], s_prev.emissive[2] };
    float met = s_prev.metallic;
    float rough = s_prev.roughness;
    if (!s_prev.valid) { bc[0]=bc[1]=bc[2]=0.7f; bc[3]=1.0f; met=0; rough=0.5f; }

    auto col = [](float r, float g, float b, float a) {
        auto C = [](float x){ x = x < 0 ? 0 : (x > 1 ? 1 : x);
                              return (int)(x * 255.0f + 0.5f); };
        return IM_COL32(C(r), C(g), C(b), C(a));
    };
    dl->AddCircleFilled(c, r, col(bc[0]*0.15f, bc[1]*0.15f, bc[2]*0.15f, 1.0f), 64);
    dl->AddCircleFilled(c, r * 0.94f,
        col(bc[0]*0.55f + em[0]*0.5f,
            bc[1]*0.55f + em[1]*0.5f,
            bc[2]*0.55f + em[2]*0.5f, 1.0f), 64);
    float hi_r = r * (0.35f - rough * 0.25f);
    if (hi_r < 4.0f) hi_r = 4.0f;
    ImVec2 hi = ImVec2(c.x - r * 0.35f, c.y - r * 0.35f);
    float spec = 0.7f + 0.3f * met;
    dl->AddCircleFilled(hi, hi_r,
        col(bc[0]*0.4f + spec, bc[1]*0.4f + spec, bc[2]*0.4f + spec, 1.0f), 32);

    char info[128];
    std::snprintf(info, sizeof(info), "M=%.2f  R=%.2f", met, rough);
    ImVec2 ts = ImGui::CalcTextSize(info);
    dl->AddText(ImVec2(p1.x - ts.x - 6.0f, p1.y - ts.y - 4.0f),
                jce_theme::text_secondary(), info);

    ImGui::Dummy(size);
}

void draw_preview_sphere(ImVec2 size)
{
    /* Renderer must be initialized — early-out to ImGui fallback. */
    JceRenderer *r = s_sr.renderer;
    if (!r || size.x < 16.0f || size.y < 16.0f) {
        draw_preview_fallback(size);
        return;
    }

    /* Lazy-create the offscreen target. The view-id stays the same
     * across resizes — only the underlying FBO is rebuilt by
     * jce_offscreen_target_prepare(). */
    if (!s_preview_target) {
        s_preview_target = jce_offscreen_target_create(r,
            JCE_VIEW_EDITOR_PREVIEW);
        if (!s_preview_target) {
            draw_preview_fallback(size);
            return;
        }
    }

    const uint32_t w = (uint32_t)size.x;
    const uint32_t h = (uint32_t)size.y;

    /* Orbit camera: derive eye from yaw/pitch/distance. */
    const float    cp     = cosf(s_orbit_pitch);
    const float    sp     = sinf(s_orbit_pitch);
    const float    cy     = cosf(s_orbit_yaw);
    const float    sy     = sinf(s_orbit_yaw);
    const jce_vec3 eye    = {
        s_orbit_dist *  cp * sy,
        s_orbit_dist *  sp,
        s_orbit_dist *  cp * cy,
    };
    const jce_vec3 target = { 0.0f, 0.0f, 0.0f };
    const jce_vec3 up     = { 0.0f, 1.0f, 0.0f };
    const float    fov_y  = 0.7853981633f;       /* 45° */
    const float    aspect = (float)w / (float)h;
    const jce_mat4 view   = jce_m4_look_at(eye, target, up);
    const jce_mat4 proj   = jce_m4_perspective(fov_y, aspect, 0.1f, 10.0f,
                                                s_sr.homogeneous_depth);

    /* Dark studio background — keeps spec highlights visible. */
    const uint32_t clear_rgba = 0x202428FFu;

    if (!jce_offscreen_target_prepare(s_preview_target, w, h,
                                       JCE_M4_PTR(view), JCE_M4_PTR(proj),
                                       clear_rgba,
                                       "MaterialGraphPreview")) {
        draw_preview_fallback(size);
        return;
    }
    s_preview_w = w;
    s_preview_h = h;

    /* Build scratch material from the resolved s_prev values, then
     * override the program with the graph-compiled handle (if any). */
    JcePbrMaterial mat = jce_pbr_material_default();
    if (s_prev.valid) {
        mat.base_color_factor[0] = s_prev.base_color[0];
        mat.base_color_factor[1] = s_prev.base_color[1];
        mat.base_color_factor[2] = s_prev.base_color[2];
        mat.base_color_factor[3] = s_prev.base_color[3];
        mat.metallic_factor      = s_prev.metallic;
        mat.roughness_factor     = s_prev.roughness;
        mat.emissive_factor[0]   = s_prev.emissive[0];
        mat.emissive_factor[1]   = s_prev.emissive[1];
        mat.emissive_factor[2]   = s_prev.emissive[2];
    }
    mat.custom_program = s_prev.custom_program.idx;

    const float eye_pos[3] = { eye.x, eye.y, eye.z };
    if (!jce_render_preview_sphere(r, JCE_VIEW_EDITOR_PREVIEW,
                                    eye_pos, &mat,
                                    s_prev.custom_program)) {
        draw_preview_fallback(size);
        return;
    }

    /* Compose into the panel. ImTextureID = bgfx texture handle index. */
    const uint16_t tex_idx =
        jce_offscreen_target_get_color_texture(s_preview_target);
    if (tex_idx == UINT16_MAX) {
        draw_preview_fallback(size);
        return;
    }
    ImGui::Image((ImTextureID)(uintptr_t)tex_idx, size);

    /* Mouse interaction: LMB-drag = orbit (yaw/pitch), wheel = dolly,
     * double-click = reset.  Pitch is clamped just shy of the poles to
     * keep the up-vector well-defined for the look-at matrix. */
    if (ImGui::IsItemHovered()) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            s_orbit_dist *= (wheel > 0.0f ? 0.9f : 1.111f);
            if (s_orbit_dist < 1.2f) s_orbit_dist = 1.2f;
            if (s_orbit_dist > 8.0f) s_orbit_dist = 8.0f;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            preview_reset_camera();
    }
    if (ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        const float  k = 0.008f;                /* radians per pixel  */
        s_orbit_yaw   -= d.x * k;
        s_orbit_pitch += d.y * k;
        const float lim = 1.4f;                 /* ≈ 80°              */
        if (s_orbit_pitch >  lim) s_orbit_pitch =  lim;
        if (s_orbit_pitch < -lim) s_orbit_pitch = -lim;
    }
}

void draw_preview_pane(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.title"));
    ImGui::Separator();

    /* Sphere */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  side = avail.x < 200.0f ? avail.x : 200.0f;
    if (side < 80.0f) side = 80.0f;
    draw_preview_sphere(ImVec2(side, side));

    ImGui::Spacing();

    /* Resolved values readout */
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.values"));
    if (!s_prev.valid) {
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.preview.notYetCompiled"));
    } else {
        ImGui::ColorButton("##bc", ImVec4(s_prev.base_color[0],
                                          s_prev.base_color[1],
                                          s_prev.base_color[2],
                                          s_prev.base_color[3]),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
        ImGui::SameLine(); ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.baseColor"));
        ImGui::Text(jce_editor_i18n("materialGraph.preview.metallicFmt"), s_prev.metallic);
        ImGui::Text(jce_editor_i18n("materialGraph.preview.roughnessFmt"), s_prev.roughness);
        ImGui::ColorButton("##em", ImVec4(s_prev.emissive[0],
                                          s_prev.emissive[1],
                                          s_prev.emissive[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
        ImGui::SameLine(); ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.emissive"));
        if (s_prev.base_tex[0]) ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.bcTexFmt"), s_prev.base_tex);
        if (s_prev.mr_tex[0])   ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.mrTexFmt"), s_prev.mr_tex);
        if (s_prev.emis_tex[0]) ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.emTexFmt"), s_prev.emis_tex);
    }

    ImGui::Spacing();
    ImGui::Separator();

    /* Compile feedback log */
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.compileLog"));
    /* Compile feedback log — tinted variants of the canvas bg so light
       and dark themes both render legibly. */
    ImU32 bg;
    if (s_log.has_error) {
        bg = jce_theme::is_light() ? IM_COL32(250, 220, 220, 255)
                                   : IM_COL32( 60,  20,  20, 255);
    } else {
        bg = jce_theme::is_light() ? IM_COL32(225, 240, 225, 255)
                                   : IM_COL32( 20,  30,  24, 255);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("##cmplog", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (s_log.text[0])
        ImGui::TextUnformatted(s_log.text);
    else
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.preview.logEmpty"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} /* namespace jce_mgp */
