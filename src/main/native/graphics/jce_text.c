/*
 * jce_text.c  Glyph-atlas text rendering with Unicode support.
 *
 * Pipeline: PAK -> decompress -> SDL_IOStream -> TTF_OpenFontIO
 *           -> render glyphs -> pack into atlas -> bgfx texture.
 *
 * ASCII glyphs (32-126) are stored in a fixed array for fast lookup.
 * Extra codepoints (e.g. CJK) are stored in an open-addressing hash table.
 * Text strings are decoded as UTF-8.
 */

#include "jce_text.h"
#include "jce_texture.h"
#include "jce_primitives.h"
#include "jce_renderer_internal.h"
#include "resource/pak_loader.h"
#include "foundation/jce_log.h"

#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string.h>

#define LOG_TAG "jce_text"

static bool s_ttf_inited = false;

static bool ensure_ttf_init(void)
{
    if (s_ttf_inited) return true;
    if (!TTF_Init()) {
        LOG_ERROR(LOG_TAG, "TTF_Init failed: %s", SDL_GetError());
        return false;
    }
    s_ttf_inited = true;
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
    GlyphInfo   ascii[GLYPH_COUNT];   /* fast path: ASCII 32-126 */
    GlyphEntry *extra;                /* hash table for non-ASCII */
    uint32_t    extra_cap;            /* capacity (power of 2)    */
};

/* -- UTF-8 helpers ------------------------------------------------- */

static uint32_t utf8_decode(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0]; *pp += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *pp += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80
                                       && (s[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12)
           | ((uint32_t)(s[1] & 0x3F) << 6)
           | (s[2] & 0x3F);
        *pp += 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80
                                       && (s[2] & 0xC0) == 0x80
                                       && (s[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(s[0] & 0x07) << 18)
           | ((uint32_t)(s[1] & 0x3F) << 12)
           | ((uint32_t)(s[2] & 0x3F) << 6)
           | (s[3] & 0x3F);
        *pp += 4;
    } else {
        cp = '?'; *pp += 1; /* malformed */
    }
    return cp;
}

