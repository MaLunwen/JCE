/*
 * jce_tilemap.c  Tilemap + tileset asset loaders (pure CPU).
 *
 * Parses the Tile Palette's .tilemap.json and the Sprite Editor's
 * .sprites.json (see jce_tilemap.h for the formats + conventions).
 * Loading mirrors the jce_particles_desc_load_json style: defaults
 * first, tolerant key reads, clamp out-of-range values.
 */

#include <jce/middleware/scene/jce_tilemap.h>

#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "tilemap"

/* ── Asset structs ────────────────────────────────────────────────── */

struct JceTilemapAsset {
    uint32_t  width;
    uint32_t  height;
    uint32_t *cells;             /* row-major width*height; 0 = empty */
    char      sprites_path[256]; /* authored "sprites" key (may be "") */
};

typedef struct {
    int32_t x, y, w, h;          /* pixel rect inside the atlas */
} TilesetRect;

struct JceTilesetAsset {
    char         image_path[256]; /* authored "source" key (may be "") */
    int32_t      source_w;
    int32_t      source_h;
    TilesetRect *rects;
    uint32_t     rect_count;
};

/* ── Tilemap: parse ───────────────────────────────────────────────── */

static JceTilemapAsset *tilemap_from_json(const JceJson *root)
{
    if (!root) return NULL;

    int w = jce_json_get_int(root, "w", 0);
    int h = jce_json_get_int(root, "h", 0);
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    /* Clamp a corrupt/oversized grid instead of failing outright. */
    if (w > 0 && h > 0 && (uint64_t)w * (uint64_t)h > JCE_TILEMAP_MAX_CELLS) {
        LOG_WARN(LOG_TAG, "grid %dx%d exceeds %d cells; clamping height",
                 w, h, (int)JCE_TILEMAP_MAX_CELLS);
        h = JCE_TILEMAP_MAX_CELLS / w;
        if (h < 1) h = 1;
    }

    JceTilemapAsset *t = (JceTilemapAsset *)JCE_CALLOC(1, sizeof(*t));
    if (!t) return NULL;
    t->width  = (uint32_t)w;
    t->height = (uint32_t)h;

    const char *spr = jce_json_get_string(root, "sprites", "");
    if (spr && spr[0]) {
        strncpy(t->sprites_path, spr, sizeof(t->sprites_path) - 1);
        t->sprites_path[sizeof(t->sprites_path) - 1] = '\0';
    }

    size_t n = (size_t)t->width * (size_t)t->height;
    if (n > 0) {
        t->cells = (uint32_t *)JCE_CALLOC(n, sizeof(uint32_t));
        if (!t->cells) { JCE_FREE(t); return NULL; }
        JceJson *arr = jce_json_get(root, "cells");
        if (arr && jce_json_is_array(arr)) {
            int an = jce_json_array_size(arr);
            if ((size_t)an > n) an = (int)n;   /* extra entries: ignored */
            for (int i = 0; i < an; ++i) {
                JceJson *e = jce_json_array_at(arr, i);
                double v = e ? jce_json_number_value(e, 0.0) : 0.0;
                t->cells[i] = v > 0.0 ? (uint32_t)v : 0u; /* <0 → empty */
            }
        }
    }
    return t;
}

JceTilemapAsset *jce_tilemap_load_mem(const char *data, size_t len)
{
    if (!data) return NULL;
    JceJson *root = jce_json_parse(data, len);
    if (!root) {
        LOG_WARN(LOG_TAG, "tilemap JSON parse failed");
        return NULL;
    }
    JceTilemapAsset *t = tilemap_from_json(root);
    jce_json_free(root);
    return t;
}

JceTilemapAsset *jce_tilemap_load_file(const char *path)
{
    if (!path || !path[0]) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN(LOG_TAG, "failed to parse %s", path);
        return NULL;
    }
    JceTilemapAsset *t = tilemap_from_json(root);
    jce_json_free(root);
    return t;
}

