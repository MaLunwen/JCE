/*
 * jce_panel_particle_editor.cpp  Authoring panel for JceParticleEmitterDesc.
 *
 * Phase A (no 3D preview yet):
 *   - Edits a single in-memory JceParticleEmitterDesc via reflection.
 *   - Hosts a private JceParticleSystem instance with one emitter so the
 *     user can hit Play / Pause / Reset and see live alive-particle count
 *     plus a rolling timeline (3D viewport hookup is a follow-up).
 *   - Save / Load as `*.particles.json` for hand-off to runtime callers.
 *
 * Reflection schema for JceParticleEmitterDesc is registered on first use
 * so we do not need to touch jce_reflect_builtin.cpp for an additive
 * panel.  Once it is consumed elsewhere it can move there.
 */

#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_editor_config.h"
#include "core/jce_reflect.h"

#include <jce/tools/jce_imgui.hpp>
extern "C" {
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_particles.h>
}

#include "io/jce_editor_file_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* ── Reflection registration (file-scope so static init order is fine) ─ */

JCE_REFLECT_BEGIN(JceParticleEmitterDesc, "Particle Emitter")
    JCE_FIELD_RANGE(JceParticleEmitterDesc, max_particles, JCE_FT_INT,   "Max Particles", 1.0f, 65536.0f, 1.0f)
    JCE_FIELD_RANGE(JceParticleEmitterDesc, emit_rate,     JCE_FT_FLOAT, "Emit Rate (/s)", 0.0f, 10000.0f, 1.0f)
    JCE_FIELD_RANGE(JceParticleEmitterDesc, emit_burst,    JCE_FT_FLOAT, "Burst Count",    0.0f, 10000.0f, 1.0f)
    JCE_FIELD_RANGE(JceParticleEmitterDesc, lifetime_min,  JCE_FT_FLOAT, "Lifetime Min",   0.01f, 60.0f, 0.05f)
    JCE_FIELD_RANGE(JceParticleEmitterDesc, lifetime_max,  JCE_FT_FLOAT, "Lifetime Max",   0.01f, 60.0f, 0.05f)
    JCE_FIELD(JceParticleEmitterDesc, velocity_min, JCE_FT_VEC3, "Velocity Min")
    JCE_FIELD(JceParticleEmitterDesc, velocity_max, JCE_FT_VEC3, "Velocity Max")
    JCE_FIELD(JceParticleEmitterDesc, gravity,      JCE_FT_VEC3, "Gravity")
    JCE_FIELD_RANGE(JceParticleEmitterDesc, size_start, JCE_FT_FLOAT, "Size Start", 0.0f, 100.0f, 0.01f)
    JCE_FIELD_RANGE(JceParticleEmitterDesc, size_end,   JCE_FT_FLOAT, "Size End",   0.0f, 100.0f, 0.01f)
    JCE_FIELD(JceParticleEmitterDesc, color_start,  JCE_FT_COLOR4, "Color Start")
    JCE_FIELD(JceParticleEmitterDesc, color_end,    JCE_FT_COLOR4, "Color End")
    JCE_FIELD(JceParticleEmitterDesc, world_space,  JCE_FT_BOOL,   "World Space")
JCE_REFLECT_END(JceParticleEmitterDesc, "Particle Emitter")

