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
    return s;
}

bool jce_render_settings_save_json(const char *vfs_path, const JceRenderSettings *s)
{
    if (!vfs_path || !s) return false;

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "$schema", "jce.rendersettings.v1");
    jce_json_set_int(root, "shadowQuality",   s->shadow_quality);
    jce_json_set_int(root, "shadowMapSize",   s->shadow_map_size);
    jce_json_set_int(root, "shadowCascades",  s->shadow_cascades);
    jce_json_set_number(root, "shadowDistance", (double)s->shadow_distance);
    jce_json_set_number(root, "lodBias",        (double)s->lod_bias);
    jce_json_set_int(root, "vsync",           s->vsync);
    jce_json_set_int(root, "msaa",            s->msaa);

    char *json_str = jce_json_print(root, true);
    jce_json_free(root);
    if (!json_str) return false;

    bool ok = jce_fs_host_write_all(vfs_path, json_str, strlen(json_str));
    jce_json_free_string(json_str);
    if (!ok) LOG_WARN(LOG_TAG, "cannot write render settings: %s", vfs_path);
    return ok;
}

bool jce_render_settings_load_json(const char *vfs_path, JceRenderSettings *out)
{
    if (!vfs_path || !out) return false;

    uint64_t sz = 0;
    char *buf = (char *)jce_fs_host_read_all(vfs_path, &sz);
    if (!buf) return false;
    if (sz == 0 || sz > (1u << 20)) { JCE_FREE(buf); return false; }

    JceJson *root = jce_json_parse(buf, (size_t)sz);
    JCE_FREE(buf);
    if (!root) {
        LOG_WARN(LOG_TAG, "invalid render settings JSON: %s", vfs_path);
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

    jce_json_free(root);
    return true;
}
