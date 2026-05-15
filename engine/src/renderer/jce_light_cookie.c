/*
 * jce_light_cookie.c  Light cookie binding registry.
 *
 * Fixed-size array keyed by light_id.  64 slots — Unity's typical
 * scene caps near a few dozen distinct cookies; matches that.
 */

#include <jce/renderer/jce_light_cookie.h>

#include <string.h>

static JceLightCookieEntry s_entries[JCE_LIGHT_COOKIE_MAX];

static int find_slot(uint32_t light_id)
{
    if (light_id == 0) return -1;
    int free_slot = -1;
    for (int i = 0; i < JCE_LIGHT_COOKIE_MAX; ++i) {
        if (s_entries[i].active && s_entries[i].light_id == light_id) return i;
        if (!s_entries[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

bool jce_light_cookie_set(uint32_t light_id, JceLightCookieKind kind,
                           const char *texture_path,
                           const float uv_scale[2],
                           const float uv_offset[2],
                           const float tint_rgba[4])
{
    int s = find_slot(light_id);
    if (s < 0) return false;
    JceLightCookieEntry *e = &s_entries[s];
    e->light_id = light_id;
    e->kind     = kind;
    e->active   = true;
    if (texture_path) {
        strncpy(e->texture_path, texture_path,
                JCE_LIGHT_COOKIE_PATH_LEN - 1);
        e->texture_path[JCE_LIGHT_COOKIE_PATH_LEN - 1] = '\0';
    } else {
        e->texture_path[0] = '\0';
    }
    e->texture_handle = 0xFFFFFFFFu;
    if (uv_scale)  { e->uv_scale[0] = uv_scale[0];  e->uv_scale[1] = uv_scale[1]; }
    else           { e->uv_scale[0] = 1.0f;         e->uv_scale[1] = 1.0f; }
    if (uv_offset) { e->uv_offset[0]= uv_offset[0]; e->uv_offset[1]= uv_offset[1]; }
    else           { e->uv_offset[0]= 0.0f;         e->uv_offset[1]= 0.0f; }
    if (tint_rgba) memcpy(e->tint_rgba, tint_rgba, 4 * sizeof(float));
    else { e->tint_rgba[0] = e->tint_rgba[1] = e->tint_rgba[2] = e->tint_rgba[3] = 1.0f; }
    return true;
}

bool jce_light_cookie_clear(uint32_t light_id)
{
    for (int i = 0; i < JCE_LIGHT_COOKIE_MAX; ++i) {
        if (s_entries[i].active && s_entries[i].light_id == light_id) {
            memset(&s_entries[i], 0, sizeof(s_entries[i]));
            return true;
        }
    }
    return false;
}

const JceLightCookieEntry *jce_light_cookie_get(uint32_t light_id)
{
    for (int i = 0; i < JCE_LIGHT_COOKIE_MAX; ++i)
        if (s_entries[i].active && s_entries[i].light_id == light_id)
            return &s_entries[i];
    return NULL;
}

uint32_t jce_light_cookie_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_LIGHT_COOKIE_MAX; ++i)
        if (s_entries[i].active) n++;
    return n;
}

const JceLightCookieEntry *jce_light_cookie_at(uint32_t index)
{
    uint32_t seen = 0;
    for (int i = 0; i < JCE_LIGHT_COOKIE_MAX; ++i) {
        if (!s_entries[i].active) continue;
        if (seen == index) return &s_entries[i];
        seen++;
    }
    return NULL;
}