namespace {

struct ParticleEditorState {
    bool registered = false;
    JceParticleEmitterDesc desc{};
    JceParticleSystem    *sys      = nullptr;
    JceEmitterHandle      emitter  = { UINT32_MAX };
    bool                  playing  = false;
    char                  path[260] = {0};
    char                  texture_path[260] = {0};
    /* 3D orbital preview state. */
    float                 spawn_accum = 0.0f;
    float                 cam_yaw     = 0.6f;
    float                 cam_pitch   = 0.4f;
    float                 cam_dist    = 8.0f;
    bool                  show_grid   = true;
    bool                  show_axes   = true;
    bool                  cam_drag    = false;
    /* Phase D: presets, scrubbing, stats */
    float                 time_scale  = 1.0f;
    bool                  step_once   = false;
    int                   alive_peak  = 0;
    float                 bg_color[4] = { 0.784f, 0.863f, 0.643f, 1.0f };  /* #C8DCA4 — light sage, theme-neutral */
    int                   preset_idx  = 0;
};

/* Internal parallel CPU "shadow" simulation. Mirrors emitter desc to
 * give a 2D top-down preview without touching engine particle state. */
struct PreviewParticle {
    float x, y, z;
    float vx, vy, vz;
    float age, life;
    float size_start, size_end;
    float cs[4], ce[4];
};
std::vector<PreviewParticle> s_preview;

float frand(float a, float b)
{
    float u = (float)std::rand() / (float)RAND_MAX;
    return a + (b - a) * u;
}

ParticleEditorState s_pe;

void desc_defaults(JceParticleEmitterDesc *d)
{
    std::memset(d, 0, sizeof(*d));
    d->max_particles = 1024;
    d->emit_rate     = 50.0f;
    d->lifetime_min  = 1.0f;
    d->lifetime_max  = 2.0f;
    d->velocity_min.x = -0.5f; d->velocity_min.y = 1.0f; d->velocity_min.z = -0.5f;
    d->velocity_max.x =  0.5f; d->velocity_max.y = 2.0f; d->velocity_max.z =  0.5f;
    d->gravity.x = 0.0f; d->gravity.y = -1.0f; d->gravity.z = 0.0f;
    d->size_start    = 0.1f;
    d->size_end      = 0.0f;
    d->color_start.x = 1.0f; d->color_start.y = 1.0f; d->color_start.z = 1.0f; d->color_start.w = 1.0f;
    d->color_end.x   = 1.0f; d->color_end.y   = 1.0f; d->color_end.z   = 1.0f; d->color_end.w   = 0.0f;
}

/* ── Phase D presets ───────────────────────────────────────────────── */

const char *kPresetNames[] = {
    "Default", "Smoke", "Fire", "Sparks", "Snow", "Magic"
};
constexpr int kPresetCount = (int)(sizeof(kPresetNames)/sizeof(kPresetNames[0]));

void apply_preset(JceParticleEmitterDesc *d, int idx)
{
    desc_defaults(d);
    switch (idx) {
        case 1: /* Smoke */
            d->max_particles = 512; d->emit_rate = 30.0f;
            d->lifetime_min  = 2.5f; d->lifetime_max = 4.0f;
            d->velocity_min  = { -0.2f, 0.6f, -0.2f };
            d->velocity_max  = {  0.2f, 1.2f,  0.2f };
            d->gravity       = { 0.0f, 0.05f, 0.0f };
            d->size_start    = 0.3f; d->size_end = 1.4f;
            d->color_start.x = 0.6f; d->color_start.y = 0.6f; d->color_start.z = 0.6f; d->color_start.w = 0.7f;
            d->color_end.x   = 0.2f; d->color_end.y   = 0.2f; d->color_end.z   = 0.2f; d->color_end.w   = 0.0f;
            break;
        case 2: /* Fire */
            d->max_particles = 1024; d->emit_rate = 120.0f;
            d->lifetime_min  = 0.6f; d->lifetime_max = 1.2f;
            d->velocity_min  = { -0.3f, 1.5f, -0.3f };
            d->velocity_max  = {  0.3f, 3.0f,  0.3f };
            d->gravity       = { 0.0f, 1.5f, 0.0f };
            d->size_start    = 0.5f; d->size_end = 0.05f;
            d->color_start.x = 1.0f; d->color_start.y = 0.85f; d->color_start.z = 0.25f; d->color_start.w = 1.0f;
            d->color_end.x   = 0.6f; d->color_end.y   = 0.05f; d->color_end.z   = 0.0f;  d->color_end.w   = 0.0f;
            break;
        case 3: /* Sparks */
            d->max_particles = 256; d->emit_rate = 80.0f;
            d->lifetime_min  = 0.3f; d->lifetime_max = 0.9f;
            d->velocity_min  = { -3.0f, 2.0f, -3.0f };
            d->velocity_max  = {  3.0f, 5.0f,  3.0f };
            d->gravity       = { 0.0f, -9.8f, 0.0f };
            d->size_start    = 0.06f; d->size_end = 0.0f;
            d->color_start.x = 1.0f; d->color_start.y = 1.0f; d->color_start.z = 0.6f; d->color_start.w = 1.0f;
            d->color_end.x   = 1.0f; d->color_end.y   = 0.4f; d->color_end.z   = 0.0f; d->color_end.w   = 0.0f;
            break;
        case 4: /* Snow */
            d->max_particles = 2048; d->emit_rate = 60.0f;
            d->lifetime_min  = 6.0f; d->lifetime_max = 10.0f;
            d->velocity_min  = { -0.2f, -0.6f, -0.2f };
            d->velocity_max  = {  0.2f, -0.3f,  0.2f };
            d->gravity       = { 0.0f, -0.05f, 0.0f };
            d->size_start    = 0.08f; d->size_end = 0.08f;
            d->color_start.x = 0.95f; d->color_start.y = 0.97f; d->color_start.z = 1.0f; d->color_start.w = 0.95f;
            d->color_end.x   = 0.95f; d->color_end.y   = 0.97f; d->color_end.z   = 1.0f; d->color_end.w   = 0.0f;
            break;
        case 5: /* Magic */
            d->max_particles = 768; d->emit_rate = 90.0f;
            d->lifetime_min  = 1.5f; d->lifetime_max = 2.4f;
            d->velocity_min  = { -1.2f, 0.6f, -1.2f };
            d->velocity_max  = {  1.2f, 1.8f,  1.2f };
            d->gravity       = { 0.0f, 0.4f, 0.0f };
            d->size_start    = 0.18f; d->size_end = 0.0f;
            d->color_start.x = 0.7f; d->color_start.y = 0.4f; d->color_start.z = 1.0f; d->color_start.w = 1.0f;
            d->color_end.x   = 0.2f; d->color_end.y   = 0.9f; d->color_end.z   = 1.0f; d->color_end.w   = 0.0f;
            break;
        default: break;
    }
}

void ensure_init(void)
{
    if (!s_pe.registered) {
        jce_reflect_register(&g_jce_type_JceParticleEmitterDesc);
        desc_defaults(&s_pe.desc);
        s_pe.registered = true;
    }
}

void rebuild_preview(void)
{
    if (s_pe.sys && jce_emitter_valid(s_pe.emitter))
        jce_particles_emitter_remove(s_pe.sys, s_pe.emitter);
    if (!s_pe.sys)
        s_pe.sys = jce_particles_create(jce_allocator_default());
    if (!s_pe.sys) return;
    s_pe.emitter = jce_particles_emitter_add(s_pe.sys, &s_pe.desc);
    if (s_pe.playing && jce_emitter_valid(s_pe.emitter))
        jce_particles_emitter_start(s_pe.sys, s_pe.emitter);
    else if (jce_emitter_valid(s_pe.emitter))
        jce_particles_emitter_stop(s_pe.sys, s_pe.emitter);
}

bool save_to_json(const char *path, const JceParticleEmitterDesc *d)
{
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "maxParticles", (double)d->max_particles);
    jce_json_set_number(root, "emitRate",     d->emit_rate);
    jce_json_set_number(root, "emitBurst",    d->emit_burst);
    jce_json_set_number(root, "lifetimeMin",  d->lifetime_min);
    jce_json_set_number(root, "lifetimeMax",  d->lifetime_max);
    jce_json_set_float_array(root, "velocityMin", &d->velocity_min.x, 3);
    jce_json_set_float_array(root, "velocityMax", &d->velocity_max.x, 3);
    jce_json_set_float_array(root, "gravity",     &d->gravity.x,      3);
    jce_json_set_number(root, "sizeStart",    d->size_start);
    jce_json_set_number(root, "sizeEnd",      d->size_end);
    jce_json_set_float_array(root, "colorStart",  &d->color_start.x,  4);
    jce_json_set_float_array(root, "colorEnd",    &d->color_end.x,    4);
    jce_json_set_bool(root, "worldSpace", d->world_space);
    jce_json_set_string(root, "texture", s_pe.texture_path);
    return ed_write_json_to_file(path, root);
}