/* Decompress a PAK entry into a transient buffer (caller frees). */
static char *pak_read_text(const struct JcePakArchive *pak, const char *vpath,
                           size_t *out_len)
{
    if (!pak || !vpath || !vpath[0]) return NULL;
    const JcePakAsset *a = jce_pak_find(pak, vpath);
    if (!a) return NULL;
    char *buf = (char *)JCE_MALLOC((size_t)a->original_size + 1);
    if (!buf) return NULL;
    size_t n = jce_pak_decompress(a, buf, (size_t)a->original_size);
    if (n == 0) { JCE_FREE(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

JceTilemapAsset *jce_tilemap_load_from_pak(const struct JcePakArchive *pak,
                                           const char *vpath)
{
    size_t len = 0;
    char *buf = pak_read_text(pak, vpath, &len);
    if (!buf) return NULL;
    JceTilemapAsset *t = jce_tilemap_load_mem(buf, len);
    JCE_FREE(buf);
    return t;
}

JceTilemapAsset *jce_tilemap_load(const char *path)
{
    return jce_tilemap_load_file(path);
}

void jce_tilemap_unload(JceTilemapAsset *t)
{
    if (!t) return;
    if (t->cells) JCE_FREE(t->cells);
    JCE_FREE(t);
}

/* ── Tilemap: queries ─────────────────────────────────────────────── */

uint32_t jce_tilemap_width(const JceTilemapAsset *t)
{
    return t ? t->width : 0;
}

uint32_t jce_tilemap_height(const JceTilemapAsset *t)
{
    return t ? t->height : 0;
}

uint32_t jce_tilemap_tile_at(const JceTilemapAsset *t, uint32_t x, uint32_t y)
{
    if (!t || !t->cells || x >= t->width || y >= t->height) return 0;
    return t->cells[y * t->width + x];
}

const char *jce_tilemap_sprites_path(const JceTilemapAsset *t)
{
    return t ? t->sprites_path : "";
}

/* ── Tileset: parse ───────────────────────────────────────────────── */

static JceTilesetAsset *tileset_from_json(const JceJson *root)
{
    if (!root) return NULL;

    JceTilesetAsset *ts = (JceTilesetAsset *)JCE_CALLOC(1, sizeof(*ts));
    if (!ts) return NULL;

    const char *src = jce_json_get_string(root, "source", "");
    if (src && src[0]) {
        strncpy(ts->image_path, src, sizeof(ts->image_path) - 1);
        ts->image_path[sizeof(ts->image_path) - 1] = '\0';
    }
    /* sourceW/H <= 0 is a valid load (the atlas may not have been sized
     * yet); jce_tileset_tile_uv() then yields nothing to render. */
    ts->source_w = jce_json_get_int(root, "sourceW", 0);
    ts->source_h = jce_json_get_int(root, "sourceH", 0);

    JceJson *arr = jce_json_get(root, "rects");
    if (arr && jce_json_is_array(arr)) {
        int n = jce_json_array_size(arr);
        if (n < 0) n = 0;
        if (n > 0) {
            ts->rects = (TilesetRect *)JCE_CALLOC((size_t)n, sizeof(TilesetRect));
            if (!ts->rects) { JCE_FREE(ts); return NULL; }
            for (int i = 0; i < n; ++i) {
                JceJson *o = jce_json_array_at(arr, i);
                TilesetRect *r = &ts->rects[i];
                if (o) {
                    r->x = jce_json_get_int(o, "x", 0);
                    r->y = jce_json_get_int(o, "y", 0);
                    r->w = jce_json_get_int(o, "w", 1);
                    r->h = jce_json_get_int(o, "h", 1);
                }
                if (r->w < 0) r->w = 0;
                if (r->h < 0) r->h = 0;
            }
            ts->rect_count = (uint32_t)n;
        }
    }
    return ts;
}

JceTilesetAsset *jce_tileset_load_mem(const char *data, size_t len)
{
    if (!data) return NULL;
    JceJson *root = jce_json_parse(data, len);
    if (!root) {
        LOG_WARN(LOG_TAG, "tileset JSON parse failed");
        return NULL;
    }
    JceTilesetAsset *ts = tileset_from_json(root);
    jce_json_free(root);
    return ts;
}

JceTilesetAsset *jce_tileset_load_file(const char *path)
{
    if (!path || !path[0]) return NULL;
    JceJson *root = jce_json_parse_file(path);
    if (!root) {
        LOG_WARN(LOG_TAG, "failed to parse %s", path);
        return NULL;
    }
    JceTilesetAsset *ts = tileset_from_json(root);
    jce_json_free(root);
    return ts;
}

JceTilesetAsset *jce_tileset_load_from_pak(const struct JcePakArchive *pak,
                                           const char *vpath)
{
    size_t len = 0;
    char *buf = pak_read_text(pak, vpath, &len);
    if (!buf) return NULL;
    JceTilesetAsset *ts = jce_tileset_load_mem(buf, len);
    JCE_FREE(buf);
    return ts;
}

void jce_tileset_unload(JceTilesetAsset *ts)
{
    if (!ts) return;
    if (ts->rects) JCE_FREE(ts->rects);
    JCE_FREE(ts);
}

/* ── Tileset: queries ─────────────────────────────────────────────── */

const char *jce_tileset_image_path(const JceTilesetAsset *ts)
{
    return ts ? ts->image_path : "";
}

uint32_t jce_tileset_rect_count(const JceTilesetAsset *ts)
{
    return ts ? ts->rect_count : 0;
}

bool jce_tileset_tile_uv(const JceTilesetAsset *ts, uint32_t id,
                         float *u0, float *v0, float *u1, float *v1)
{
    if (!ts || id == 0 || id > ts->rect_count) return false;
    if (ts->source_w <= 0 || ts->source_h <= 0) return false;
    const TilesetRect *r = &ts->rects[id - 1];
    float iw = (float)ts->source_w;
    float ih = (float)ts->source_h;
    if (u0) *u0 = (float)r->x / iw;
    if (v0) *v0 = (float)r->y / ih;
    if (u1) *u1 = (float)(r->x + r->w) / iw;
    if (v1) *v1 = (float)(r->y + r->h) / ih;
    return true;
}

/* ── Solid-rect extraction (greedy merge) ─────────────────────────── */

uint32_t jce_tilemap_solid_rects(const JceTilemapAsset *t,
                                 JceTilemapSolidRect *out, uint32_t max)
{
    if (!t || !t->cells || t->width == 0 || t->height == 0) return 0;
    uint32_t w = t->width, h = t->height;

    /* `consumed` marks cells already swallowed by an emitted rect. */
    uint8_t *consumed = (uint8_t *)JCE_CALLOC((size_t)w * h, 1);
    if (!consumed) return 0;

    uint32_t total = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t idx = y * w + x;
            if (consumed[idx] || t->cells[idx] == 0) continue;

            /* 1. Maximal horizontal run of solid, unconsumed cells. */
            uint32_t rw = 1;
            while (x + rw < w && !consumed[idx + rw] &&
                   t->cells[idx + rw] != 0)
                rw++;

            /* 2. Extend DOWN while the whole [x, x+rw) span of the next
             *    row is solid and unconsumed (equal-width runs only). */
            uint32_t rh = 1;
            while (y + rh < h) {
                uint32_t row = (y + rh) * w + x;
                bool ok = true;
                for (uint32_t k = 0; k < rw; ++k) {
                    if (consumed[row + k] || t->cells[row + k] == 0) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
                rh++;
            }

            for (uint32_t ry = 0; ry < rh; ++ry)
                memset(&consumed[(y + ry) * w + x], 1, rw);

            if (out && total < max) {
                out[total].x = (uint16_t)x;
                out[total].y = (uint16_t)y;
                out[total].w = (uint16_t)(rw > UINT16_MAX ? UINT16_MAX : rw);
                out[total].h = (uint16_t)(rh > UINT16_MAX ? UINT16_MAX : rh);
            }
            total++;   /* counts past `max` so callers can detect truncation */
        }
    }

    JCE_FREE(consumed);
    return total;
}
