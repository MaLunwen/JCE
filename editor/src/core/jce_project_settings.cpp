/*
 * jce_project_settings.cpp  Project-scoped settings persistence and
 * runtime apply.
 *
 * On-disk format: `.jce/project-settings.json`. JSON layout mirrors the
 * struct organisation declared in jce_project_settings.h. Missing keys
 * fall back to defaults provided by jce_project_settings_defaults().
 */

#include "jce_project_settings.h"
#include "jce_editor_alloc.h"
#include "io/jce_editor_file_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
}

#define LOG_TAG     "project_settings"
#define PS_PATH     ".jce/project-settings.json"
#define PS_DIR      ".jce"

/* ── Cached snapshot ──────────────────────────────────────────────── */

static JceProjectSettings s_ps_current;
static bool               s_ps_have = false;

const JceProjectSettings *jce_project_settings_current(void)
{
    return s_ps_have ? &s_ps_current : NULL;
}

/* ── Defaults ─────────────────────────────────────────────────────── */

static const char *kBuiltinLayer0 = "Default";
static const char *kBuiltinLayer1 = "TransparentFX";
static const char *kBuiltinLayer2 = "IgnoreRaycast";
static const char *kBuiltinLayer4 = "Water";
static const char *kBuiltinLayer5 = "UI";

static const char *kDefaultTags[] = {
    "Untagged", "Respawn", "Finish", "EditorOnly", "MainCamera", "Player", "GameController"
};
static const char *kDefaultSortLayers[] = { "Default" };

