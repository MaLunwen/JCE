/*
 * jce_text.c  Glyph-atlas text rendering with Unicode support.
 *
 * Pipeline: PAK -> decompress -> FT_New_Memory_Face
 *           -> FT_Set_Pixel_Sizes -> render glyphs -> pack into atlas
 *           -> bgfx texture.  HarfBuzz handles text shaping.
 *
 * ASCII glyphs (32-126) are stored in a fixed array for fast lookup.
 * Extra codepoints (e.g. CJK) are stored in an open-addressing hash table.
 * Text strings are decoded as UTF-8.
 */

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_pak_loader.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>

#include "jce_renderer_internal.h"
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <ft2build.h>
#include <SDL3/SDL.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>
#include <string.h>

#define LOG_TAG "jce_text"

static FT_Library s_ft_lib = NULL;

static bool ensure_ft_init(void)
{
    if (s_ft_lib) return true;
    FT_Error err = FT_Init_FreeType(&s_ft_lib);
    if (err) {
        LOG_ERROR(LOG_TAG, "FT_Init_FreeType failed: error %d", err);
        return false;
    }
    return true;
}

/* ASCII printable range: 32 (space) to 126 (~). */
#define GLYPH_FIRST  32
#define GLYPH_LAST  126
#define GLYPH_COUNT (GLYPH_LAST - GLYPH_FIRST + 1)

/* Per-glyph metrics stored in the font. */
typedef struct {
    float u0, v0, u1, v1;   /* UV coordinates in atlas */
    int   w, h;              /* glyph pixel dimensions  */
    int   advance;           /* horizontal advance       */
    int   bearing_x;         /* left side bearing        */
    int   bearing_y;         /* top bearing              */
} GlyphInfo;

/* Hash table entry for non-ASCII glyphs. */
typedef struct {
    uint32_t  codepoint;     /* 0 = empty slot */
    GlyphInfo glyph;
} GlyphEntry;

struct JceFont {
    JceTexture  atlas;
    uint32_t    atlas_w, atlas_h;
    int         line_height;
    int         ascender;
    GlyphInfo   ascii[GLYPH_COUNT];   /* fast path: ASCII 32-126 */
    GlyphEntry *extra;                /* hash table for non-ASCII */
    uint32_t    extra_cap;            /* capacity (power of 2)    */
    /* FreeType / HarfBuzz handles kept alive for shaping. */
    FT_Face     ft_face;
    hb_font_t  *hb_font;
    void       *font_data;            /* decompressed TTF kept for FT */
};

/* -- UTF-8 helpers ------------------------------------------------- */

static uint32_t utf8_decode(const char **pp)
{
    return (uint32_t)SDL_StepUTF8(pp, NULL);
}

/* -- Hash table helpers -------------------------------------------- */

static uint32_t next_pow2(uint32_t v)
{
    v--;
    v |= v >> 1;  v |= v >> 2;
    v |= v >> 4;  v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static void glyph_map_insert(GlyphEntry *map, uint32_t cap,
                              uint32_t cp, const GlyphInfo *g)
{
    uint32_t mask = cap - 1;
    uint32_t idx = cp & mask;
    while (map[idx].codepoint != 0)
        idx = (idx + 1) & mask;
    map[idx].codepoint = cp;
    map[idx].glyph     = *g;
}

static const GlyphInfo *glyph_map_find(const GlyphEntry *map, uint32_t cap,
                                        uint32_t cp)
{
    if (!map || cap == 0) return NULL;
    uint32_t mask = cap - 1;
    uint32_t idx = cp & mask;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t slot = (idx + i) & mask;
        if (map[slot].codepoint == 0)  return NULL;
        if (map[slot].codepoint == cp) return &map[slot].glyph;
    }
    return NULL;
}

/* Unified glyph lookup: ASCII fast path + hash table fallback. */
static const GlyphInfo *font_get_glyph(const JceFont *font, uint32_t cp)
{
    if (cp >= GLYPH_FIRST && cp <= GLYPH_LAST)
        return &font->ascii[cp - GLYPH_FIRST];
    return glyph_map_find(font->extra, font->extra_cap, cp);
}

/* -- Atlas construction -------------------------------------------- */