bool load_from_json(const char *path, JceParticleEmitterDesc *d)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) return false;
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return false;
    desc_defaults(d);
    d->max_particles = (uint32_t)jce_json_get_int(root,    "maxParticles", (int)d->max_particles);
    d->emit_rate     = (float)   jce_json_get_number(root, "emitRate",     d->emit_rate);
    d->emit_burst    = (float)   jce_json_get_number(root, "emitBurst",    d->emit_burst);
    d->lifetime_min  = (float)   jce_json_get_number(root, "lifetimeMin",  d->lifetime_min);
    d->lifetime_max  = (float)   jce_json_get_number(root, "lifetimeMax",  d->lifetime_max);
    d->size_start    = (float)   jce_json_get_number(root, "sizeStart",    d->size_start);
    d->size_end      = (float)   jce_json_get_number(root, "sizeEnd",      d->size_end);
    d->world_space   = jce_json_get_bool(root, "worldSpace", d->world_space);
    jce_json_get_floats(root, "velocityMin", &d->velocity_min.x, 3, &d->velocity_min.x);
    jce_json_get_floats(root, "velocityMax", &d->velocity_max.x, 3, &d->velocity_max.x);
    jce_json_get_floats(root, "gravity",     &d->gravity.x,      3, &d->gravity.x);
    jce_json_get_floats(root, "colorStart",  &d->color_start.x,  4, &d->color_start.x);
    jce_json_get_floats(root, "colorEnd",    &d->color_end.x,    4, &d->color_end.x);
    const char *tex = jce_json_get_string(root, "texture", "");
    std::snprintf(s_pe.texture_path, sizeof(s_pe.texture_path), "%s", tex ? tex : "");
    jce_json_free(root);
    return true;
}