void jce_project_settings_defaults(JceProjectSettings *s)
{
    memset(s, 0, sizeof(*s));

    /* Audio */
    s->audio.master_volume         = 1.0f;
    s->audio.doppler_factor        = 1.0f;
    s->audio.sample_rate           = 48000;
    s->audio.pause_on_focus_loss   = true;

    /* Editor */
    s->editor.auto_save_enabled       = true;
    s->editor.auto_save_interval_sec  = 300;
    s->editor.default_behavior_mode   = 0;   /* 3D */
    s->editor.version_control_mode    = 1;   /* Visible Meta */

    /* Graphics */
    s->graphics.color_space          = 1;    /* Linear */
    s->graphics.hdr                  = true;
    s->graphics.srgb_write           = true;
    s->graphics.default_msaa         = 4;
    s->graphics.anisotropic_textures = 1;    /* PerTexture */

    /* Input */
    s->input.dead_zone     = 0.19f;
    s->input.gravity       = 3.0f;
    s->input.sensitivity   = 1.0f;
    s->input.enable_gamepad = true;

    /* Physics 3D */
    s->physics.gravity[0] = 0.0f;
    s->physics.gravity[1] = -9.81f;
    s->physics.gravity[2] = 0.0f;
    s->physics.default_contact_offset             = 0.01f;
    s->physics.default_solver_iterations          = 6;
    s->physics.default_solver_velocity_iterations = 1;
    s->physics.bounce_threshold                   = 2.0f;
    s->physics.sleep_threshold                    = 0.005f;
    s->physics.queries_hit_triggers               = true;
    s->physics.auto_simulation                    = true;
    for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
        s->physics.layer_collision_matrix[i] = 0xFFFFFFFFu;

    /* Physics 2D */
    s->physics2d.gravity[0]              = 0.0f;
    s->physics2d.gravity[1]              = -9.81f;
    s->physics2d.velocity_iterations     = 8;
    s->physics2d.position_iterations     = 3;
    s->physics2d.queries_hit_triggers    = true;
    s->physics2d.auto_sync_transforms    = false;
    s->physics2d.auto_simulation         = true;
    for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
        s->physics2d.layer_collision_matrix[i] = 0xFFFFFFFFu;

    /* Player */
    strncpy(s->player.company_name, "DefaultCompany", JCE_PS_NAME_LEN - 1);
    strncpy(s->player.product_name, "JCEProject",     JCE_PS_NAME_LEN - 1);
    strncpy(s->player.version,      "0.1.0",          sizeof(s->player.version) - 1);
    s->player.splash_bg_color[0] = 0.13f;
    s->player.splash_bg_color[1] = 0.13f;
    s->player.splash_bg_color[2] = 0.13f;
    s->player.show_splash         = true;
    s->player.run_in_background   = false;
    s->player.fullscreen_default  = false;
    s->player.default_screen_width  = 1280;
    s->player.default_screen_height = 720;

    /* Quality: Low / Medium / High */
    s->quality.count = 3;
    s->quality.current_level = 2;
    static const char *names[3] = { "Low", "Medium", "High" };
    static const int   shadow_q  [3] = { 0, 1, 2 };
    static const int   aa        [3] = { 0, 2, 4 };
    static const float shadow_d  [3] = { 20.0f, 40.0f, 150.0f };
    static const int   shadow_res[3] = { 1, 2, 3 };
    static const int   shadow_cas[3] = { 1, 2, 4 };
    static const int   tex_q     [3] = { 2, 1, 0 };
    for (int i = 0; i < 3; i++) {
        JceProjectQualityLevel *q = &s->quality.levels[i];
        strncpy(q->name, names[i], JCE_PS_NAME_LEN - 1);
        q->pixel_light_count          = 1 + i;
        q->texture_quality            = tex_q[i];
        q->anisotropic                = (i >= 1) ? 1 : 0;
        q->anti_aliasing              = aa[i];
        q->soft_particles             = (i == 2);
        q->realtime_reflection_probes = (i == 2);
        q->shadow_quality             = shadow_q[i];
        q->shadow_resolution          = shadow_res[i];
        q->shadow_distance            = shadow_d[i];
        q->shadow_cascades            = shadow_cas[i];
        q->vsync_count                = 1;
        q->target_framerate           = -1;
        q->lod_bias                   = 1.0f + 0.5f * (float)i;
    }

    /* Tags & Layers */
    s->tags_layers.tag_count = (int)(sizeof(kDefaultTags)/sizeof(kDefaultTags[0]));
    for (int i = 0; i < s->tags_layers.tag_count; i++)
        strncpy(s->tags_layers.tags[i], kDefaultTags[i], JCE_PS_NAME_LEN - 1);
    s->tags_layers.sorting_layer_count = 1;
    strncpy(s->tags_layers.sorting_layers[0], kDefaultSortLayers[0], JCE_PS_NAME_LEN - 1);
    strncpy(s->tags_layers.layers[0], kBuiltinLayer0, JCE_PS_NAME_LEN - 1);
    strncpy(s->tags_layers.layers[1], kBuiltinLayer1, JCE_PS_NAME_LEN - 1);
    strncpy(s->tags_layers.layers[2], kBuiltinLayer2, JCE_PS_NAME_LEN - 1);
    strncpy(s->tags_layers.layers[4], kBuiltinLayer4, JCE_PS_NAME_LEN - 1);
    strncpy(s->tags_layers.layers[5], kBuiltinLayer5, JCE_PS_NAME_LEN - 1);

    /* Time */
    s->time.fixed_timestep                  = 0.02f;
    s->time.max_allowed_timestep            = 0.3333f;
    s->time.time_scale                      = 1.0f;
    s->time.maximum_particle_timestep_ms    = 30;

    /* Presets */
    s->presets.count = 0;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static void read_str(const JceJson *root, const char *key, char *out, size_t n)
{
    const char *s = jce_json_get_string(root, key, NULL);
    if (s) { strncpy(out, s, n - 1); out[n - 1] = '\0'; }
}

static JceJson *child_obj_or_null(const JceJson *parent, const char *key)
{
    JceJson *c = jce_json_get(parent, key);
    return (c && jce_json_is_object(c)) ? c : NULL;
}

/* ── Save ─────────────────────────────────────────────────────────── */

bool jce_project_settings_save(const JceProjectSettings *s)
{
    jce_fs_host_create_directory(PS_DIR);

    JceJson *root = jce_json_object();

    /* audio */
    {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "master_volume",       s->audio.master_volume);
        jce_json_set_number(o, "doppler_factor",      s->audio.doppler_factor);
        jce_json_set_int   (o, "sample_rate",         s->audio.sample_rate);
        jce_json_set_bool  (o, "pause_on_focus_loss", s->audio.pause_on_focus_loss);
        jce_json_set_bool  (o, "disable_audio",       s->audio.disable_audio);
        jce_json_set_child (root, "audio", o);
    }
    /* editor */
    {
        JceJson *o = jce_json_object();
        jce_json_set_bool  (o, "auto_save_enabled",      s->editor.auto_save_enabled);
        jce_json_set_int   (o, "auto_save_interval_sec", s->editor.auto_save_interval_sec);
        jce_json_set_int   (o, "default_behavior_mode",  s->editor.default_behavior_mode);
        jce_json_set_int   (o, "version_control_mode",   s->editor.version_control_mode);
        jce_json_set_string(o, "external_script_editor", s->editor.external_script_editor);
        jce_json_set_string(o, "external_image_editor",  s->editor.external_image_editor);
        jce_json_set_child (root, "editor", o);
    }
    /* graphics */
    {
        JceJson *o = jce_json_object();
        jce_json_set_int   (o, "color_space",          s->graphics.color_space);
        jce_json_set_bool  (o, "hdr",                  s->graphics.hdr);
        jce_json_set_bool  (o, "srgb_write",           s->graphics.srgb_write);
        jce_json_set_int   (o, "default_msaa",         s->graphics.default_msaa);
        jce_json_set_int   (o, "anisotropic_textures", s->graphics.anisotropic_textures);
        jce_json_set_string(o, "always_included_shaders",
                            s->graphics.always_included_shaders);
        jce_json_set_child (root, "graphics", o);
    }
    /* input */
    {
        JceJson *o = jce_json_object();
        jce_json_set_bool  (o, "treat_keyboard_as_dpad", s->input.treat_keyboard_as_dpad);
        jce_json_set_number(o, "dead_zone",   s->input.dead_zone);
        jce_json_set_number(o, "gravity",     s->input.gravity);
        jce_json_set_number(o, "sensitivity", s->input.sensitivity);
        jce_json_set_bool  (o, "enable_gamepad", s->input.enable_gamepad);
        jce_json_set_child (root, "input", o);
    }
    /* physics 3d */
    {
        JceJson *o = jce_json_object();
        jce_json_set_float_array(o, "gravity", s->physics.gravity, 3);
        jce_json_set_number(o, "default_contact_offset", s->physics.default_contact_offset);
        jce_json_set_int   (o, "default_solver_iterations", s->physics.default_solver_iterations);
        jce_json_set_int   (o, "default_solver_velocity_iterations",
                            s->physics.default_solver_velocity_iterations);
        jce_json_set_number(o, "bounce_threshold",  s->physics.bounce_threshold);
        jce_json_set_number(o, "sleep_threshold",   s->physics.sleep_threshold);
        jce_json_set_bool  (o, "queries_hit_triggers", s->physics.queries_hit_triggers);
        jce_json_set_bool  (o, "queries_hit_backfaces",s->physics.queries_hit_backfaces);
        jce_json_set_bool  (o, "auto_simulation",   s->physics.auto_simulation);
        JceJson *m = jce_json_array();
        for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
            jce_json_array_push_number(m, (double)s->physics.layer_collision_matrix[i]);
        jce_json_set_child(o, "layer_collision_matrix", m);
        jce_json_set_child(root, "physics", o);
    }
    /* physics 2d */
    {
        JceJson *o = jce_json_object();
        jce_json_set_float_array(o, "gravity", s->physics2d.gravity, 2);
        jce_json_set_int   (o, "velocity_iterations", s->physics2d.velocity_iterations);
        jce_json_set_int   (o, "position_iterations", s->physics2d.position_iterations);
        jce_json_set_bool  (o, "queries_hit_triggers", s->physics2d.queries_hit_triggers);
        jce_json_set_bool  (o, "auto_sync_transforms", s->physics2d.auto_sync_transforms);
        jce_json_set_bool  (o, "auto_simulation",      s->physics2d.auto_simulation);
        JceJson *m = jce_json_array();
        for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
            jce_json_array_push_number(m, (double)s->physics2d.layer_collision_matrix[i]);
        jce_json_set_child(o, "layer_collision_matrix", m);
        jce_json_set_child(root, "physics2d", o);
    }
    /* player */
    {
        JceJson *o = jce_json_object();
        jce_json_set_string(o, "company_name", s->player.company_name);
        jce_json_set_string(o, "product_name", s->player.product_name);
        jce_json_set_string(o, "version",      s->player.version);
        jce_json_set_string(o, "default_icon_path",   s->player.default_icon_path);
        jce_json_set_string(o, "default_cursor_path", s->player.default_cursor_path);
        jce_json_set_float_array(o, "splash_bg_color", s->player.splash_bg_color, 3);
        jce_json_set_bool  (o, "show_splash",        s->player.show_splash);
        jce_json_set_bool  (o, "run_in_background",  s->player.run_in_background);
        jce_json_set_bool  (o, "fullscreen_default", s->player.fullscreen_default);
        jce_json_set_int   (o, "default_screen_width",  s->player.default_screen_width);
        jce_json_set_int   (o, "default_screen_height", s->player.default_screen_height);
        jce_json_set_child(root, "player", o);
    }
    /* presets */
    {
        JceJson *arr = jce_json_array();
        for (int i = 0; i < s->presets.count; i++) {
            JceJson *e = jce_json_object();
            jce_json_set_string(e, "component_type", s->presets.bindings[i].component_type);
            jce_json_set_string(e, "preset_path",    s->presets.bindings[i].preset_path);
            jce_json_set_string(e, "filter",         s->presets.bindings[i].filter);
            jce_json_array_push(arr, e);
        }
        jce_json_set_child(root, "presets", arr);
    }
    /* quality */
    {
        JceJson *o = jce_json_object();
        jce_json_set_int(o, "current_level", s->quality.current_level);
        JceJson *arr = jce_json_array();
        for (int i = 0; i < s->quality.count; i++) {
            const JceProjectQualityLevel *q = &s->quality.levels[i];
            JceJson *qo = jce_json_object();
            jce_json_set_string(qo, "name", q->name);
            jce_json_set_int   (qo, "pixel_light_count",  q->pixel_light_count);
            jce_json_set_int   (qo, "texture_quality",    q->texture_quality);
            jce_json_set_int   (qo, "anisotropic",        q->anisotropic);
            jce_json_set_int   (qo, "anti_aliasing",      q->anti_aliasing);
            jce_json_set_bool  (qo, "soft_particles",     q->soft_particles);
            jce_json_set_bool  (qo, "realtime_reflection_probes", q->realtime_reflection_probes);
            jce_json_set_int   (qo, "shadow_quality",     q->shadow_quality);
            jce_json_set_int   (qo, "shadow_resolution",  q->shadow_resolution);
            jce_json_set_number(qo, "shadow_distance",    q->shadow_distance);
            jce_json_set_int   (qo, "shadow_cascades",    q->shadow_cascades);
            jce_json_set_int   (qo, "vsync_count",        q->vsync_count);
            jce_json_set_int   (qo, "target_framerate",   q->target_framerate);
            jce_json_set_number(qo, "lod_bias",           q->lod_bias);
            jce_json_array_push(arr, qo);
        }
        jce_json_set_child(o, "levels", arr);
        jce_json_set_child(root, "quality", o);
    }
    /* tags & layers */
    {
        JceJson *o = jce_json_object();
        JceJson *t = jce_json_array();
        for (int i = 0; i < s->tags_layers.tag_count; i++)
            jce_json_array_push_string(t, s->tags_layers.tags[i]);
        jce_json_set_child(o, "tags", t);
        JceJson *sl = jce_json_array();
        for (int i = 0; i < s->tags_layers.sorting_layer_count; i++)
            jce_json_array_push_string(sl, s->tags_layers.sorting_layers[i]);
        jce_json_set_child(o, "sorting_layers", sl);
        JceJson *ly = jce_json_array();
        for (int i = 0; i < JCE_PS_LAYER_COUNT; i++)
            jce_json_array_push_string(ly, s->tags_layers.layers[i]);
        jce_json_set_child(o, "layers", ly);
        jce_json_set_child(root, "tags_layers", o);
    }
    /* time */
    {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "fixed_timestep",       s->time.fixed_timestep);
        jce_json_set_number(o, "max_allowed_timestep", s->time.max_allowed_timestep);
        jce_json_set_number(o, "time_scale",           s->time.time_scale);
        jce_json_set_int   (o, "maximum_particle_timestep_ms",
                            s->time.maximum_particle_timestep_ms);
        jce_json_set_child(root, "time", o);
    }

    bool ok = jce_json_write_file(PS_PATH, root, true, true);
    if (!ok) LOG_ERROR(LOG_TAG, "Failed to write %s", PS_PATH);

    /* Update cached snapshot. */
    s_ps_current = *s;
    s_ps_have    = true;
    return ok;
}