static JceTexture build_atlas(FT_Face face, JceFont *font,
                               const uint32_t *extra_cps, int extra_count)
{
    int total = GLYPH_COUNT + extra_count;

    GlyphInfo *infos = (GlyphInfo *)JCE_CALLOC((size_t)total, sizeof(GlyphInfo));
    if (!infos) return JCE_TEXTURE_INVALID;

    /* Pass 1: render every glyph and collect metrics. */
    /* We need to store each rendered bitmap temporarily. */
    uint8_t **bitmaps = (uint8_t **)JCE_CALLOC((size_t)total, sizeof(uint8_t *));
    if (!bitmaps) { JCE_FREE(infos); return JCE_TEXTURE_INVALID; }

    int max_glyph_h = 0;  /* track tallest glyph for atlas row height */

    for (int i = 0; i < total; i++) {
        uint32_t cp = (i < GLYPH_COUNT) ? (uint32_t)(GLYPH_FIRST + i)
                                         : extra_cps[i - GLYPH_COUNT];

        FT_Error err = FT_Load_Char(face, cp, FT_LOAD_RENDER);
        if (err) continue;

        FT_GlyphSlot slot = face->glyph;
        int bw = (int)slot->bitmap.width;
        int bh = (int)slot->bitmap.rows;

        infos[i].w         = bw;
        infos[i].h         = bh;
        infos[i].advance   = (int)(slot->advance.x >> 6);
        infos[i].bearing_x = slot->bitmap_left;
        infos[i].bearing_y = slot->bitmap_top;

        if (bh > max_glyph_h) max_glyph_h = bh;

        if (bw > 0 && bh > 0) {
            size_t sz = (size_t)(bw * bh);
            bitmaps[i] = (uint8_t *)JCE_MALLOC(sz);
            if (bitmaps[i]) {
                /* FreeType bitmap may have padding; copy row by row. */
                for (int r = 0; r < bh; r++) {
                    memcpy(bitmaps[i] + r * bw,
                           slot->bitmap.buffer + r * (int)slot->bitmap.pitch,
                           (size_t)bw);
                }
            }
        }
    }

    /* Pass 2: compute atlas dimensions.
     * Only count glyphs that produced a bitmap so the row simulation
     * matches the blitting pass exactly (whitespace/missing glyphs have
     * w == 0 and are skipped during blitting). */
    int total_w = 0;
    for (int i = 0; i < total; i++)
        if (infos[i].w > 0)
            total_w += infos[i].w + 1;

    uint32_t aw = 64;
    while ((int)aw < total_w) aw <<= 1;
    if (aw > 4096) aw = 4096;

    int line_h = font->line_height;
    /* Use the tallest glyph height to size atlas rows; some glyphs
     * (especially CJK) can exceed the font's line_height metric. */
    int row_h = (max_glyph_h > line_h) ? max_glyph_h : line_h;

    int rows = 1, cx = 0;
    for (int i = 0; i < total; i++) {
        if (infos[i].w == 0) continue;   /* skip zero-width (matches blit pass) */
        if (cx + infos[i].w + 1 > (int)aw) { rows++; cx = 0; }
        cx += infos[i].w + 1;
    }

    uint32_t ah = 1;
    while (ah < (uint32_t)(rows * (row_h + 1)))
        ah <<= 1;

    /* Pass 3: create atlas pixel buffer (RGBA). */
    uint32_t atlas_size = aw * ah * 4;
    uint8_t *atlas_pixels = (uint8_t *)JCE_CALLOC(1, atlas_size);
    if (!atlas_pixels) {
        LOG_ERROR(LOG_TAG, "atlas allocation failed (%ux%u)", aw, ah);
        goto cleanup;
    }

    /* Allocate extra-glyph hash table. */
    if (extra_count > 0) {
        font->extra_cap = next_pow2((uint32_t)(extra_count * 2));
        if (font->extra_cap < 16) font->extra_cap = 16;
        font->extra = (GlyphEntry *)JCE_CALLOC(font->extra_cap,
                                                sizeof(GlyphEntry));
    }

    /* Pass 4: blit glyphs into atlas, compute UVs, distribute to storage. */
    cx = 0;
    {
        int cy = 0;
        for (int i = 0; i < total; i++) {
            GlyphInfo *g = &infos[i];

            if (bitmaps[i] && g->w > 0 && g->h > 0) {
                if (cx + g->w + 1 > (int)aw) { cx = 0; cy += row_h + 1; }

                /* Blit grayscale glyph bitmap to RGBA atlas as white + alpha. */
                for (int gy = 0; gy < g->h; gy++) {
                    for (int gx = 0; gx < g->w; gx++) {
                        uint8_t alpha = bitmaps[i][gy * g->w + gx];
                        uint32_t offset = ((uint32_t)(cy + gy) * aw + (uint32_t)(cx + gx)) * 4;
                        atlas_pixels[offset + 0] = 255; /* R */
                        atlas_pixels[offset + 1] = 255; /* G */
                        atlas_pixels[offset + 2] = 255; /* B */
                        atlas_pixels[offset + 3] = alpha;
                    }
                }

                g->u0 = (float)cx            / (float)aw;
                g->v0 = (float)cy            / (float)ah;
                g->u1 = (float)(cx + g->w)   / (float)aw;
                g->v1 = (float)(cy + g->h)   / (float)ah;

                cx += g->w + 1;
            }

            if (i < GLYPH_COUNT) {
                font->ascii[i] = *g;
            } else if (font->extra) {
                uint32_t cp = extra_cps[i - GLYPH_COUNT];
                glyph_map_insert(font->extra, font->extra_cap, cp, g);
            }
        }
    }

    /* Pass 5: upload atlas to bgfx. */
    JceTexture tex = jce_texture_from_rgba(atlas_pixels, aw, ah);
    JCE_FREE(atlas_pixels);

    font->atlas_w = aw;
    font->atlas_h = ah;

    for (int i = 0; i < total; i++) JCE_FREE(bitmaps[i]);
    JCE_FREE(bitmaps);
    JCE_FREE(infos);
    return tex;

cleanup:
    for (int i = 0; i < total; i++) JCE_FREE(bitmaps[i]);
    JCE_FREE(bitmaps);
    JCE_FREE(infos);
    return JCE_TEXTURE_INVALID;
}

