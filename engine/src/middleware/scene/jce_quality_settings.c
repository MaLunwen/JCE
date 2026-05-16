/*
 * jce_quality_settings.c  Registry + JSON I/O for quality levels.
 */

#include <jce/middleware/scene/jce_quality_settings.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

static JceQualityLevel s_levels[JCE_QUALITY_LEVELS_MAX];
static uint32_t        s_active = 0;

void jce_quality_settings_clear(void)
{
    memset(s_levels, 0, sizeof(s_levels));
    s_active = 0;
}

bool jce_quality_register_level(uint32_t idx, const JceQualityLevel *level)
{
    if (idx >= JCE_QUALITY_LEVELS_MAX || !level) return false;
    s_levels[idx] = *level;
    s_levels[idx].active = true;
    return true;
}

const JceQualityLevel *jce_quality_get_level(uint32_t idx)
{
    if (idx >= JCE_QUALITY_LEVELS_MAX) return NULL;
    return s_levels[idx].active ? &s_levels[idx] : NULL;
}

uint32_t jce_quality_level_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < JCE_QUALITY_LEVELS_MAX; ++i)
        if (s_levels[i].active) n++;
    return n;
}

bool jce_quality_set_active(uint32_t idx)
{
    if (idx >= JCE_QUALITY_LEVELS_MAX || !s_levels[idx].active) return false;
    s_active = idx;
    return true;
}

uint32_t jce_quality_get_active(void) { return s_active; }

const JceQualityLevel *jce_quality_active_level(void)
{
    return jce_quality_get_level(s_active);
}

bool jce_quality_settings_load_json(const char *path)
{
    if (!path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_quality_settings_clear();
    s_active = (uint32_t)jce_json_get_number(root, "active", 0);
    uint32_t n = (uint32_t)jce_json_get_number(root, "count", 0);
    if (n > JCE_QUALITY_LEVELS_MAX) n = JCE_QUALITY_LEVELS_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        char key[40];
        JceQualityLevel lv;
        memset(&lv, 0, sizeof(lv));
        snprintf(key, sizeof(key), "q%u_name", (unsigned)i);
        strncpy(lv.name, jce_json_get_string(root, key, "Quality"),
                 JCE_QUALITY_NAME_LEN - 1);
        snprintf(key, sizeof(key), "q%u_shadow_res", (unsigned)i);
        lv.shadow_resolution = (uint32_t)jce_json_get_number(root, key, 1024);
        snprintf(key, sizeof(key), "q%u_shadow_dist", (unsigned)i);
        lv.shadow_distance = (float)jce_json_get_number(root, key, 50.0);
        snprintf(key, sizeof(key), "q%u_aniso", (unsigned)i);
        lv.anisotropic = (uint8_t)jce_json_get_number(root, key, 4);
        snprintf(key, sizeof(key), "q%u_msaa", (unsigned)i);
        lv.msaa = (uint8_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "q%u_vsync", (unsigned)i);
        lv.vsync_mode = (uint8_t)jce_json_get_number(root, key, 1);
        snprintf(key, sizeof(key), "q%u_lod_bias", (unsigned)i);
        lv.lod_bias = (float)jce_json_get_number(root, key, 1.0);
        snprintf(key, sizeof(key), "q%u_max_lod", (unsigned)i);
        lv.maximum_lod_level = (uint8_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "q%u_pix_lights", (unsigned)i);
        lv.pixel_light_count = (uint8_t)jce_json_get_number(root, key, 4);
        snprintf(key, sizeof(key), "q%u_soft_part", (unsigned)i);
        lv.soft_particles = jce_json_get_bool(root, key, false);
        snprintf(key, sizeof(key), "q%u_soft_veg", (unsigned)i);
        lv.soft_vegetation = jce_json_get_bool(root, key, false);
        snprintf(key, sizeof(key), "q%u_render_scale", (unsigned)i);
        lv.render_scale = (float)jce_json_get_number(root, key, 1.0);

        snprintf(key, sizeof(key), "q%u_kw_count", (unsigned)i);
        uint32_t kn = (uint32_t)jce_json_get_number(root, key, 0);
        if (kn > JCE_QUALITY_KEYWORDS_PER_LEVEL)
            kn = JCE_QUALITY_KEYWORDS_PER_LEVEL;
        for (uint32_t k = 0; k < kn; ++k) {
            snprintf(key, sizeof(key), "q%u_kw%u", (unsigned)i, (unsigned)k);
            strncpy(lv.keywords[k], jce_json_get_string(root, key, ""),
                     JCE_QUALITY_KEYWORD_LEN - 1);
        }
        lv.keyword_count = kn;
        jce_quality_register_level(i, &lv);
    }
    jce_json_free(root);
    return true;
}

bool jce_quality_settings_save_json(const char *path)
{
    if (!path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "active", s_active);
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < JCE_QUALITY_LEVELS_MAX; ++i) {
        if (!s_levels[i].active) continue;
        const JceQualityLevel *lv = &s_levels[i];
        char key[40];
        snprintf(key, sizeof(key), "q%u_name", (unsigned)emitted);
        jce_json_set_string(root, key, lv->name);
        snprintf(key, sizeof(key), "q%u_shadow_res", (unsigned)emitted);
        jce_json_set_number(root, key, lv->shadow_resolution);
        snprintf(key, sizeof(key), "q%u_shadow_dist", (unsigned)emitted);
        jce_json_set_number(root, key, lv->shadow_distance);
        snprintf(key, sizeof(key), "q%u_aniso", (unsigned)emitted);
        jce_json_set_number(root, key, lv->anisotropic);
        snprintf(key, sizeof(key), "q%u_msaa", (unsigned)emitted);
        jce_json_set_number(root, key, lv->msaa);
        snprintf(key, sizeof(key), "q%u_vsync", (unsigned)emitted);
        jce_json_set_number(root, key, lv->vsync_mode);
        snprintf(key, sizeof(key), "q%u_lod_bias", (unsigned)emitted);
        jce_json_set_number(root, key, lv->lod_bias);
        snprintf(key, sizeof(key), "q%u_max_lod", (unsigned)emitted);
        jce_json_set_number(root, key, lv->maximum_lod_level);
        snprintf(key, sizeof(key), "q%u_pix_lights", (unsigned)emitted);
        jce_json_set_number(root, key, lv->pixel_light_count);
        snprintf(key, sizeof(key), "q%u_soft_part", (unsigned)emitted);
        jce_json_set_number(root, key, lv->soft_particles ? 1 : 0);
        snprintf(key, sizeof(key), "q%u_soft_veg", (unsigned)emitted);
        jce_json_set_number(root, key, lv->soft_vegetation ? 1 : 0);
        snprintf(key, sizeof(key), "q%u_render_scale", (unsigned)emitted);
        jce_json_set_number(root, key, lv->render_scale);

        snprintf(key, sizeof(key), "q%u_kw_count", (unsigned)emitted);
        jce_json_set_number(root, key, lv->keyword_count);
        for (uint32_t k = 0; k < lv->keyword_count; ++k) {
            snprintf(key, sizeof(key), "q%u_kw%u",
                      (unsigned)emitted, (unsigned)k);
            jce_json_set_string(root, key, lv->keywords[k]);
        }
        emitted++;
    }
    jce_json_set_number(root, "count", emitted);
    return jce_json_write_file(path, root, true, true);
}