/* ── 3D preview helpers ─────────────────────────────────────────────── */

struct V3 { float x, y, z; };

V3 cam_to_view(V3 p, float yaw, float pitch)
{
    /* Rotate world by -yaw around Y, then -pitch around X. */
    float cy = std::cos(-yaw),   sy = std::sin(-yaw);
    float cp = std::cos(-pitch), sp = std::sin(-pitch);
    V3 a = { cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z };
    V3 b = { a.x, cp * a.y - sp * a.z, sp * a.y + cp * a.z };
    return b;
}

bool project(V3 view, V3 cam, ImVec2 c0, ImVec2 sz, float fov_y,
             ImVec2 *out, float *out_depth, float *out_size_scale)
{
    /* view-space relative to camera (camera at (0,0,cam_dist) looking -Z). */
    float vz = view.z - cam.z;
    if (vz >= -0.05f) return false;
    float zn = -vz;
    float f  = 1.0f / std::tan(fov_y * 0.5f);
    float aspect = sz.x / sz.y;
    float ndc_x = (view.x / zn) * f / aspect;
    float ndc_y = (view.y / zn) * f;
    out->x = c0.x + (ndc_x * 0.5f + 0.5f) * sz.x;
    out->y = c0.y + (1.0f - (ndc_y * 0.5f + 0.5f)) * sz.y;
    *out_depth      = zn;
    *out_size_scale = (sz.y * 0.5f) * f / zn;
    return true;
}

