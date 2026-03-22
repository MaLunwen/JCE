/*
 * jce_text.c  Glyph-atlas text rendering implementation.
 *
 * Pipeline: PAK  decompress  SDL_IOStream  TTF_OpenFontIO 
 *           render ASCII glyphs  pack into atlas  bgfx texture.
 *
 * Each character is drawn as a textured quad via jce_draw_textured_rect.
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

struct JceFont {
    JceTexture  atlas;
    uint32_t    atlas_w, atlas_h;
    int         line_height;
    GlyphInfo   glyphs[GLYPH_COUNT];
};

/* -- Atlas construction -------------------------------------------- */

/* Compute atlas dimensions: single row of glyphs, power-of-two width. */
static void compute_atlas_size(const GlyphInfo *glyphs, int line_h,
                               uint32_t *out_w, uint32_t *out_h)
{
    int total_w = 0;
    for (int i = 0; i < GLYPH_COUNT; i++)
        total_w += glyphs[i].w + 1; /* +1 pixel padding */

    /* Round up to next power of two for GPU friendliness. */
    uint32_t w = 64;
    while ((int)w < total_w) w <<= 1;
    if (w > 4096) w = 4096;

    /* Use multiple rows if single row doesn't fit. */
    int rows = 1;
    {
        int x = 0;
        for (int i = 0; i < GLYPH_COUNT; i++) {
            if (x + glyphs[i].w + 1 > (int)w) {
                rows++;
                x = 0;
            }
            x += glyphs[i].w + 1;
        }
    }

    uint32_t h = 1;
    while (h < (uint32_t)(rows * (line_h + 1)))
        h <<= 1;

    *out_w = w;
    *out_h = h;
}

static JceTexture build_atlas(TTF_Font *ttf, GlyphInfo *glyphs,
                              int line_h, uint32_t *aw, uint32_t *ah)
{
    /* First pass: render each glyph, store surfaces and metrics. */
    SDL_Surface *glyph_surfs[GLYPH_COUNT];
    memset(glyph_surfs, 0, sizeof(glyph_surfs));

    SDL_Color white = { 255, 255, 255, 255 };

    for (int i = 0; i < GLYPH_COUNT; i++) {
        char ch[2] = { (char)(GLYPH_FIRST + i), '\0' };
        SDL_Surface *s = TTF_RenderText_Blended(ttf, ch, 0, white);
        if (s) {
            glyph_surfs[i] = s;
            glyphs[i].w = s->w;
            glyphs[i].h = s->h;
        } else {
            glyphs[i].w = 0;
            glyphs[i].h = 0;
        }

        /* Get advance width. */
        int adv = 0;
        TTF_GetGlyphMetrics(ttf, (uint32_t)(GLYPH_FIRST + i),
                            NULL, NULL, NULL, NULL, &adv);
        glyphs[i].advance = adv;
    }

    /* Compute atlas size. */
    compute_atlas_size(glyphs, line_h, aw, ah);

    /* Create RGBA8 atlas surface. */
    SDL_Surface *atlas = SDL_CreateSurface((int)*aw, (int)*ah,
                                           SDL_PIXELFORMAT_RGBA32);
    if (!atlas) {
        LOG_ERROR(LOG_TAG, "SDL_CreateSurface failed: %s", SDL_GetError());
        for (int i = 0; i < GLYPH_COUNT; i++)
            if (glyph_surfs[i]) SDL_DestroySurface(glyph_surfs[i]);
        return JCE_TEXTURE_INVALID;
    }

    /* Clear atlas to transparent. */
    SDL_FillSurfaceRect(atlas, NULL, 0);

    /* Blit glyphs into atlas, computing UVs. */
    int cx = 0, cy = 0;
    for (int i = 0; i < GLYPH_COUNT; i++) {
        if (!glyph_surfs[i] || glyphs[i].w == 0) {
            glyphs[i].u0 = glyphs[i].v0 = 0;
            glyphs[i].u1 = glyphs[i].v1 = 0;
            continue;
        }

        /* Wrap to next row if needed. */
        if (cx + glyphs[i].w + 1 > (int)*aw) {
            cx = 0;
            cy += line_h + 1;
        }

        SDL_Rect dst = { cx, cy, glyphs[i].w, glyphs[i].h };
        SDL_BlitSurface(glyph_surfs[i], NULL, atlas, &dst);

        glyphs[i].u0 = (float)cx / (float)*aw;
        glyphs[i].v0 = (float)cy / (float)*ah;
        glyphs[i].u1 = (float)(cx + glyphs[i].w) / (float)*aw;
        glyphs[i].v1 = (float)(cy + glyphs[i].h) / (float)*ah;

        cx += glyphs[i].w + 1;
        SDL_DestroySurface(glyph_surfs[i]);
        glyph_surfs[i] = NULL;
    }

    /* Upload to bgfx. Handle possible pitch != w*4. */
    JceTexture tex;
    {
        uint32_t expected_pitch = *aw * 4;
        if ((uint32_t)atlas->pitch == expected_pitch) {
            tex = jce_texture_from_rgba(atlas->pixels, *aw, *ah);
        } else {
            /* Copy row-by-row to tightly packed buffer. */
            uint32_t sz = *aw * *ah * 4;
            uint8_t *packed = (uint8_t *)SDL_malloc(sz);
            if (packed) {
                const uint8_t *src = (const uint8_t *)atlas->pixels;
                for (uint32_t row = 0; row < *ah; row++) {
                    memcpy(packed + row * expected_pitch,
                           src + row * atlas->pitch, expected_pitch);
                }
                tex = jce_texture_from_rgba(packed, *aw, *ah);
                SDL_free(packed);
            } else {
                tex = JCE_TEXTURE_INVALID;
            }
        }
    }
    SDL_DestroySurface(atlas);

    /* Clean up any remaining surfaces. */
    for (int i = 0; i < GLYPH_COUNT; i++)
        if (glyph_surfs[i]) SDL_DestroySurface(glyph_surfs[i]);

    return tex;
}