/* -- Public API ----------------------------------------------------- */

JceFont *jce_font_open_ex(const JcePakArchive *pak, const char *asset_path,
                           float pt_size,
                           const uint32_t *extra_cps, int extra_count)
{
    if (!pak || !asset_path) return NULL;
    if (!ensure_ft_init()) return NULL;

    LOG_DEBUG(LOG_TAG, "opening font: %s (%.0fpt, +%d extra)", asset_path,
              pt_size, extra_count > 0 ? extra_count : 0);

    const JcePakAsset *asset = jce_pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "font not found in PAK: %s", asset_path);
        return NULL;
    }

    /* Decompress from PAK. */
    LOG_DEBUG(LOG_TAG, "decompressing %s (%llu bytes)", asset_path,
              (unsigned long long)asset->original_size);
    void *buf = JCE_MALLOC((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = jce_pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        JCE_FREE(buf);
        return NULL;
    }

    /* Open font from memory via FreeType. */
    LOG_DEBUG(LOG_TAG, "FT_New_Memory_Face: %s (%zu bytes)", asset_path, n);
    FT_Face face;
    FT_Error err = FT_New_Memory_Face(s_ft_lib,
                                       (const FT_Byte *)buf,
                                       (FT_Long)asset->original_size,
                                       0, &face);
    if (err) {
        LOG_ERROR(LOG_TAG, "FT_New_Memory_Face failed for %s: error %d",
                  asset_path, err);
        JCE_FREE(buf);
        return NULL;
    }

    /* Set pixel size from point size (approximate: 1pt ≈ 1px at 72 DPI). */
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)pt_size);

    JceFont *font = (JceFont *)JCE_CALLOC(1, sizeof(*font));
    if (!font) {
        FT_Done_Face(face);
        JCE_FREE(buf);
        return NULL;
    }

    font->ft_face   = face;
    font->font_data = buf; /* keep alive — FreeType references it */
    font->line_height = (int)(face->size->metrics.height >> 6);
    font->ascender    = (int)(face->size->metrics.ascender >> 6);

    /* Create HarfBuzz font for shaping. */
    LOG_DEBUG(LOG_TAG, "hb_ft_font_create_referenced: %s", asset_path);
    font->hb_font = hb_ft_font_create_referenced(face);

    LOG_DEBUG(LOG_TAG, "building atlas: %s", asset_path);
    font->atlas = build_atlas(face, font,
                               extra_cps, extra_count < 0 ? 0 : extra_count);

    if (!jce_texture_valid(font->atlas)) {
        LOG_ERROR(LOG_TAG, "atlas build failed for %s", asset_path);
        hb_font_destroy(font->hb_font);
        FT_Done_Face(face);
        JCE_FREE(font->extra);
        JCE_FREE(buf);
        JCE_FREE(font);
        return NULL;
    }

    LOG_DEBUG(LOG_TAG, "loaded %s (%.0fpt, atlas %ux%u, +%d extra glyphs)",
              asset_path, pt_size, font->atlas_w, font->atlas_h,
              extra_count > 0 ? extra_count : 0);
    return font;
}

