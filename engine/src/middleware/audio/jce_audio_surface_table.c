/*
 * jce_audio_surface_table.c  Footstep surface table.
 */

#include <jce/middleware/audio/jce_audio_surface_table.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

static JceSurfaceEntry s_entries[JCE_SURFACE_TABLE_MAX];

void jce_audio_surface_clear(void)
{
    memset(s_entries, 0, sizeof(s_entries));
}

static int find_slot(uint32_t id)
{
    int free_slot = -1;
    for (int i = 0; i < JCE_SURFACE_TABLE_MAX; ++i) {
        if (s_entries[i].active && s_entries[i].surface_id == id) return i;
        if (!s_entries[i].active && free_slot < 0) free_slot = i;
    }
    return free_slot;
}

bool jce_audio_surface_add(uint32_t id, const char *label,
                             const JceSurfaceClip *clips, uint32_t n)
{
    int s = find_slot(id);
    if (s < 0) return false;
    JceSurfaceEntry *e = &s_entries[s];
    memset(e, 0, sizeof(*e));
    e->surface_id = id;
    if (label) strncpy(e->label, label, sizeof(e->label) - 1);
    if (n > JCE_SURFACE_CLIPS_PER_ENTRY) n = JCE_SURFACE_CLIPS_PER_ENTRY;
    for (uint32_t i = 0; i < n; ++i) {
        e->clips[i] = clips[i];
        if (e->clips[i].weight <= 0.0f) e->clips[i].weight = 1.0f;
        if (e->clips[i].pitch_min == 0 && e->clips[i].pitch_max == 0) {
            e->clips[i].pitch_min = e->clips[i].pitch_max = 1.0f;
        }
        if (e->clips[i].volume_min == 0 && e->clips[i].volume_max == 0) {
            e->clips[i].volume_min = e->clips[i].volume_max = 1.0f;
        }
    }
    e->clip_count = n;
    e->active = true;
    return true;
}

const JceSurfaceEntry *jce_audio_surface_get(uint32_t id)
{
    int s = find_slot(id);
    if (s < 0 || !s_entries[s].active) return NULL;
    return &s_entries[s];
}

uint32_t jce_audio_surface_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_SURFACE_TABLE_MAX; ++i)
        if (s_entries[i].active) n++;
    return n;
}

static uint32_t lcg_step(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u;
    return (*s >> 8) & 0xFFFFFF;
}

static float lerp_unit(uint32_t *s, float lo, float hi)
{
    float f = (float)lcg_step(s) / (float)0x1000000;
    return lo + (hi - lo) * f;
}

bool jce_audio_surface_resolve(uint32_t id, uint32_t *rng,
                                 JceSurfaceClip *out_clip,
                                 float *out_pitch, float *out_volume)
{
    const JceSurfaceEntry *e = jce_audio_surface_get(id);
    if (!e || e->clip_count == 0 || !rng) return false;

    float total = 0;
    for (uint32_t i = 0; i < e->clip_count; ++i) total += e->clips[i].weight;
    if (total <= 0) return false;

    float pick = (float)lcg_step(rng) / (float)0x1000000 * total;
    float accum = 0;
    for (uint32_t i = 0; i < e->clip_count; ++i) {
        accum += e->clips[i].weight;
        if (pick <= accum) {
            if (out_clip)   *out_clip   = e->clips[i];
            if (out_pitch)  *out_pitch  = lerp_unit(rng,
                                                     e->clips[i].pitch_min,
                                                     e->clips[i].pitch_max);
            if (out_volume) *out_volume = lerp_unit(rng,
                                                     e->clips[i].volume_min,
                                                     e->clips[i].volume_max);
            return true;
        }
    }
    return false;
}

/* ── JSON I/O ────────────────────────────────────────────────── */