void draw_content(void)
{
    ensure_init();

    /* Toolbar. */
    if (ImGui::Button(s_pe.playing ? jce_editor_i18n("particleEditor.button.pause") : jce_editor_i18n("particleEditor.button.play"))) {
        s_pe.playing = !s_pe.playing;
        if (!s_pe.sys) rebuild_preview();
        if (jce_emitter_valid(s_pe.emitter)) {
            if (s_pe.playing) jce_particles_emitter_start(s_pe.sys, s_pe.emitter);
            else              jce_particles_emitter_stop (s_pe.sys, s_pe.emitter);
        }
    }
    ImGui::SameLine();
    if (!s_pe.playing) {
        if (ImGui::Button(jce_editor_i18n("particleEditor.button.step"))) s_pe.step_once = true;
    } else {
        ImGui::BeginDisabled(); ImGui::Button(jce_editor_i18n("particleEditor.button.step")); ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.reset"))) {
        s_pe.playing = false;
        s_pe.alive_peak = 0;
        rebuild_preview();
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.applyChanges"))) {
        rebuild_preview();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat(jce_editor_i18n("particleEditor.field.speed"), &s_pe.time_scale, 0.0f, 3.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const char *preset_keys[] = {
        "particleEditor.preset.default",
        "particleEditor.preset.smoke",
        "particleEditor.preset.fire",
        "particleEditor.preset.sparks",
        "particleEditor.preset.snow",
        "particleEditor.preset.magic",
    };
    const char *preset_labels[kPresetCount];
    for (int i = 0; i < kPresetCount; ++i)
        preset_labels[i] = jce_editor_i18n_or(preset_keys[i], kPresetNames[i]);
    if (ImGui::Combo(jce_editor_i18n_id("particleEditor.field.preset", "pe_preset"), &s_pe.preset_idx, preset_labels, kPresetCount)) {
        /* combo selection only — explicit Apply below */
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.applyPreset"))) {
        apply_preset(&s_pe.desc, s_pe.preset_idx);
        s_pe.alive_peak = 0;
        rebuild_preview();
        jce_editor_console_log("particle preset applied: %s",
                               kPresetNames[s_pe.preset_idx]);
    }

    ImGui::Separator();

    /* Path + Save / Load. */
    ImGui::InputText(jce_editor_i18n("particleEditor.field.file"), s_pe.path, sizeof(s_pe.path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.save")) && s_pe.path[0]) {
        if (save_to_json(s_pe.path, &s_pe.desc))
            jce_editor_console_log("particle preset saved: %s", s_pe.path);
        else
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "particle preset save failed: %s", s_pe.path);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.load")) && s_pe.path[0]) {
        if (load_from_json(s_pe.path, &s_pe.desc)) {
            jce_editor_console_log("particle preset loaded: %s", s_pe.path);
            rebuild_preview();
        } else {
            jce_editor_console_log_level(JCE_CONSOLE_ERROR,
                "particle preset load failed: %s", s_pe.path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("particleEditor.button.defaults"))) {
        desc_defaults(&s_pe.desc);
        rebuild_preview();
    }

    ImGui::Separator();

    /* Reflection-driven property grid. */
    const JceReflectType *t = jce_reflect_find("Particle Emitter");
    if (t) jce_reflect_draw(t, &s_pe.desc);

    /* Texture path (Phase B Authoring; binding to handle is TODO until
     * editor exposes a string->JceTextureHandle loader). */
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("particleEditor.section.texture"));
    ImGui::InputText("##texpath", s_pe.texture_path, sizeof(s_pe.texture_path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n_id("particleEditor.button.clear", "tex"))) s_pe.texture_path[0] = 0;
    ImGui::TextDisabled("%s", jce_editor_i18n("particleEditor.hint.texturePath"));

    /* 3D Preview canvas (orbital camera). */
    ImGui::Separator();
    ImGui::TextUnformatted(jce_editor_i18n("particleEditor.section.preview3d"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat(jce_editor_i18n_id("particleEditor.field.dist", "pe"), &s_pe.cam_dist, 1.0f, 60.0f, "%.1f m");
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("particleEditor.field.grid", "pe"), &s_pe.show_grid);
    ImGui::SameLine();
    ImGui::Checkbox(jce_editor_i18n_id("particleEditor.field.axes", "pe"), &s_pe.show_axes);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    ImGui::ColorEdit4(jce_editor_i18n_id("particleEditor.field.bg", "pe"), s_pe.bg_color, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("particleEditor.hint.orbit"));

    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 csz = ImGui::GetContentRegionAvail();
    if (csz.y < 240.0f) csz.y = 240.0f;
    if (csz.x < 240.0f) csz.x = 240.0f;
    ImVec2 cp1 = ImVec2(cp0.x + csz.x, cp0.y + csz.y);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 bgu = IM_COL32((int)(s_pe.bg_color[0]*255),
                         (int)(s_pe.bg_color[1]*255),
                         (int)(s_pe.bg_color[2]*255), 255);
    dl->AddRectFilled(cp0, cp1, bgu);

    ImGui::InvisibleButton("##pe3d", csz,
                           ImGuiButtonFlags_MouseButtonLeft);
    bool hov = ImGui::IsItemHovered();
    if (hov && ImGui::IsMouseClicked(0))         s_pe.cam_drag = true;
    if (!ImGui::IsMouseDown(0))                  s_pe.cam_drag = false;
    if (s_pe.cam_drag) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        /* Defaults: drag right -> camera orbits right (so the world
           appears to rotate left), drag down -> camera tilts down.
           User pref inverts both axes together. */
        float dx_sign = jce_editor_pref_invert_drag_y ?  1.0f : -1.0f;
        float dy_sign = jce_editor_pref_invert_drag_y ?  1.0f : -1.0f;
        s_pe.cam_yaw   += d.x * 0.01f * dx_sign;
        s_pe.cam_pitch += d.y * 0.01f * dy_sign;
        if (s_pe.cam_pitch >  1.4f) s_pe.cam_pitch =  1.4f;
        if (s_pe.cam_pitch < -1.4f) s_pe.cam_pitch = -1.4f;
    }
    if (hov) {
        float w = ImGui::GetIO().MouseWheel;
        /* Default: wheel up -> zoom in. f=0.85 brings the camera closer,
           f=1.18 pushes it away. MouseWheel is +ve on wheel-up, so no
           flip in the default branch. Pref opts into natural-scroll. */
        if (jce_editor_pref_invert_scroll_zoom) w = -w;
        if (w != 0.0f) {
            float f = (w > 0.0f) ? 0.85f : 1.18f;
            s_pe.cam_dist *= f;
            if (s_pe.cam_dist < 1.0f)  s_pe.cam_dist = 1.0f;
            if (s_pe.cam_dist > 60.0f) s_pe.cam_dist = 60.0f;
        }
    }

    const float fov_y = 1.0472f;   /* 60 deg */
    V3 cam_pos = { 0.0f, 0.0f, s_pe.cam_dist };

    /* Step preview sim. */
    bool sim_active = s_pe.playing || s_pe.step_once;
    if (sim_active) {
        float dt = s_pe.step_once ? (1.0f / 60.0f)
                                  : (ImGui::GetIO().DeltaTime * s_pe.time_scale);
        if (dt > 0.1f) dt = 0.1f;
        s_pe.spawn_accum += s_pe.desc.emit_rate * dt;
        int to_spawn = (int)s_pe.spawn_accum;
        s_pe.spawn_accum -= (float)to_spawn;
        for (int i = 0; i < to_spawn; ++i) {
            if ((int)s_preview.size() >= (int)s_pe.desc.max_particles) break;
            PreviewParticle p;
            p.x = p.y = p.z = 0.0f;
            p.vx = frand(s_pe.desc.velocity_min.x, s_pe.desc.velocity_max.x);
            p.vy = frand(s_pe.desc.velocity_min.y, s_pe.desc.velocity_max.y);
            p.vz = frand(s_pe.desc.velocity_min.z, s_pe.desc.velocity_max.z);
            p.life = frand(s_pe.desc.lifetime_min, s_pe.desc.lifetime_max);
            p.age = 0.0f;
            p.size_start = s_pe.desc.size_start;
            p.size_end   = s_pe.desc.size_end;
            p.cs[0] = s_pe.desc.color_start.x; p.cs[1] = s_pe.desc.color_start.y;
            p.cs[2] = s_pe.desc.color_start.z; p.cs[3] = s_pe.desc.color_start.w;
            p.ce[0] = s_pe.desc.color_end.x;   p.ce[1] = s_pe.desc.color_end.y;
            p.ce[2] = s_pe.desc.color_end.z;   p.ce[3] = s_pe.desc.color_end.w;
            s_preview.push_back(p);
        }
        for (size_t i = 0; i < s_preview.size(); ) {
            PreviewParticle &p = s_preview[i];
            p.vx += s_pe.desc.gravity.x * dt;
            p.vy += s_pe.desc.gravity.y * dt;
            p.vz += s_pe.desc.gravity.z * dt;
            p.x  += p.vx * dt;
            p.y  += p.vy * dt;
            p.z  += p.vz * dt;
            p.age += dt;
            if (p.age >= p.life) {
                s_preview[i] = s_preview.back();
                s_preview.pop_back();
            } else { ++i; }
        }
        if ((int)s_preview.size() > s_pe.alive_peak)
            s_pe.alive_peak = (int)s_preview.size();
        s_pe.step_once = false;
    } else {
        s_preview.clear();
        s_pe.spawn_accum = 0.0f;
    }

    /* Push a clip rect so off-canvas geometry doesn't bleed. */
    dl->PushClipRect(cp0, cp1, true);

    /* Ground grid (10×10 metres). */
    if (s_pe.show_grid) {
        const int GN = 10;
        const float GS = 0.5f;
        for (int i = -GN; i <= GN; ++i) {
            float a = (float)i * GS;
            float b = (float)GN * GS;
            V3 p0w = { a, 0.0f, -b }, p1w = { a, 0.0f,  b };
            V3 p2w = { -b, 0.0f, a }, p3w = {  b, 0.0f, a };
            ImVec2 s0, s1, s2, s3; float zd, zs;
            bool ok0 = project(cam_to_view(p0w, s_pe.cam_yaw, s_pe.cam_pitch),
                               cam_pos, cp0, csz, fov_y, &s0, &zd, &zs);
            bool ok1 = project(cam_to_view(p1w, s_pe.cam_yaw, s_pe.cam_pitch),
                               cam_pos, cp0, csz, fov_y, &s1, &zd, &zs);
            bool ok2 = project(cam_to_view(p2w, s_pe.cam_yaw, s_pe.cam_pitch),
                               cam_pos, cp0, csz, fov_y, &s2, &zd, &zs);
            bool ok3 = project(cam_to_view(p3w, s_pe.cam_yaw, s_pe.cam_pitch),
                               cam_pos, cp0, csz, fov_y, &s3, &zd, &zs);
            ImU32 c = (i == 0) ? IM_COL32(110, 110, 130, 220)
                               : IM_COL32(60, 60, 75, 180);
            if (ok0 && ok1) dl->AddLine(s0, s1, c, 1.0f);
            if (ok2 && ok3) dl->AddLine(s2, s3, c, 1.0f);
        }
    }

    /* World axes. */
    if (s_pe.show_axes) {
        struct Ax { V3 p; ImU32 c; };
        Ax axes[3] = {
            { { 1.0f, 0.0f, 0.0f }, IM_COL32(230,  80,  80, 255) },
            { { 0.0f, 1.0f, 0.0f }, IM_COL32( 80, 230,  80, 255) },
            { { 0.0f, 0.0f, 1.0f }, IM_COL32( 80, 130, 230, 255) },
        };
        ImVec2 sO; float zd, zs;
        bool okO = project(cam_to_view({0,0,0}, s_pe.cam_yaw, s_pe.cam_pitch),
                           cam_pos, cp0, csz, fov_y, &sO, &zd, &zs);
        for (int i = 0; i < 3; ++i) {
            ImVec2 sA;
            bool okA = project(cam_to_view(axes[i].p, s_pe.cam_yaw, s_pe.cam_pitch),
                               cam_pos, cp0, csz, fov_y, &sA, &zd, &zs);
            if (okO && okA) dl->AddLine(sO, sA, axes[i].c, 2.0f);
        }
    }

    /* Project + sort particles back-to-front. */
    struct Splat { ImVec2 sp; float r; ImU32 col; float depth; };
    std::vector<Splat> splats;
    splats.reserve(s_preview.size());
    for (auto &p : s_preview) {
        V3 vp = cam_to_view({ p.x, p.y, p.z }, s_pe.cam_yaw, s_pe.cam_pitch);
        ImVec2 sp; float depth, sscale;
        if (!project(vp, cam_pos, cp0, csz, fov_y, &sp, &depth, &sscale)) continue;
        float u = p.age / (p.life > 0.0f ? p.life : 1.0f);
        if (u > 1.0f) u = 1.0f;
        float sz_w = p.size_start + (p.size_end - p.size_start) * u;
        float r    = sz_w * sscale * 0.5f;
        if (r < 1.0f) r = 1.0f;
        if (r > 200.0f) r = 200.0f;
        float c0 = p.cs[0] + (p.ce[0] - p.cs[0]) * u;
        float c1 = p.cs[1] + (p.ce[1] - p.cs[1]) * u;
        float c2 = p.cs[2] + (p.ce[2] - p.cs[2]) * u;
        float ca = p.cs[3] + (p.ce[3] - p.cs[3]) * u;
        Splat sp_e;
        sp_e.sp = sp; sp_e.r = r; sp_e.depth = depth;
        sp_e.col = IM_COL32((int)(c0 * 255.0f), (int)(c1 * 255.0f),
                            (int)(c2 * 255.0f), (int)(ca * 255.0f));
        splats.push_back(sp_e);
    }
    std::sort(splats.begin(), splats.end(),
              [](const Splat &a, const Splat &b) { return a.depth > b.depth; });
    for (auto &s : splats) dl->AddCircleFilled(s.sp, s.r, s.col);

    dl->PopClipRect();

    char hud[160];
    int alive_now = (int)s_preview.size();
    int cap = (int)s_pe.desc.max_particles;
    float fill = cap > 0 ? (float)alive_now / (float)cap : 0.0f;
    std::snprintf(hud, sizeof(hud),
                  "alive %d / %d (peak %d)  speed %.2fx  yaw %.2f  pitch %.2f  dist %.1fm",
                  alive_now, cap, s_pe.alive_peak,
                  s_pe.time_scale, s_pe.cam_yaw, s_pe.cam_pitch, s_pe.cam_dist);
    dl->AddText(ImVec2(cp0.x + 6.0f, cp1.y - 18.0f),
                jce_theme::text_secondary(), hud);
    /* Fill bar (top of canvas). */
    {
        float bw = csz.x - 12.0f;
        ImVec2 b0 = ImVec2(cp0.x + 6.0f, cp0.y + 6.0f);
        ImVec2 b1 = ImVec2(cp0.x + 6.0f + bw, cp0.y + 12.0f);
        dl->AddRectFilled(b0, b1, jce_theme::track_even());
        ImU32 fc = (fill > 0.9f) ? IM_COL32(220, 90, 90, 255)
                                  : IM_COL32(90, 200, 120, 255);
        dl->AddRectFilled(b0, ImVec2(b0.x + bw * fill, b1.y), fc);
        dl->AddRect(b0, b1, jce_theme::col_from(ImGuiCol_Border));
    }
    ImGui::TextDisabled("%s", jce_editor_i18n("particleEditor.hint.shadowSim"));

    /* Live stats. */
    if (s_pe.sys) {
        float dt2 = ImGui::GetIO().DeltaTime * s_pe.time_scale;
        if (s_pe.playing) jce_particles_update(s_pe.sys, dt2);
        ImGui::Text(jce_editor_i18n("particleEditor.stats.engine"),
                    jce_particles_alive_count(s_pe.sys),
                    alive_now, s_pe.alive_peak);
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("particleEditor.stats.idle"));
    }
}

} /* namespace */

extern "C" void jce_editor_panel_particle_editor(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PARTICLE_EDITOR);
    if (!vis || !*vis) return;
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_particle_editor", jce_editor_i18n("particleEditor.title"));
    if (ImGui::Begin(_wt, vis)) {
        draw_content();
    }
    ImGui::End();
}