JceFont *jce_font_open(const JcePakArchive *pak, const char *asset_path,
                        float pt_size)
{
    return jce_font_open_ex(pak, asset_path, pt_size, NULL, 0);
}

void jce_font_close(JceFont *font)
{
    if (!font) return;
    jce_texture_destroy(font->atlas);
    if (font->hb_font)  hb_font_destroy(font->hb_font);
    if (font->ft_face)   FT_Done_Face(font->ft_face);
    JCE_FREE(font->extra);
    JCE_FREE(font->font_data);
    JCE_FREE(font);
}

void jce_text_shutdown(void)
{
    if (s_ft_lib) {
        FT_Done_FreeType(s_ft_lib);
        s_ft_lib = NULL;
    }
}

void jce_text_draw(const JceRenderer *r, JceFont *font,
                   float x, float y,
                   const char *text, uint32_t color)
{
    jce_text_draw_scaled(r, font, x, y, 1.0f, text, color);
}

void jce_text_draw_scaled(const JceRenderer *r, JceFont *font,
                          float x, float y, float scale,
                          const char *text, uint32_t color)
{
    if (!r || !font || !text || !font->hb_font) return;
    if (!jce_texture_valid(font->atlas)) return;

    /* Use HarfBuzz for text shaping. */
    hb_buffer_t *hb_buf = hb_buffer_create();
    hb_buffer_add_utf8(hb_buf, text, -1, 0, -1);
    hb_buffer_guess_segment_properties(hb_buf);
    hb_shape(font->hb_font, hb_buf, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_info_t     *glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    hb_glyph_position_t *glyph_pos  = hb_buffer_get_glyph_positions(hb_buf, &glyph_count);

    float cx = x;
    float cy = y;
    for (unsigned int i = 0; i < glyph_count; i++) {
        /* HarfBuzz gives us glyph IDs — we need to map back to codepoints.
           For our atlas, we index by Unicode codepoint. Use the cluster value
           which maps back to the original UTF-8 byte offset. */
        uint32_t cluster = glyph_info[i].cluster;
        const char *p = text + cluster;
        uint32_t ucp = utf8_decode(&p);

        if (ucp == '\n') {
            cx = x;
            cy += (float)font->line_height * scale;
            continue;
        }

        float x_offset  = (float)(glyph_pos[i].x_offset >> 6) * scale;
        float y_offset  = (float)(glyph_pos[i].y_offset >> 6) * scale;
        float x_advance = (float)(glyph_pos[i].x_advance >> 6) * scale;

        const GlyphInfo *g = font_get_glyph(font, ucp);
        if (!g) g = font_get_glyph(font, '?');

        if (g && g->w > 0 && g->h > 0) {
            float gx = cx + x_offset + (float)g->bearing_x * scale;
            float gy = cy + y_offset + (float)(font->ascender - g->bearing_y) * scale;
            const float uv[4] = { g->u0, g->v0, g->u1, g->v1 };
            jce_draw_textured_rect(r, gx, gy,
                                   (float)g->w * scale,
                                   (float)g->h * scale,
                                   font->atlas, color, uv);
        }
        cx += x_advance;
    }

    hb_buffer_destroy(hb_buf);
}

int jce_font_line_height(const JceFont *font)
{
    return font ? font->line_height : 0;
}

void jce_text_measure(const JceFont *font, const char *text,
                      float *out_w, float *out_h)
{
    if (!font || !text || !font->hb_font) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    /* Use HarfBuzz for accurate measurement. */
    hb_buffer_t *hb_buf = hb_buffer_create();
    hb_buffer_add_utf8(hb_buf, text, -1, 0, -1);
    hb_buffer_guess_segment_properties(hb_buf);
    hb_shape(font->hb_font, hb_buf, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_info_t     *glyph_info = hb_buffer_get_glyph_infos(hb_buf, &glyph_count);
    hb_glyph_position_t *glyph_pos  = hb_buffer_get_glyph_positions(hb_buf, &glyph_count);

    float max_w = 0, cx = 0;
    int lines = 1;

    for (unsigned int i = 0; i < glyph_count; i++) {
        uint32_t cluster = glyph_info[i].cluster;
        const char *p = text + cluster;
        uint32_t ucp = utf8_decode(&p);

        if (ucp == '\n') {
            if (cx > max_w) max_w = cx;
            cx = 0;
            lines++;
            continue;
        }
        cx += (float)(glyph_pos[i].x_advance >> 6);
    }
    if (cx > max_w) max_w = cx;

    hb_buffer_destroy(hb_buf);

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)(lines * font->line_height);
}