/* -- Public API ----------------------------------------------------- */

JceFont *jce_font_open(const PakArchive *pak, const char *asset_path, float pt_size)
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

    /* Open font from memory.
       closeio=false because we manage the buffer ourselves. */
    SDL_IOStream *io = SDL_IOFromConstMem(buf, (size_t)asset->original_size);
    if (!io) {
        SDL_free(buf);
        return NULL;
    }

    TTF_Font *ttf = TTF_OpenFontIO(io, true, pt_size); /* true = auto-close io */

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

    font->atlas = build_atlas(ttf, font->glyphs, font->line_height,
                              &font->atlas_w, &font->atlas_h);

    TTF_CloseFont(ttf);
    SDL_free(buf);  /* safe now: TTF no longer references the buffer */

    if (!jce_texture_valid(font->atlas)) {
        LOG_ERROR(LOG_TAG, "atlas build failed for %s", asset_path);
        SDL_free(font);
        return NULL;
    }

    LOG_DEBUG(LOG_TAG, "loaded %s (%.0fpt, atlas %ux%u)",
              asset_path, pt_size, font->atlas_w, font->atlas_h);
    return font;
}

void jce_font_close(JceFont *font)
{
    if (!font) return;
    jce_texture_destroy(font->atlas);
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
    for (const char *p = text; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch == '\n') {
            cx = x;
            y += (float)font->line_height * scale;
            continue;
        }
        if (ch < GLYPH_FIRST || ch > GLYPH_LAST)
            ch = '?'; /* fallback for non-ASCII */

        const GlyphInfo *g = &font->glyphs[ch - GLYPH_FIRST];
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

    for (const char *p = text; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch == '\n') {
            if (cx > max_w) max_w = cx;
            cx = 0;
            lines++;
            continue;
        }
        if (ch < GLYPH_FIRST || ch > GLYPH_LAST) ch = '?';
        cx += (float)font->glyphs[ch - GLYPH_FIRST].advance;
    }
    if (cx > max_w) max_w = cx;

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)(lines * font->line_height);
}