bool jce_audio_surface_load_json(const char *path)
{
    if (!path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    jce_audio_surface_clear();
    uint32_t n = (uint32_t)jce_json_get_number(root, "count", 0);
    if (n > JCE_SURFACE_TABLE_MAX) n = JCE_SURFACE_TABLE_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        char key[40];
        snprintf(key, sizeof(key), "s%u_id", (unsigned)i);
        uint32_t id = (uint32_t)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "s%u_label", (unsigned)i);
        const char *lbl = jce_json_get_string(root, key, "");
        snprintf(key, sizeof(key), "s%u_clip_count", (unsigned)i);
        uint32_t cc = (uint32_t)jce_json_get_number(root, key, 0);
        if (cc > JCE_SURFACE_CLIPS_PER_ENTRY) cc = JCE_SURFACE_CLIPS_PER_ENTRY;
        JceSurfaceClip clips[JCE_SURFACE_CLIPS_PER_ENTRY];
        memset(clips, 0, sizeof(clips));
        for (uint32_t c = 0; c < cc; ++c) {
            snprintf(key, sizeof(key), "s%u_c%u_path", (unsigned)i, (unsigned)c);
            strncpy(clips[c].path, jce_json_get_string(root, key, ""),
                     JCE_SURFACE_CLIP_PATH_LEN - 1);
            snprintf(key, sizeof(key), "s%u_c%u_w", (unsigned)i, (unsigned)c);
            clips[c].weight     = (float)jce_json_get_number(root, key, 1.0);
            snprintf(key, sizeof(key), "s%u_c%u_pmin", (unsigned)i, (unsigned)c);
            clips[c].pitch_min  = (float)jce_json_get_number(root, key, 1.0);
            snprintf(key, sizeof(key), "s%u_c%u_pmax", (unsigned)i, (unsigned)c);
            clips[c].pitch_max  = (float)jce_json_get_number(root, key, 1.0);
            snprintf(key, sizeof(key), "s%u_c%u_vmin", (unsigned)i, (unsigned)c);
            clips[c].volume_min = (float)jce_json_get_number(root, key, 1.0);
            snprintf(key, sizeof(key), "s%u_c%u_vmax", (unsigned)i, (unsigned)c);
            clips[c].volume_max = (float)jce_json_get_number(root, key, 1.0);
        }
        jce_audio_surface_add(id, lbl, clips, cc);
    }
    jce_json_free(root);
    return true;
}

bool jce_audio_surface_save_json(const char *path)
{
    if (!path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    uint32_t written = 0;
    for (int i = 0; i < JCE_SURFACE_TABLE_MAX; ++i) {
        if (!s_entries[i].active) continue;
        const JceSurfaceEntry *e = &s_entries[i];
        char key[40];
        snprintf(key, sizeof(key), "s%u_id", (unsigned)written);
        jce_json_set_number(root, key, e->surface_id);
        snprintf(key, sizeof(key), "s%u_label", (unsigned)written);
        jce_json_set_string(root, key, e->label);
        snprintf(key, sizeof(key), "s%u_clip_count", (unsigned)written);
        jce_json_set_number(root, key, e->clip_count);
        for (uint32_t c = 0; c < e->clip_count; ++c) {
            snprintf(key, sizeof(key), "s%u_c%u_path",
                      (unsigned)written, (unsigned)c);
            jce_json_set_string(root, key, e->clips[c].path);
            snprintf(key, sizeof(key), "s%u_c%u_w",
                      (unsigned)written, (unsigned)c);
            jce_json_set_number(root, key, e->clips[c].weight);
            snprintf(key, sizeof(key), "s%u_c%u_pmin",
                      (unsigned)written, (unsigned)c);
            jce_json_set_number(root, key, e->clips[c].pitch_min);
            snprintf(key, sizeof(key), "s%u_c%u_pmax",
                      (unsigned)written, (unsigned)c);
            jce_json_set_number(root, key, e->clips[c].pitch_max);
            snprintf(key, sizeof(key), "s%u_c%u_vmin",
                      (unsigned)written, (unsigned)c);
            jce_json_set_number(root, key, e->clips[c].volume_min);
            snprintf(key, sizeof(key), "s%u_c%u_vmax",
                      (unsigned)written, (unsigned)c);
            jce_json_set_number(root, key, e->clips[c].volume_max);
        }
        written++;
    }
    jce_json_set_number(root, "count", written);
    return jce_json_write_file(path, root, true, true);
}
