/*
 * jce_render_settings.c — project-wide render/quality settings carrier.
 * See jce_render_settings.h.  Mirrors the JSON I/O pattern of
 * jce_physics_layers.c (jce_json + jce_fs_host_*).
 */

#include <jce/renderer/jce_render_settings.h>

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "render_settings"

JceRenderSettings jce_render_settings_default(void)
{
    JceRenderSettings s;
    memset(&s, 0, sizeof s);
    s.shadow_quality = 2;   /* hard+soft — matches the renderer's default */
    s.vsync          = 1;

    /* Look Profile project defaults — all neutral (byte-identical baseline). */
    s.rim_color[0] = 1.0f; s.rim_color[1] = 1.0f; s.rim_color[2] = 1.0f;
    s.rim_power    = 4.0f;
    s.tonemap_op   = 0;   /* JCE_TONEMAP_ACES */
    /* memset already zeroed wrap_factor/ambient_hemisphere/ground/rim_intensity/
     * lut_path/lut_strength/toon_character/bloom_knee. */

    /* Grass (plan 07) — off by default; old project files that lack the key
     * will keep this 0 via the defaults-first load pattern. */
    s.grass_enabled = 0;

    return s;
}

bool jce_render_settings_save_json(const char *vfs_path, const JceRenderSettings *s)
{
    if (!vfs_path || !s) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema", "jce.rendersettings.v2");
    jce_json_set_int(root, "shadowQuality",   s->shadow_quality);
    jce_json_set_int(root, "shadowMapSize",   s->shadow_map_size);
    jce_json_set_int(root, "shadowCascades",  s->shadow_cascades);
    jce_json_set_number(root, "shadowDistance", (double)s->shadow_distance);
    jce_json_set_number(root, "lodBias",        (double)s->lod_bias);
    jce_json_set_int(root, "vsync",           s->vsync);
    jce_json_set_int(root, "msaa",            s->msaa);
    jce_json_set_int(root, "grassEnabled",    s->grass_enabled);

    /* ── Look Profile project defaults (plan 02) ─────────────────────────
     * Nested "look" object; absent in old files (v1) → parse keeps neutral
     * defaults.  Flat channel keys (groundR/rimR…) avoid needing a float-
     * array helper that does not exist in this lighter jce_json API. */
    JceJson *look = jce_json_object();
    if (look) {
        jce_json_set_number(look, "wrap", (double)s->wrap_factor);
        jce_json_set_bool(look, "hemisphere", s->ambient_hemisphere);
        jce_json_set_number(look, "groundR", (double)s->ambient_ground_color[0]);
        jce_json_set_number(look, "groundG", (double)s->ambient_ground_color[1]);
        jce_json_set_number(look, "groundB", (double)s->ambient_ground_color[2]);
        jce_json_set_number(look, "rimR", (double)s->rim_color[0]);
        jce_json_set_number(look, "rimG", (double)s->rim_color[1]);
        jce_json_set_number(look, "rimB", (double)s->rim_color[2]);
        jce_json_set_number(look, "rimPower", (double)s->rim_power);
        jce_json_set_number(look, "rimIntensity", (double)s->rim_intensity);
        jce_json_set_int(look, "tonemapOp", s->tonemap_op);
        jce_json_set_string(look, "lutPath", s->lut_path);
        jce_json_set_number(look, "lutStrength", (double)s->lut_strength);
        jce_json_set_bool(look, "toonCharacter", s->toon_character);
        jce_json_set_number(look, "bloomKnee", (double)s->bloom_knee);
        jce_json_set_child(root, "look", look);
    }

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    bool ok = jce_fs_host_write_all(vfs_path, json_str, strlen(json_str));
    jce_json_free_string(json_str);
    if (!ok) LOG_WARN(LOG_TAG, "cannot write render settings: %s", vfs_path);
    return ok;
}

/* Parse render settings from an already-loaded JSON buffer.  The single-exe
 * runtime uses this to read render_settings.json straight from the embedded
 * PAK (jce_pak_decompress bytes) when no loose cooked tree is on disk. */