static int utf8_encode(uint32_t cp, char *buf)
{
    if (cp < 0x80) {
        buf[0] = (char)cp; buf[1] = 0;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        buf[2] = 0;
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        buf[3] = 0;
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    buf[4] = 0;
    return 4;
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

static JceTexture build_atlas(TTF_Font *ttf, JceFont *font,
                               const uint32_t *extra_cps, int extra_count)
{
    int total = GLYPH_COUNT + extra_count;

    /* Temp arrays for surfaces and metrics. */
    SDL_Surface **surfs = (SDL_Surface **)SDL_calloc((size_t)total,
                                                      sizeof(SDL_Surface *));
    GlyphInfo *infos = (GlyphInfo *)SDL_calloc((size_t)total, sizeof(GlyphInfo));
    if (!surfs || !infos) {
        SDL_free(surfs); SDL_free(infos);
        return JCE_TEXTURE_INVALID;
    }

    SDL_Color white = { 255, 255, 255, 255 };

    /* Pass 1: render every glyph and collect metrics. */
    for (int i = 0; i < total; i++) {
        uint32_t cp = (i < GLYPH_COUNT) ? (uint32_t)(GLYPH_FIRST + i)
                                         : extra_cps[i - GLYPH_COUNT];

        char utf8[5];
        utf8_encode(cp, utf8);
        SDL_Surface *s = TTF_RenderText_Blended(ttf, utf8, 0, white);
        if (s) {
            surfs[i]    = s;
            infos[i].w  = s->w;
            infos[i].h  = s->h;
        }

        int adv = 0;
        TTF_GetGlyphMetrics(ttf, cp, NULL, NULL, NULL, NULL, &adv);
        infos[i].advance = adv;
    }

    /* Pass 2: compute atlas dimensions. */
    int total_w = 0;
    for (int i = 0; i < total; i++)
        total_w += infos[i].w + 1;

    uint32_t aw = 64;
    while ((int)aw < total_w) aw <<= 1;
    if (aw > 4096) aw = 4096;

    int line_h = font->line_height;
    int rows = 1, cx = 0;
    for (int i = 0; i < total; i++) {
        if (cx + infos[i].w + 1 > (int)aw) { rows++; cx = 0; }
        cx += infos[i].w + 1;
    }

    uint32_t ah = 1;
    while (ah < (uint32_t)(rows * (line_h + 1)))
        ah <<= 1;

    /* Pass 3: create atlas surface. */
    SDL_Surface *atlas = SDL_CreateSurface((int)aw, (int)ah,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!atlas) {
        LOG_ERROR(LOG_TAG, "atlas surface failed: %s", SDL_GetError());
        goto cleanup;
    }
    SDL_FillSurfaceRect(atlas, NULL, 0);

    /* Allocate extra-glyph hash table. */
    if (extra_count > 0) {
        font->extra_cap = next_pow2((uint32_t)(extra_count * 2));
        if (font->extra_cap < 16) font->extra_cap = 16;
        font->extra = (GlyphEntry *)SDL_calloc(font->extra_cap,
                                                sizeof(GlyphEntry));
    }

    /* Pass 4: blit glyphs into atlas, compute UVs, distribute to storage. */
    cx = 0;
    {
        int cy = 0;
        for (int i = 0; i < total; i++) {
            GlyphInfo *g = &infos[i];

            if (surfs[i] && g->w > 0 && g->h > 0) {
                if (cx + g->w + 1 > (int)aw) { cx = 0; cy += line_h + 1; }

                SDL_Rect dst = { cx, cy, g->w, g->h };
                SDL_BlitSurface(surfs[i], NULL, atlas, &dst);

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
    JceTexture tex;
    {
        uint32_t expected_pitch = aw * 4;
        if ((uint32_t)atlas->pitch == expected_pitch) {
            tex = jce_texture_from_rgba(atlas->pixels, aw, ah);
        } else {
            uint32_t sz = aw * ah * 4;
            uint8_t *packed = (uint8_t *)SDL_malloc(sz);
            if (packed) {
                const uint8_t *src = (const uint8_t *)atlas->pixels;
                for (uint32_t row = 0; row < ah; row++)
                    memcpy(packed + row * expected_pitch,
                           src + row * atlas->pitch, expected_pitch);
                tex = jce_texture_from_rgba(packed, aw, ah);
                SDL_free(packed);
            } else {
                tex = JCE_TEXTURE_INVALID;
            }
        }
    }
    SDL_DestroySurface(atlas);

    font->atlas_w = aw;
    font->atlas_h = ah;

    for (int i = 0; i < total; i++)
        if (surfs[i]) SDL_DestroySurface(surfs[i]);
    SDL_free(surfs);
    SDL_free(infos);
    return tex;

cleanup:
    for (int i = 0; i < total; i++)
        if (surfs[i]) SDL_DestroySurface(surfs[i]);
    SDL_free(surfs);
    SDL_free(infos);
    return JCE_TEXTURE_INVALID;
}

/* -- Public API ----------------------------------------------------- */

JceFont *jce_font_open_ex(const PakArchive *pak, const char *asset_path,
                           float pt_size,
                           const uint32_t *extra_cps, int extra_count)
{
    if (!pak || !asset_path) return NULL;
    if (!ensure_ttf_init()) return NULL;

    const PakAsset *asset = pak_find(pak, asset_path);
    if (!asset) {
        LOG_ERROR(LOG_TAG, "font not found in PAK: %s", asset_path);
        return NULL;
    }

    /* Decompress from PAK. */
    void *buf = SDL_malloc((size_t)asset->original_size);
    if (!buf) return NULL;

    size_t n = pak_decompress(asset, buf, (size_t)asset->original_size);
    if (n == 0) {
        LOG_ERROR(LOG_TAG, "decompression failed: %s", asset_path);
        SDL_free(buf);
        return NULL;
    }

    /* Open font from memory. */
    SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
    if (!io) { SDL_free(buf); return NULL; }

    TTF_Font *ttf = TTF_OpenFontIO(io, true, pt_size);
    if (!ttf) {
        LOG_ERROR(LOG_TAG, "TTF_OpenFontIO failed for %s: %s",
                  asset_path, SDL_GetError());
        SDL_free(buf);
        return NULL;
    }

    JceFont *font = (JceFont *)SDL_calloc(1, sizeof(*font));
    if (!font) {
        TTF_CloseFont(ttf);
        SDL_free(buf);
        return NULL;
    }

    font->line_height = TTF_GetFontHeight(ttf);

    font->atlas = build_atlas(ttf, font,
                               extra_cps, extra_count < 0 ? 0 : extra_count);

    TTF_CloseFont(ttf);
    SDL_free(buf);

    if (!jce_texture_valid(font->atlas)) {
        LOG_ERROR(LOG_TAG, "atlas build failed for %s", asset_path);
        SDL_free(font->extra);
        SDL_free(font);
        return NULL;
    }

    LOG_DEBUG(LOG_TAG, "loaded %s (%.0fpt, atlas %ux%u, +%d extra glyphs)",
              asset_path, pt_size, font->atlas_w, font->atlas_h,
              extra_count > 0 ? extra_count : 0);
    return font;
}

JceFont *jce_font_open(const PakArchive *pak, const char *asset_path,
                        float pt_size)
{
    return jce_font_open_ex(pak, asset_path, pt_size, NULL, 0);
}

void jce_font_close(JceFont *font)
{
    if (!font) return;
    jce_texture_destroy(font->atlas);
    SDL_free(font->extra);
    SDL_free(font);
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
    if (!r || !font || !text) return;
    if (!jce_texture_valid(font->atlas)) return;

    float cx = x;
    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            cx = x;
            y += (float)font->line_height * scale;
            continue;
        }

        const GlyphInfo *g = font_get_glyph(font, cp);
        if (!g) g = font_get_glyph(font, '?');
        if (!g) { cx += (float)font->line_height * 0.5f * scale; continue; }

        if (g->w > 0 && g->h > 0) {
            const float uv[4] = { g->u0, g->v0, g->u1, g->v1 };
            jce_draw_textured_rect(r, cx, y,
                                   (float)g->w * scale,
                                   (float)g->h * scale,
                                   font->atlas, color, uv);
        }
        cx += (float)g->advance * scale;
    }
}

int jce_font_line_height(const JceFont *font)
{
    return font ? font->line_height : 0;
}

void jce_text_measure(const JceFont *font, const char *text,
                      float *out_w, float *out_h)
{
    if (!font || !text) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    float max_w = 0, cx = 0;
    int lines = 1;

    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_decode(&p);
        if (cp == '\n') {
            if (cx > max_w) max_w = cx;
            cx = 0;
            lines++;
            continue;
        }
        const GlyphInfo *g = font_get_glyph(font, cp);
        if (!g) g = font_get_glyph(font, '?');
        if (g) cx += (float)g->advance;
    }
    if (cx > max_w) max_w = cx;

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)(lines * font->line_height);
}
