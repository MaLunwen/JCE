/*
 * jce_sprite_atlas.c  Atlas metadata + 9-slice geometry helper.
 */

#include <jce/renderer/jce_sprite_atlas.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

void jce_sprite_atlas_init(JceSpriteAtlas *a, int w, int h,
                            const char *texture_path)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
    a->atlas_width  = w;
    a->atlas_height = h;
    if (texture_path) {
        strncpy(a->texture_path, texture_path, sizeof(a->texture_path) - 1);
        a->texture_path[sizeof(a->texture_path) - 1] = '\0';
    }
}

static void compute_uv(const JceSpriteAtlas *a, JceSpriteEntry *e)
{
    if (a->atlas_width <= 0 || a->atlas_height <= 0) return;
    e->u0 = (float)e->x                / (float)a->atlas_width;
    e->v0 = (float)e->y                / (float)a->atlas_height;
    e->u1 = (float)(e->x + e->w)       / (float)a->atlas_width;
    e->v1 = (float)(e->y + e->h)       / (float)a->atlas_height;
}

uint32_t jce_sprite_atlas_add(JceSpriteAtlas *a, const char *name,
                               int x, int y, int w, int h)
{
    if (!a || !name) return UINT32_MAX;
    if (a->sprite_count >= JCE_SPRITE_ATLAS_MAX_SPRITES) return UINT32_MAX;
    JceSpriteEntry *e = &a->sprites[a->sprite_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, JCE_SPRITE_ATLAS_NAME_LEN - 1);
    e->name[JCE_SPRITE_ATLAS_NAME_LEN - 1] = '\0';
    e->x = x; e->y = y; e->w = w; e->h = h;
    e->pivot[0] = 0.5f; e->pivot[1] = 0.5f;
    e->active = true;
    compute_uv(a, e);
    return a->sprite_count++;
}

bool jce_sprite_atlas_set_slice(JceSpriteAtlas *a, uint32_t idx,
                                  const int border[4], const float pivot[2])
{
    if (!a || idx >= a->sprite_count) return false;
    JceSpriteEntry *e = &a->sprites[idx];
    if (border) {
        e->border[0] = border[0]; e->border[1] = border[1];
        e->border[2] = border[2]; e->border[3] = border[3];
    }
    if (pivot) {
        e->pivot[0] = pivot[0]; e->pivot[1] = pivot[1];
    }
    return true;
}

uint32_t jce_sprite_atlas_grid(JceSpriteAtlas *a, int cols, int rows,
                                int cw, int ch, int ox, int oy,
                                const char *prefix)
{
    if (!a || cols <= 0 || rows <= 0) return 0;
    a->sprite_count = 0;
    memset(a->sprites, 0, sizeof(a->sprites));
    uint32_t added = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            char name[JCE_SPRITE_ATLAS_NAME_LEN];
            snprintf(name, sizeof(name), "%s_%03d",
                     prefix ? prefix : "cell", r * cols + c);
            if (jce_sprite_atlas_add(a, name,
                                      ox + c * cw, oy + r * ch,
                                      cw, ch) == UINT32_MAX)
                return added;
            added++;
        }
    }
    return added;
}

const JceSpriteEntry *jce_sprite_atlas_find(const JceSpriteAtlas *a,
                                              const char *name)
{
    if (!a || !name) return NULL;
    for (uint32_t i = 0; i < a->sprite_count; ++i) {
        if (a->sprites[i].active &&
            strncmp(a->sprites[i].name, name, JCE_SPRITE_ATLAS_NAME_LEN) == 0)
            return &a->sprites[i];
    }
    return NULL;
}

const JceSpriteEntry *jce_sprite_atlas_at(const JceSpriteAtlas *a, uint32_t i)
{
    if (!a || i >= a->sprite_count || !a->sprites[i].active) return NULL;
    return &a->sprites[i];
}

uint32_t jce_sprite_atlas_emit_9slice(const JceSpriteEntry *e,
                                        int atlas_w, int atlas_h,
                                        float dst_x, float dst_y,
                                        float dst_w, float dst_h,
                                        float *out_positions,
                                        float *out_uvs)
{
    if (!e || !out_positions || !out_uvs) return 0;
    int bl = e->border[0], bt = e->border[1];
    int br = e->border[2], bb = e->border[3];
    bool sliced = (bl | bt | br | bb) != 0;
    if (!sliced) {
        /* Emit a single quad covering the destination rect. */
        out_positions[0] = dst_x;          out_positions[1] = dst_y;
        out_positions[2] = dst_x + dst_w;  out_positions[3] = dst_y;
        out_positions[4] = dst_x + dst_w;  out_positions[5] = dst_y + dst_h;
        out_positions[6] = dst_x;          out_positions[7] = dst_y + dst_h;
        out_uvs[0] = e->u0; out_uvs[1] = e->v0;
        out_uvs[2] = e->u1; out_uvs[3] = e->v0;
        out_uvs[4] = e->u1; out_uvs[5] = e->v1;
        out_uvs[6] = e->u0; out_uvs[7] = e->v1;
        return 1;
    }
    /* 9 quads: corners stay at pixel border size, edges + centre stretch. */
    float xs[4] = { dst_x,
                    dst_x + (float)bl,
                    dst_x + dst_w - (float)br,
                    dst_x + dst_w };
    float ys[4] = { dst_y,
                    dst_y + (float)bt,
                    dst_y + dst_h - (float)bb,
                    dst_y + dst_h };
    /* UV slices match the pixel borders. */
    float us[4] = { e->u0,
                    e->u0 + (float)bl / (float)atlas_w,
                    e->u1 - (float)br / (float)atlas_w,
                    e->u1 };
    float vs[4] = { e->v0,
                    e->v0 + (float)bt / (float)atlas_h,
                    e->v1 - (float)bb / (float)atlas_h,
                    e->v1 };
    uint32_t q = 0;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            float *p = out_positions + q * 8;
            float *u = out_uvs       + q * 8;
            p[0] = xs[i];     p[1] = ys[j];
            p[2] = xs[i + 1]; p[3] = ys[j];
            p[4] = xs[i + 1]; p[5] = ys[j + 1];
            p[6] = xs[i];     p[7] = ys[j + 1];
            u[0] = us[i];     u[1] = vs[j];
            u[2] = us[i + 1]; u[3] = vs[j];
            u[4] = us[i + 1]; u[5] = vs[j + 1];
            u[6] = us[i];     u[7] = vs[j + 1];
            q++;
        }
    }
    return q;
}