/* ── Load ─────────────────────────────────────────────────────────── */

bool jce_project_settings_load(JceProjectSettings *out)
{
    jce_project_settings_defaults(out);

    JceJson *root = jce_json_parse_file(PS_PATH);
    if (!root) {
        LOG_INFO(LOG_TAG, "%s not found; using defaults", PS_PATH);
        s_ps_current = *out;
        s_ps_have    = true;
        return false;
    }

    /* audio */
    if (JceJson *o = child_obj_or_null(root, "audio")) {
        out->audio.master_volume        = (float)jce_json_get_number(o, "master_volume",  out->audio.master_volume);
        out->audio.doppler_factor       = (float)jce_json_get_number(o, "doppler_factor", out->audio.doppler_factor);
        out->audio.sample_rate          = jce_json_get_int (o, "sample_rate", out->audio.sample_rate);
        out->audio.pause_on_focus_loss  = jce_json_get_bool(o, "pause_on_focus_loss", out->audio.pause_on_focus_loss);
        out->audio.disable_audio        = jce_json_get_bool(o, "disable_audio", out->audio.disable_audio);
    }
    if (JceJson *o = child_obj_or_null(root, "editor")) {
        out->editor.auto_save_enabled      = jce_json_get_bool(o, "auto_save_enabled", out->editor.auto_save_enabled);
        out->editor.auto_save_interval_sec = jce_json_get_int (o, "auto_save_interval_sec", out->editor.auto_save_interval_sec);
        out->editor.default_behavior_mode  = jce_json_get_int (o, "default_behavior_mode", out->editor.default_behavior_mode);
        out->editor.version_control_mode   = jce_json_get_int (o, "version_control_mode", out->editor.version_control_mode);
        read_str(o, "external_script_editor", out->editor.external_script_editor, JCE_PS_PATH_LEN);
        read_str(o, "external_image_editor",  out->editor.external_image_editor,  JCE_PS_PATH_LEN);
    }
    if (JceJson *o = child_obj_or_null(root, "graphics")) {
        out->graphics.color_space          = jce_json_get_int (o, "color_space", out->graphics.color_space);
        out->graphics.hdr                  = jce_json_get_bool(o, "hdr", out->graphics.hdr);
        out->graphics.srgb_write           = jce_json_get_bool(o, "srgb_write", out->graphics.srgb_write);
        out->graphics.default_msaa         = jce_json_get_int (o, "default_msaa", out->graphics.default_msaa);
        out->graphics.anisotropic_textures = jce_json_get_int (o, "anisotropic_textures", out->graphics.anisotropic_textures);
        read_str(o, "always_included_shaders", out->graphics.always_included_shaders,
                 sizeof(out->graphics.always_included_shaders));
    }
    if (JceJson *o = child_obj_or_null(root, "input")) {
        out->input.treat_keyboard_as_dpad = jce_json_get_bool(o, "treat_keyboard_as_dpad", out->input.treat_keyboard_as_dpad);
        out->input.dead_zone   = (float)jce_json_get_number(o, "dead_zone",   out->input.dead_zone);
        out->input.gravity     = (float)jce_json_get_number(o, "gravity",     out->input.gravity);
        out->input.sensitivity = (float)jce_json_get_number(o, "sensitivity", out->input.sensitivity);
        out->input.enable_gamepad = jce_json_get_bool(o, "enable_gamepad", out->input.enable_gamepad);
    }
    if (JceJson *o = child_obj_or_null(root, "physics")) {
        jce_json_get_floats(o, "gravity", out->physics.gravity, 3, out->physics.gravity);
        out->physics.default_contact_offset             = (float)jce_json_get_number(o, "default_contact_offset", out->physics.default_contact_offset);
        out->physics.default_solver_iterations          = jce_json_get_int(o, "default_solver_iterations", out->physics.default_solver_iterations);
        out->physics.default_solver_velocity_iterations = jce_json_get_int(o, "default_solver_velocity_iterations", out->physics.default_solver_velocity_iterations);
        out->physics.bounce_threshold = (float)jce_json_get_number(o, "bounce_threshold", out->physics.bounce_threshold);
        out->physics.sleep_threshold  = (float)jce_json_get_number(o, "sleep_threshold",  out->physics.sleep_threshold);
        out->physics.queries_hit_triggers  = jce_json_get_bool(o, "queries_hit_triggers",  out->physics.queries_hit_triggers);
        out->physics.queries_hit_backfaces = jce_json_get_bool(o, "queries_hit_backfaces", out->physics.queries_hit_backfaces);
        out->physics.auto_simulation       = jce_json_get_bool(o, "auto_simulation",       out->physics.auto_simulation);
        JceJson *m = jce_json_get(o, "layer_collision_matrix");
        if (m && jce_json_is_array(m)) {
            int n = jce_json_array_size(m);
            if (n > JCE_PS_LAYER_COUNT) n = JCE_PS_LAYER_COUNT;
            for (int i = 0; i < n; i++)
                out->physics.layer_collision_matrix[i] =
                    (uint32_t)jce_json_number_value(jce_json_array_at(m, i),
                                                   (double)out->physics.layer_collision_matrix[i]);
        }
    }
    if (JceJson *o = child_obj_or_null(root, "physics2d")) {
        jce_json_get_floats(o, "gravity", out->physics2d.gravity, 2, out->physics2d.gravity);
        out->physics2d.velocity_iterations  = jce_json_get_int (o, "velocity_iterations", out->physics2d.velocity_iterations);
        out->physics2d.position_iterations  = jce_json_get_int (o, "position_iterations", out->physics2d.position_iterations);
        out->physics2d.queries_hit_triggers = jce_json_get_bool(o, "queries_hit_triggers", out->physics2d.queries_hit_triggers);
        out->physics2d.auto_sync_transforms = jce_json_get_bool(o, "auto_sync_transforms", out->physics2d.auto_sync_transforms);
        out->physics2d.auto_simulation      = jce_json_get_bool(o, "auto_simulation",      out->physics2d.auto_simulation);
        JceJson *m = jce_json_get(o, "layer_collision_matrix");
        if (m && jce_json_is_array(m)) {
            int n = jce_json_array_size(m);
            if (n > JCE_PS_LAYER_COUNT) n = JCE_PS_LAYER_COUNT;
            for (int i = 0; i < n; i++)
                out->physics2d.layer_collision_matrix[i] =
                    (uint32_t)jce_json_number_value(jce_json_array_at(m, i),
                                                   (double)out->physics2d.layer_collision_matrix[i]);
        }
    }
    if (JceJson *o = child_obj_or_null(root, "player")) {
        read_str(o, "company_name", out->player.company_name, JCE_PS_NAME_LEN);
        read_str(o, "product_name", out->player.product_name, JCE_PS_NAME_LEN);
        read_str(o, "version",      out->player.version,      sizeof(out->player.version));
        read_str(o, "default_icon_path",   out->player.default_icon_path,   JCE_PS_PATH_LEN);
        read_str(o, "default_cursor_path", out->player.default_cursor_path, JCE_PS_PATH_LEN);
        jce_json_get_floats(o, "splash_bg_color", out->player.splash_bg_color, 3,
                            out->player.splash_bg_color);
        out->player.show_splash         = jce_json_get_bool(o, "show_splash",        out->player.show_splash);
        out->player.run_in_background   = jce_json_get_bool(o, "run_in_background",  out->player.run_in_background);
        out->player.fullscreen_default  = jce_json_get_bool(o, "fullscreen_default", out->player.fullscreen_default);
        out->player.default_screen_width  = jce_json_get_int(o, "default_screen_width",  out->player.default_screen_width);
        out->player.default_screen_height = jce_json_get_int(o, "default_screen_height", out->player.default_screen_height);
    }
    if (JceJson *arr = jce_json_get(root, "presets")) {
        if (jce_json_is_array(arr)) {
            int n = jce_json_array_size(arr);
            if (n > JCE_PS_MAX_PRESET_BINDINGS) n = JCE_PS_MAX_PRESET_BINDINGS;
            out->presets.count = n;
            for (int i = 0; i < n; i++) {
                JceJson *e = jce_json_array_at(arr, i);
                if (!e || !jce_json_is_object(e)) continue;
                read_str(e, "component_type", out->presets.bindings[i].component_type, JCE_PS_NAME_LEN);
                read_str(e, "preset_path",    out->presets.bindings[i].preset_path,    JCE_PS_PATH_LEN);
                read_str(e, "filter",         out->presets.bindings[i].filter,         JCE_PS_NAME_LEN);
            }
        }
    }
    if (JceJson *o = child_obj_or_null(root, "quality")) {
        out->quality.current_level = jce_json_get_int(o, "current_level", out->quality.current_level);
        JceJson *arr = jce_json_get(o, "levels");
        if (arr && jce_json_is_array(arr)) {
            int n = jce_json_array_size(arr);
            if (n > JCE_PS_MAX_QUALITY_LEVELS) n = JCE_PS_MAX_QUALITY_LEVELS;
            out->quality.count = n;
            for (int i = 0; i < n; i++) {
                JceJson *qo = jce_json_array_at(arr, i);
                if (!qo || !jce_json_is_object(qo)) continue;
                JceProjectQualityLevel *q = &out->quality.levels[i];
                read_str(qo, "name", q->name, JCE_PS_NAME_LEN);
                q->pixel_light_count = jce_json_get_int(qo, "pixel_light_count", q->pixel_light_count);
                q->texture_quality   = jce_json_get_int(qo, "texture_quality",   q->texture_quality);
                q->anisotropic       = jce_json_get_int(qo, "anisotropic",       q->anisotropic);
                q->anti_aliasing     = jce_json_get_int(qo, "anti_aliasing",     q->anti_aliasing);
                q->soft_particles    = jce_json_get_bool(qo, "soft_particles",   q->soft_particles);
                q->realtime_reflection_probes = jce_json_get_bool(qo, "realtime_reflection_probes", q->realtime_reflection_probes);
                q->shadow_quality    = jce_json_get_int(qo, "shadow_quality",    q->shadow_quality);
                q->shadow_resolution = jce_json_get_int(qo, "shadow_resolution", q->shadow_resolution);
                q->shadow_distance   = (float)jce_json_get_number(qo, "shadow_distance", q->shadow_distance);
                q->shadow_cascades   = jce_json_get_int(qo, "shadow_cascades",   q->shadow_cascades);
                q->vsync_count       = jce_json_get_int(qo, "vsync_count",       q->vsync_count);
                q->target_framerate  = jce_json_get_int(qo, "target_framerate",  q->target_framerate);
                q->lod_bias          = (float)jce_json_get_number(qo, "lod_bias", q->lod_bias);
            }
        }
    }
    if (JceJson *o = child_obj_or_null(root, "tags_layers")) {
        JceJson *t = jce_json_get(o, "tags");
        if (t && jce_json_is_array(t)) {
            int n = jce_json_array_size(t);
            if (n > JCE_PS_MAX_TAGS) n = JCE_PS_MAX_TAGS;
            out->tags_layers.tag_count = n;
            for (int i = 0; i < n; i++) {
                const char *s = jce_json_string_value(jce_json_array_at(t, i), "");
                strncpy(out->tags_layers.tags[i], s, JCE_PS_NAME_LEN - 1);
                out->tags_layers.tags[i][JCE_PS_NAME_LEN - 1] = '\0';
            }
        }
        JceJson *sl = jce_json_get(o, "sorting_layers");
        if (sl && jce_json_is_array(sl)) {
            int n = jce_json_array_size(sl);
            if (n > JCE_PS_MAX_SORTING_LAYERS) n = JCE_PS_MAX_SORTING_LAYERS;
            out->tags_layers.sorting_layer_count = n;
            for (int i = 0; i < n; i++) {
                const char *s = jce_json_string_value(jce_json_array_at(sl, i), "");
                strncpy(out->tags_layers.sorting_layers[i], s, JCE_PS_NAME_LEN - 1);
                out->tags_layers.sorting_layers[i][JCE_PS_NAME_LEN - 1] = '\0';
            }
        }
        JceJson *ly = jce_json_get(o, "layers");
        if (ly && jce_json_is_array(ly)) {
            int n = jce_json_array_size(ly);
            if (n > JCE_PS_LAYER_COUNT) n = JCE_PS_LAYER_COUNT;
            for (int i = 0; i < n; i++) {
                const char *s = jce_json_string_value(jce_json_array_at(ly, i), "");
                strncpy(out->tags_layers.layers[i], s, JCE_PS_NAME_LEN - 1);
                out->tags_layers.layers[i][JCE_PS_NAME_LEN - 1] = '\0';
            }
        }
    }
    if (JceJson *o = child_obj_or_null(root, "time")) {
        out->time.fixed_timestep                  = (float)jce_json_get_number(o, "fixed_timestep",       out->time.fixed_timestep);
        out->time.max_allowed_timestep            = (float)jce_json_get_number(o, "max_allowed_timestep", out->time.max_allowed_timestep);
        out->time.time_scale                      = (float)jce_json_get_number(o, "time_scale",           out->time.time_scale);
        out->time.maximum_particle_timestep_ms    = jce_json_get_int(o, "maximum_particle_timestep_ms", out->time.maximum_particle_timestep_ms);
    }

    jce_json_free(root);
    s_ps_current = *out;
    s_ps_have    = true;
    return true;
}

/* ── Apply (best-effort runtime sync) ─────────────────────────────── */

void jce_project_settings_apply(const JceProjectSettings *s)
{
    /* Persist snapshot first so any new play session reads fresh values. */
    s_ps_current = *s;
    s_ps_have    = true;

    /* Live-apply hooks: most engine subsystems consume via per-play
     * session creation (see jce_editor_play). The values here are picked
     * up at that boundary. Hot-apply for currently-playing sessions is
     * tracked under the P1 follow-up backlog. */
}