bool jce_render_settings_load_json_mem(const char *json, size_t len,
                                       JceRenderSettings *out)
{
    if (!json || !out) return false;
    if (len == 0) len = strlen(json);
    if (len > (1u << 20)) return false;

    JceJson *root = jce_json_parse(json, len);
    if (!root) {
        LOG_WARN(LOG_TAG, "%s", "invalid render settings JSON (mem)");
        return false;
    }

    /* Start from defaults so a partial/old document still yields a valid set. */
    *out = jce_render_settings_default();
    out->shadow_quality  = (int)jce_json_number_value(jce_json_get(root, "shadowQuality"),
                                                      (double)out->shadow_quality);
    out->shadow_map_size = (int)jce_json_number_value(jce_json_get(root, "shadowMapSize"),
                                                      (double)out->shadow_map_size);
    out->shadow_cascades = (int)jce_json_number_value(jce_json_get(root, "shadowCascades"),
                                                      (double)out->shadow_cascades);
    out->shadow_distance = (float)jce_json_number_value(jce_json_get(root, "shadowDistance"),
                                                        (double)out->shadow_distance);
    out->lod_bias        = (float)jce_json_number_value(jce_json_get(root, "lodBias"),
                                                        (double)out->lod_bias);
    out->vsync           = (int)jce_json_number_value(jce_json_get(root, "vsync"),
                                                      (double)out->vsync);
    out->msaa            = (int)jce_json_number_value(jce_json_get(root, "msaa"),
                                                      (double)out->msaa);
    out->grass_enabled   = (int)jce_json_number_value(jce_json_get(root, "grassEnabled"),
                                                      (double)out->grass_enabled);

    /* ── Look Profile (absent "look" key → out already holds neutral defaults) */
    JceJson *look = jce_json_get(root, "look");
    if (look) {
        out->wrap_factor = (float)jce_json_number_value(jce_json_get(look, "wrap"),
                                                        (double)out->wrap_factor);
        out->ambient_hemisphere = jce_json_get_bool(look, "hemisphere",
                                                    out->ambient_hemisphere);
        out->ambient_ground_color[0] = (float)jce_json_number_value(
            jce_json_get(look, "groundR"), (double)out->ambient_ground_color[0]);
        out->ambient_ground_color[1] = (float)jce_json_number_value(
            jce_json_get(look, "groundG"), (double)out->ambient_ground_color[1]);
        out->ambient_ground_color[2] = (float)jce_json_number_value(
            jce_json_get(look, "groundB"), (double)out->ambient_ground_color[2]);
        out->rim_color[0] = (float)jce_json_number_value(
            jce_json_get(look, "rimR"), (double)out->rim_color[0]);
        out->rim_color[1] = (float)jce_json_number_value(
            jce_json_get(look, "rimG"), (double)out->rim_color[1]);
        out->rim_color[2] = (float)jce_json_number_value(
            jce_json_get(look, "rimB"), (double)out->rim_color[2]);
        out->rim_power     = (float)jce_json_number_value(
            jce_json_get(look, "rimPower"), (double)out->rim_power);
        out->rim_intensity = (float)jce_json_number_value(
            jce_json_get(look, "rimIntensity"), (double)out->rim_intensity);
        out->tonemap_op    = (int)jce_json_number_value(
            jce_json_get(look, "tonemapOp"), (double)out->tonemap_op);
        {
            const char *lp = jce_json_string_value(jce_json_get(look, "lutPath"), "");
            if (lp) {
                size_t i = 0;
                for (; lp[i] && i + 1 < sizeof(out->lut_path); i++)
                    out->lut_path[i] = lp[i];
                out->lut_path[i] = '\0';
            }
        }
        out->lut_strength  = (float)jce_json_number_value(
            jce_json_get(look, "lutStrength"), (double)out->lut_strength);
        out->toon_character = jce_json_get_bool(look, "toonCharacter",
                                                out->toon_character);
        out->bloom_knee    = (float)jce_json_number_value(
            jce_json_get(look, "bloomKnee"), (double)out->bloom_knee);
    }

    jce_json_free(root);
    return true;
}

bool jce_render_settings_load_json(const char *vfs_path, JceRenderSettings *out)
{
    if (!vfs_path || !out) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(vfs_path, &sz);
    if (!buf) return false;
    if (sz == 0 || sz > (1u << 20)) { JCE_FREE(buf); return false; }

    bool ok = jce_render_settings_load_json_mem(buf, (size_t)sz, out);
    if (!ok) LOG_WARN(LOG_TAG, "invalid render settings JSON: %s", vfs_path);
    JCE_FREE(buf);
    return ok;
}