/* ── JSON I/O ────────────────────────────────────────────────── */

bool jce_sprite_atlas_save_json(const JceSpriteAtlas *a, const char *path)
{
    if (!a || !path) return false;
    JceJson *root = jce_json_object();
    if (!root) return false;
    jce_json_set_number(root, "atlas_w",   a->atlas_width);
    jce_json_set_number(root, "atlas_h",   a->atlas_height);
    jce_json_set_string(root, "texture",   a->texture_path);
    jce_json_set_number(root, "count",     a->sprite_count);
    for (uint32_t i = 0; i < a->sprite_count; ++i) {
        const JceSpriteEntry *e = &a->sprites[i];
        if (!e->active) continue;
        char key[40];
        snprintf(key, sizeof(key), "s%u_name", (unsigned)i);
        jce_json_set_string(root, key, e->name);
        snprintf(key, sizeof(key), "s%u_x", (unsigned)i);
        jce_json_set_number(root, key, e->x);
        snprintf(key, sizeof(key), "s%u_y", (unsigned)i);
        jce_json_set_number(root, key, e->y);
        snprintf(key, sizeof(key), "s%u_w", (unsigned)i);
        jce_json_set_number(root, key, e->w);
        snprintf(key, sizeof(key), "s%u_h", (unsigned)i);
        jce_json_set_number(root, key, e->h);
        for (int k = 0; k < 4; ++k) {
            snprintf(key, sizeof(key), "s%u_b%d", (unsigned)i, k);
            jce_json_set_number(root, key, e->border[k]);
        }
        snprintf(key, sizeof(key), "s%u_px", (unsigned)i);
        jce_json_set_number(root, key, e->pivot[0]);
        snprintf(key, sizeof(key), "s%u_py", (unsigned)i);
        jce_json_set_number(root, key, e->pivot[1]);
    }
    return jce_json_write_file(path, root, true, true);
}

bool jce_sprite_atlas_load_json(JceSpriteAtlas *a, const char *path)
{
    if (!a || !path) return false;
    JceJson *root = jce_json_parse_file(path);
    if (!root) return false;
    int w = (int)jce_json_get_number(root, "atlas_w", 0);
    int h = (int)jce_json_get_number(root, "atlas_h", 0);
    const char *tex = jce_json_get_string(root, "texture", "");
    jce_sprite_atlas_init(a, w, h, tex);
    uint32_t count = (uint32_t)jce_json_get_number(root, "count", 0);
    if (count > JCE_SPRITE_ATLAS_MAX_SPRITES)
        count = JCE_SPRITE_ATLAS_MAX_SPRITES;
    for (uint32_t i = 0; i < count; ++i) {
        char key[40];
        snprintf(key, sizeof(key), "s%u_name", (unsigned)i);
        const char *nm = jce_json_get_string(root, key, "");
        if (!nm[0]) continue;
        snprintf(key, sizeof(key), "s%u_x", (unsigned)i);
        int sx = (int)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "s%u_y", (unsigned)i);
        int sy = (int)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "s%u_w", (unsigned)i);
        int sw = (int)jce_json_get_number(root, key, 0);
        snprintf(key, sizeof(key), "s%u_h", (unsigned)i);
        int sh = (int)jce_json_get_number(root, key, 0);
        uint32_t idx = jce_sprite_atlas_add(a, nm, sx, sy, sw, sh);
        if (idx == UINT32_MAX) break;
        int border[4];
        for (int k = 0; k < 4; ++k) {
            snprintf(key, sizeof(key), "s%u_b%d", (unsigned)i, k);
            border[k] = (int)jce_json_get_number(root, key, 0);
        }
        float pivot[2];
        snprintf(key, sizeof(key), "s%u_px", (unsigned)i);
        pivot[0] = (float)jce_json_get_number(root, key, 0.5);
        snprintf(key, sizeof(key), "s%u_py", (unsigned)i);
        pivot[1] = (float)jce_json_get_number(root, key, 0.5);
        jce_sprite_atlas_set_slice(a, idx, border, pivot);
    }
    jce_json_free(root);
    return true;
}
