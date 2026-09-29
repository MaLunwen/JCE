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

#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/renderer/jce_primitives.h>
#include <jce/renderer/jce_text.h>
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_views.h>

#include "jce_renderer_internal.h"
#include "os/core/jce_memory.h"

#include <bgfx/c99/bgfx.h>
#include <ft2build.h>
#include FT_MODULE_H   /* FT_Property_Set - the SDF renderer spread */
#include "renderer/jce_text_shape.h"

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
    uint8_t dynamic;         /* 0 = static atlas, 1 = dynamic overflow atlas */
} GlyphInfo;

/* Hash table entry for non-ASCII glyphs. */
typedef struct {
    uint32_t  codepoint;     /* 0 = empty slot */
    GlyphInfo glyph;
} GlyphEntry;

/* Dynamic overflow atlas: any codepoint outside the pre-rendered set is
 * rasterized on demand into this second texture (kept alongside a CPU mirror
 * for re-upload) so arbitrary UTF-8 — Greek, math symbols, less-common CJK —
 * renders without pre-baking every glyph.  512x512 RGBA = 1 MiB; a shelf
 * packer fills it left-to-right, top-to-bottom.  The static `atlas` above is
 * never touched, so existing text keeps its battle-tested fast path. */
#define DYN_ATLAS_DIM 512

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
    /* Dynamic overflow atlas (lazily created on first miss). */
    JceTexture  dyn_atlas;
    uint8_t    *dyn_pixels;           /* CPU mirror (DYN_ATLAS_DIM^2 * 4)   */
    GlyphEntry *dyn_map;              /* codepoint -> dynamic GlyphInfo     */
    uint32_t    dyn_cap;              /* dyn_map capacity (power of 2)      */
    uint32_t    dyn_count;            /* live dynamic glyphs                */
    int         dyn_pen_x, dyn_pen_y; /* shelf packer cursor                */
    int         dyn_row_h;            /* current shelf row height           */
    bool        dyn_full;            /* atlas exhausted (stop rasterizing)  */
    /* 0 = the glyph bitmaps hold COVERAGE (what a font has always held here).
     * > 0 = they hold a signed DISTANCE FIELD with this spread in pixels, so
     * one atlas stays crisp at any size instead of softening as it is scaled
     * up.  Captured at open because the atlas is built there; a font cannot
     * change its mind later without rebuilding every glyph. */
    int         sdf_spread;

    /* -- Fallback chain ------------------------------------------------
     *
     * A font that lacks a codepoint rasterises .notdef -- a tofu box -- and
     * the file already said so about U+0020.  For a Latin UI font in front of
     * a Chinese, Japanese or Korean string that is EVERY character, which is
     * the shipping state this chain ends.
     *
     * NOT OWNED.  A fallback is an ordinary JceFont the caller opened and
     * closes; sharing one CJK face across every UI font is the normal case and
     * copying it per font would be a second atlas each time.  jce_font_close
     * therefore does not close them -- it only drops this font's shaped runs,
     * which is what the shape cache keys on. */
    JceFont    *fallback[JCE_FONT_MAX_FALLBACKS];
    uint8_t     fallback_count;
    /* Bumped whenever the chain changes.  The shape cache keys on it: the same
     * bytes shape differently once a fallback can claim some of them, and a
     * run cached before the fallback was added would otherwise be served for
     * the life of the process. */
    uint32_t    chain_generation;
};

/* -- The chain, as the shape cache's two questions ------------------- */

static uint8_t font_slot_for_codepoint(void *ctx, uint32_t cp)
{
    JceFont *f = (JceFont *)ctx;
    if (!f) return 0u;
    /* The PRIMARY wins whenever it can draw the character, so adding a
     * fallback cannot change how any string that already rendered looks. */
    if (f->ft_face && FT_Get_Char_Index(f->ft_face, (FT_ULong)cp) != 0)
        return 0u;
    for (uint8_t i = 0; i < f->fallback_count; i++) {
        JceFont *fb = f->fallback[i];
        if (fb && fb->ft_face &&
            FT_Get_Char_Index(fb->ft_face, (FT_ULong)cp) != 0)
            return (uint8_t)(i + 1u);
    }
    /* Nothing has it.  Slot 0, so the primary draws its own .notdef -- the
     * honest answer, and the behaviour that shipped before there was a chain. */
    return 0u;
}

static hb_font_t *font_hb_for_slot(void *ctx, uint8_t slot)
{
    JceFont *f = (JceFont *)ctx;
    if (!f) return NULL;
    if (slot == 0) return f->hb_font;
    if (slot > f->fallback_count) return NULL;
    JceFont *fb = f->fallback[slot - 1u];
    return fb ? fb->hb_font : NULL;
}

/* The JceFont a shaped glyph's slot names -- the atlas side of the same
 * question.  Used by the draw loop; a draw that ignored it would rasterise
 * every fallback glyph out of the primary's atlas, i.e. tofu with correct
 * spacing, which looks like a font problem rather than a wiring one. */
static JceFont *font_for_slot(JceFont *f, uint8_t slot)
{
    if (!f || slot == 0 || slot > f->fallback_count) return f;
    JceFont *fb = f->fallback[slot - 1u];
    return fb ? fb : f;
}

/* const in, because jce_text_measure is a reader and has a const font.  The
 * cast is safe by inspection and not merely by convention: both callbacks only
 * READ -- FT_Get_Char_Index and a pointer fetch -- and neither can reach the
 * dynamic-atlas rasteriser, which is the one thing on this struct a draw
 * mutates. */
static JceTextFontChain font_chain(const JceFont *f)
{
    JceTextFontChain c;
    c.ctx                = (void *)(uintptr_t)f;
    c.slot_for_codepoint = font_slot_for_codepoint;
    c.font_for_slot      = font_hb_for_slot;
    c.generation         = f ? f->chain_generation : 0u;
    return c;
}

bool jce_font_add_fallback(JceFont *font, JceFont *fallback)
{
    if (!font || !fallback || font == fallback) return false;
    if (font->fallback_count >= JCE_FONT_MAX_FALLBACKS) return false;
    /* One level only, and refused rather than flattened: a chain of chains
     * would make "which font drew this glyph" depend on a walk the shaped run
     * has no room to record, and the slot in JceShapedGlyph is one byte
     * indexing THIS font's list. */
    if (fallback->fallback_count > 0) return false;
    for (uint8_t i = 0; i < font->fallback_count; i++)
        if (font->fallback[i] == fallback) return true;   /* idempotent */
    font->fallback[font->fallback_count++] = fallback;
    font->chain_generation++;
    /* Every run this font has already shaped was itemized against the OLD
     * chain.  Dropping them is not an optimisation to skip: without it a label
     * drawn before the fallback was added keeps its tofu forever. */
    jce_text_shape_forget(font);
    return true;
}


/* Requested for the NEXT font opened, not for fonts already open -- the atlas
 * is rasterised during open.  Same shape as jce_ui_canvas_set_default_font,
 * which this subsystem already uses for a process-scoped authoring choice.
 * 0 restores the bitmap path. */
static int s_next_sdf_spread = 0;

void jce_font_set_sdf_spread(int spread_px)
{
    if (spread_px < 0)  spread_px = 0;
    if (spread_px > 64) spread_px = 64;   /* FreeType's own ceiling */
    s_next_sdf_spread = spread_px;
}

int jce_font_sdf_spread(const JceFont *font)
{
    return font ? font->sdf_spread : 0;
}

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

/* Unified glyph lookup: ASCII fast path + pre-rendered hash table + dynamic
 * overflow atlas.  const: does NOT rasterize a missing glyph (see the mutable
 * font_get_or_render_glyph below for the on-demand path). */
static const GlyphInfo *font_get_glyph(const JceFont *font, uint32_t cp)
{
    if (cp >= GLYPH_FIRST && cp <= GLYPH_LAST)
        return &font->ascii[cp - GLYPH_FIRST];
    const GlyphInfo *g = glyph_map_find(font->extra, font->extra_cap, cp);
    if (g) return g;
    return glyph_map_find(font->dyn_map, font->dyn_cap, cp);
}

/* Load and render ONE glyph, as coverage or as a distance field.
 *
 * ONE helper for both atlas paths on purpose.  If the static ASCII atlas and
 * the dynamic overflow atlas chose their render mode separately, the day they
 * disagreed a codepoint outside ASCII would come out as a grey block on a page
 * of crisp text -- the SDF shader reading coverage texels -- and nothing would
 * say why.
 *
 * FT_LOAD_RENDER is deliberately NOT used here: it renders with the driver's
 * default mode, and the SDF renderer has to be asked for by name. */
static FT_Error text_render_glyph(FT_Face face, uint32_t cp, int spread)
{
    FT_Error err = FT_Load_Char(face, cp, FT_LOAD_DEFAULT);
    if (err) return err;
    return FT_Render_Glyph(face->glyph,
                           spread > 0 ? FT_RENDER_MODE_SDF
                                      : FT_RENDER_MODE_NORMAL);
}

/* -- Dynamic overflow atlas: rasterize any codepoint on demand ------- */

/* Rasterize `cp` via FreeType and pack it into the font's dynamic atlas,
 * creating the atlas on first use.  Re-uploads the CPU mirror after packing
 * (rare — only on the first appearance of each new codepoint).  Returns the
 * cached GlyphInfo (dynamic=1) or NULL if the atlas is full / render failed. */
static const GlyphInfo *font_render_dynamic_glyph(JceFont *font, uint32_t cp)
{
    if (font->dyn_full || !font->ft_face)
        return NULL;

    if (!font->dyn_pixels) {
        font->dyn_pixels = (uint8_t *)JCE_CALLOC(
            1, (size_t)DYN_ATLAS_DIM * DYN_ATLAS_DIM * 4u);
        if (!font->dyn_pixels) { font->dyn_full = true; return NULL; }
        font->dyn_cap = 256;
        font->dyn_map = (GlyphEntry *)JCE_CALLOC(font->dyn_cap,
                                                 sizeof(GlyphEntry));
        if (!font->dyn_map) {
            JCE_FREE(font->dyn_pixels); font->dyn_pixels = NULL;
            font->dyn_full = true; return NULL;
        }
        font->dyn_atlas = jce_texture_from_rgba(font->dyn_pixels,
                                                DYN_ATLAS_DIM, DYN_ATLAS_DIM);
        font->dyn_pen_x = 0; font->dyn_pen_y = 0; font->dyn_row_h = 0;
    }
    if (font->dyn_count + 1 >= font->dyn_cap) { /* keep load factor < 1 */
        font->dyn_full = true; return NULL;
    }

    if (text_render_glyph(font->ft_face, cp, font->sdf_spread) != 0)
        return NULL;
    FT_GlyphSlot slot = font->ft_face->glyph;
    int bw = (int)slot->bitmap.width;
    int bh = (int)slot->bitmap.rows;

    /* Shelf packer: advance to a new row when the current one is full. */
    if (font->dyn_pen_x + bw + 1 > DYN_ATLAS_DIM) {
        font->dyn_pen_y += font->dyn_row_h + 1;
        font->dyn_pen_x = 0;
        font->dyn_row_h = 0;
    }
    if (font->dyn_pen_y + bh + 1 > DYN_ATLAS_DIM) {
        font->dyn_full = true;               /* atlas exhausted */
        LOG_WARN(LOG_TAG, "dynamic glyph atlas full (cp U+%04X)", cp);
        return NULL;
    }

    int gx = font->dyn_pen_x, gy = font->dyn_pen_y;
    for (int r = 0; r < bh; r++) {
        for (int c = 0; c < bw; c++) {
            uint8_t a = slot->bitmap.buffer[r * (int)slot->bitmap.pitch + c];
            uint32_t off = ((uint32_t)(gy + r) * DYN_ATLAS_DIM
                            + (uint32_t)(gx + c)) * 4u;
            font->dyn_pixels[off + 0] = 255;
            font->dyn_pixels[off + 1] = 255;
            font->dyn_pixels[off + 2] = 255;
            font->dyn_pixels[off + 3] = a;
        }
    }
    if (bh > font->dyn_row_h) font->dyn_row_h = bh;
    font->dyn_pen_x += bw + 1;

    GlyphInfo g;
    memset(&g, 0, sizeof(g));
    g.w = bw; g.h = bh;
    g.advance   = (int)(slot->advance.x >> 6);
    g.bearing_x = slot->bitmap_left;
    g.bearing_y = slot->bitmap_top;
    g.dynamic   = 1;
    g.u0 = (float)gx / (float)DYN_ATLAS_DIM;
    g.v0 = (float)gy / (float)DYN_ATLAS_DIM;
    g.u1 = (float)(gx + bw) / (float)DYN_ATLAS_DIM;
    g.v1 = (float)(gy + bh) / (float)DYN_ATLAS_DIM;
    glyph_map_insert(font->dyn_map, font->dyn_cap, cp, &g);
    font->dyn_count++;

    /* Re-upload the CPU mirror (one full 1 MiB update per NEW codepoint —
     * amortizes to zero once a string's glyphs are cached). */
    jce_texture_update_rgba(font->dyn_atlas, font->dyn_pixels,
                            DYN_ATLAS_DIM, DYN_ATLAS_DIM);
    return glyph_map_find(font->dyn_map, font->dyn_cap, cp);
}

/* Glyph lookup that rasterizes on demand: static set first, then the dynamic
 * overflow atlas.  Whitespace / control codepoints never reach here (callers
 * skip them), so a miss means a real glyph we should try to render. */
static const GlyphInfo *font_get_or_render_glyph(JceFont *font, uint32_t cp)
{
    if (cp >= GLYPH_FIRST && cp <= GLYPH_LAST)
        return &font->ascii[cp - GLYPH_FIRST];
    const GlyphInfo *g = glyph_map_find(font->extra, font->extra_cap, cp);
    if (g) return g;
    g = glyph_map_find(font->dyn_map, font->dyn_cap, cp);
    if (g) return g;
    return font_render_dynamic_glyph(font, cp);
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

        FT_Error err = text_render_glyph(face, cp, font->sdf_spread);
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

static JceFont *font_open_from_memory(void *buf, size_t size,
                                      const char *label, float pt_size,
                                      const uint32_t *extra_cps,
                                      int extra_count);

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

    return font_open_from_memory(buf, (size_t)asset->original_size,
                                 asset_path, pt_size,
                                 extra_cps, extra_count);
}

/* Public: open a font from an in-memory TTF/OTF blob (e.g. bytes read from the
   active VFS / a mounted content bundle).  Copies `data` into an engine
   allocation the font then owns (font_open_from_memory + jce_font_close manage
   its lifetime), so the caller's buffer may be freed immediately after. */
JceFont *jce_font_open_mem_ex(const void *data, size_t size, const char *label,
                              float pt_size, const uint32_t *extra_cps,
                              int extra_count)
{
    if (!data || size == 0) return NULL;
    if (!ensure_ft_init()) return NULL;
    void *buf = JCE_MALLOC(size);
    if (!buf) return NULL;
    memcpy(buf, data, size);
    return font_open_from_memory(buf, size, label ? label : "<mem>",
                                 pt_size, extra_cps, extra_count);
}

/* Build a JceFont from an in-memory TTF/OTF blob.  Takes ownership of `buf`
   (engine-internal allocation) in every path — FreeType keeps referencing it
   for the font's lifetime, so it is stored on the font and released by
   jce_font_close. */
static JceFont *font_open_from_memory(void *buf, size_t size,
                                      const char *label, float pt_size,
                                      const uint32_t *extra_cps,
                                      int extra_count)
{
    LOG_DEBUG(LOG_TAG, "FT_New_Memory_Face: %s (%zu bytes)", label, size);
    FT_Face face;
    FT_Error err = FT_New_Memory_Face(s_ft_lib,
                                       (const FT_Byte *)buf,
                                       (FT_Long)size,
                                       0, &face);
    if (err) {
        LOG_ERROR(LOG_TAG, "FT_New_Memory_Face failed for %s: error %d",
                  label, err);
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
    /* Captured HERE, not read at draw time: build_atlas below rasterises with
     * it, so a font that changed its mind later would have an atlas that
     * disagreed with its own shader. */
    font->sdf_spread = s_next_sdf_spread;
    if (font->sdf_spread > 0) {
        /* FreeType s SDF renderer takes its spread as a module property, not
         * a per-call argument, so it is set per open rather than per glyph.
         * A failure here is not fatal: the module keeps its default spread
         * and the glyphs are still a distance field, just a different one. */
        FT_Int spread = (FT_Int)font->sdf_spread;
        FT_Property_Set(s_ft_lib, "sdf", "spread", &spread);
    }
    font->line_height = (int)(face->size->metrics.height >> 6);
    font->ascender    = (int)(face->size->metrics.ascender >> 6);
    /* A zero-initialized bgfx texture handle has idx 0 — which reads as
     * "valid" (idx != UINT16_MAX).  Mark the not-yet-created dynamic atlas
     * invalid so jce_font_close never destroys texture handle 0. */
    font->dyn_atlas.idx = UINT16_MAX;

    /* Create HarfBuzz font for shaping. */
    LOG_DEBUG(LOG_TAG, "hb_ft_font_create_referenced: %s", label);
    font->hb_font = hb_ft_font_create_referenced(face);

    LOG_DEBUG(LOG_TAG, "building atlas: %s", label);
    font->atlas = build_atlas(face, font,
                               extra_cps, extra_count < 0 ? 0 : extra_count);

    if (!jce_texture_valid(font->atlas)) {
        LOG_ERROR(LOG_TAG, "atlas build failed for %s", label);
        hb_font_destroy(font->hb_font);
        FT_Done_Face(face);
        JCE_FREE(font->extra);
        JCE_FREE(buf);
        JCE_FREE(font);
        return NULL;
    }

    LOG_DEBUG(LOG_TAG, "loaded %s (%.0fpt, atlas %ux%u, +%d extra glyphs)",
              label, pt_size, font->atlas_w, font->atlas_h,
              extra_count > 0 ? extra_count : 0);
    return font;
}

JceFont *jce_font_open(const JcePakArchive *pak, const char *asset_path,
                        float pt_size)
{
    return jce_font_open_ex(pak, asset_path, pt_size, NULL, 0);
}

JceFont *jce_font_open_file_ex(const char *host_path, float pt_size,
                               const uint32_t *extra_cps, int extra_count)
{
    if (!host_path || !host_path[0]) return NULL;
    if (!ensure_ft_init()) return NULL;
    uint64_t size = 0;
    void *buf = jce_fs_host_read_all(host_path, &size);
    if (!buf || size == 0) {
        JCE_FREE(buf);
        return NULL;
    }
    LOG_DEBUG(LOG_TAG, "opening host font: %s (%.0fpt)", host_path, pt_size);
    return font_open_from_memory(buf, (size_t)size, host_path, pt_size,
                                 extra_cps, extra_count);
}

void jce_font_close(JceFont *font)
{
    if (!font) return;
    /* The shape cache keys on this pointer.  Leaving its runs behind would
     * hand them to whatever is allocated at this address next. */
    jce_text_shape_forget(font);
    /* The fallbacks are NOT owned -- see the chain's note on JceFont. */
    font->fallback_count = 0;
    jce_texture_destroy(font->atlas);
    if (jce_texture_valid(font->dyn_atlas)) jce_texture_destroy(font->dyn_atlas);
    if (font->hb_font)  hb_font_destroy(font->hb_font);
    if (font->ft_face)   FT_Done_Face(font->ft_face);
    JCE_FREE(font->extra);
    JCE_FREE(font->dyn_map);
    JCE_FREE(font->dyn_pixels);
    JCE_FREE(font->font_data);
    JCE_FREE(font);
}

void jce_text_shutdown(void)
{
    /* Report the shape cache before dropping it.
     *
     * "It caches" is a claim about COUNTS, and the pixels are identical either
     * way -- a cache that silently stopped hitting would look exactly like one
     * that works.  One line at shutdown is the cheapest place the claim can be
     * checked in a real run rather than only in a unit test: every headless
     * capture already keeps the engine's stdout, so the number is in the log
     * beside every measurement. */
    {
        uint64_t hits = 0, misses = 0;
        jce_text_shape_stats(&hits, &misses);
        if (hits || misses)
            LOG_INFO(LOG_TAG,
                     "shape cache: %llu hit(s), %llu HarfBuzz shape(s) "
                     "(%.1f%% served from cache)",
                     (unsigned long long)hits, (unsigned long long)misses,
                     100.0 * (double)hits / (double)(hits + misses));
    }
    jce_text_shape_shutdown();
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
    jce_text_draw_scaled_view(r, font, JCE_VIEW_UI, x, y, scale, text, color);
}

/* The transform in force for the glyph batches of the CURRENT draw.
 *
 * A pointer rather than a value, and NULL when unarmed, so the untransformed
 * path submits through exactly the call it always did -- a line of text is
 * hundreds of glyph quads and "identity applied 400 times" is not the same
 * thing as "not applied".
 *
 * It is file-scope for the same reason jce_model.c's material override is:
 * this function shapes and batches internally and has no per-glyph place to
 * thread a parameter through, and the arming is one assignment either side of
 * one call on the render thread. */
/* Both BORROWED for the duration of one draw call and cleared at its end --
 * the plumbing between the public entry point and the batch flush two frames
 * down, not state anybody sets and leaves.  The style joined the transform
 * here rather than becoming a mode with a setter, which is why there is
 * nothing for the next label to inherit. */
static const JceRectXform *s_text_xf    = NULL;
static const JceTextStyle *s_text_style = NULL;

/* Draw one run of glyph quads with the program that matches how the atlas was
 * rasterised.
 *
 * ONE helper because there are TWO drains -- the mid-run one when the batch
 * fills or the atlas changes, and the final one -- and two sites choosing
 * independently is how the last 128 glyphs of a paragraph end up drawn
 * differently from the rest.
 *
 * THE SMOOTHING WIDTH IS COMPUTED HERE, NOT IN THE SHADER.  The usual
 * formulation takes it from fwidth(), which needs derivatives this engine's
 * ES 2.0 / GL 2.1 floor does not guarantee.  Here the CPU already knows the
 * number.  Working it out rather than guessing, because the first version of
 * this line was out by exactly a factor of two and the measurement said so:
 * the atlas stores distance normalised so the outline is 0.5 and one atlas
 * pixel spans 0.5/spread of it.  smoothstep(edge-w, edge+w, d) transitions
 * over 2w of distance, which is 2w / (0.5/spread) = 4*w*spread atlas pixels,
 * or 4*w*spread*scale SCREEN pixels.  Setting that to one screen pixel gives
 *
 *     w = 0.25 / (spread * scale)
 *
 * 0.5/(spread*scale) -- the value that reads right and is wrong -- spreads the
 * edge over TWO screen pixels, and at 1:1 that measured 2.34x the bitmap
 * path's edge width instead of matching it.
 *
 * Falling back to the coverage program when the SDF one is missing would draw
 * distance texels as alpha -- a grey slab per glyph.  Better to draw nothing
 * and let the missing-shader warning be the explanation. */
static void text_flush_batch(const JceRenderer *r, uint16_t view_id,
                             const JceQuad2D *batch, uint32_t n,
                             JceTexture tex, const JceFont *font, float scale)
{
    const uint32_t aw = font ? font->atlas_w : 0u;
    const uint32_t ah = font ? font->atlas_h : 0u;
    if (n == 0) return;
    const int spread = font ? font->sdf_spread : 0;
    if (spread <= 0) {
        jce_draw_textured_quads_view_xf(r, view_id, batch, n, tex,
                                        s_text_xf);
        return;
    }
    const float px = (scale > 0.0f) ? scale : 1.0f;

    JceTextSdfStyle st;
    memset(&st, 0, sizeof st);
    st.smoothing = 0.25f / ((float)spread * px);
    st.edge      = 0.5f;

    if (s_text_style) {
        /* PIXELS IN, DISTANCE OUT.  An author sets an outline in pixels of the
         * drawn glyph, because that is the only unit they can see; the shader
         * thresholds a normalised distance.  One atlas pixel spans 0.5/spread
         * of that distance, and `px` screen pixels cover one atlas pixel, so a
         * width of W drawn pixels is W * 0.5 / (spread * px).  Same derivation
         * as the smoothing above, which is why they sit together. */
        const float per_px = 0.5f / ((float)spread * px);
        st.outline_width = s_text_style->outline_width * per_px;
        for (int i = 0; i < 4; i++) {
            st.outline_rgba[i] = s_text_style->outline_color[i];
            st.shadow_rgba[i]  = s_text_style->shadow_color[i];
        }
        /* The shadow offset is in TEXTURE space so the shader never needs the
         * atlas dimensions.  It is also in the ATLAS's pixels, not the screen's
         * -- an offset of N drawn pixels is N/scale atlas pixels. */
        if (aw > 0u && ah > 0u) {
            st.shadow_uv[0] = (s_text_style->shadow_offset[0] / px) / (float)aw;
            st.shadow_uv[1] = (s_text_style->shadow_offset[1] / px) / (float)ah;
        }
    }

    (void)jce_draw_text_sdf_quads_view_xf(r, view_id, batch, n, tex,
                                          s_text_xf, &st);
}


void jce_text_draw_styled_view_xf(const JceRenderer *r, JceFont *font,
                                  uint16_t view_id,
                                  float x, float y, float scale,
                                  const char *text, uint32_t color,
                                  const JceRectXform *xf,
                                  const JceTextStyle *style)
{
    s_text_xf    = xf;
    s_text_style = style;
    jce_text_draw_scaled_view(r, font, view_id, x, y, scale, text, color);
    s_text_style = NULL;
    s_text_xf    = NULL;
}

void jce_text_draw_scaled_view_xf(const JceRenderer *r, JceFont *font,
                                  uint16_t view_id,
                                  float x, float y, float scale,
                                  const char *text, uint32_t color,
                                  const JceRectXform *xf)
{
    jce_text_draw_styled_view_xf(r, font, view_id, x, y, scale, text, color,
                                 xf, NULL);
}

void jce_text_draw_scaled_view(const JceRenderer *r, JceFont *font,
                               uint16_t view_id,
                               float x, float y, float scale,
                               const char *text, uint32_t color)
{
    if (!r || !font || !text || !font->hb_font) return;
    if (!jce_texture_valid(font->atlas)) return;

    /* Shaping is a pure function of (font, text) -- `scale` is applied to the
     * fixed-point values BELOW, not inside HarfBuzz -- so a label whose string
     * has not changed has nothing to recompute.  It was recomputed every
     * frame, and twice per frame for anything that measures before it draws.
     * See renderer/jce_text_shape.h. */
    uint32_t glyph_count = 0;
    const JceTextFontChain chain = font_chain(font);
    const JceShapedGlyph *run = jce_text_shape(font, &chain, text,
                                               &glyph_count);
    if (!run) return;

    float cx = x;
    float cy = y;

    /* A glyph run is ONE draw call.
     *
     * Every glyph used to go through jce_draw_textured_rect_view on its own,
     * so a 60-character line cost 60 transient allocations, 60 texture binds,
     * 60 state sets and 60 submits that differed only in four floats and a
     * UV rect.  A screen of UI text ran into the hundreds, and
     * jce_ui_canvas.c:80 named the cost in its own comment while it stayed.
     *
     * The batch drains on three events and nothing else:
     *   - the atlas changes, because a run may straddle the static atlas and
     *     the dynamic one (g->dynamic) glyph by glyph;
     *   - the staging array fills;
     *   - the run ends.
     * Ordering is unaffected: the quads in a batch share one texture and one
     * state, the UI views are BGFX_VIEW_MODE_SEQUENTIAL, and nothing else can
     * submit between two glyphs of the same run.
     *
     * 128 quads is 4.6 KB of stack and covers a full line of text at any
     * realistic width; longer runs simply drain more than once. */
    enum { TEXT_BATCH_QUADS = 128 };
    JceQuad2D  batch[TEXT_BATCH_QUADS];
    uint32_t   batch_n = 0;
    JceTexture batch_tex = { UINT16_MAX };
    /* The batch also drains when the FONT changes, not only the atlas: the
     * flush reads atlas_w/atlas_h and sdf_spread off it, and two fonts can
     * share neither.  Two different fonts with equal atlas ids is not possible
     * today, but "the texture happens to match" is not the property the flush
     * depends on. */
    JceFont   *batch_font = font;

    for (uint32_t i = 0; i < glyph_count; i++) {
        /* The atlas is indexed by Unicode codepoint, and the shaper decoded
           it from the cluster once, when it shaped.  Reading it back here
           means the run does not refer into `text` at all. */
        uint32_t ucp = run[i].codepoint;

        if (ucp == '\n') {
            cx = x;
            cy += (float)font->line_height * scale;
            continue;
        }

        /* Whitespace is advance-only.  Fonts may lack a glyph for U+0020
         * entirely (the pinned UI font does), in which case FreeType
         * rasterizes .notdef — a visible tofu box — for every word gap. */
        if (ucp == ' ' || ucp == '\t' || ucp == 0x00A0u) {
            cx += (float)(run[i].x_advance >> 6) * scale;
            continue;
        }

        float x_offset  = (float)(run[i].x_offset >> 6) * scale;
        float y_offset  = (float)(run[i].y_offset >> 6) * scale;
        float x_advance = (float)(run[i].x_advance >> 6) * scale;

        /* The glyph comes out of the font the SHAPER chose for it, which is
         * the primary unless a fallback claimed this codepoint. */
        JceFont *gf = font_for_slot(font, run[i].font_slot);
        const GlyphInfo *g = font_get_or_render_glyph(gf, ucp);
        if (!g) g = font_get_glyph(gf, '?');

        if (g && g->w > 0 && g->h > 0) {
            const float gx = cx + x_offset + (float)g->bearing_x * scale;
            /* The PRIMARY's ascender, not the fallback's: the baseline belongs
             * to the line, and two fonts opened at one point size do not agree
             * on where the top of their box is.  Using each glyph's own font
             * here makes a mixed line ripple. */
            const float gy = cy + y_offset
                           + (float)(font->ascender - g->bearing_y) * scale;
            const JceTexture atlas = g->dynamic ? gf->dyn_atlas
                                                : gf->atlas;
            JceQuad2D *q;

            if (batch_n > 0 && (atlas.idx != batch_tex.idx
                                || batch_font != gf
                                || batch_n == TEXT_BATCH_QUADS)) {
                text_flush_batch(r, view_id, batch, batch_n, batch_tex,
                                 batch_font, scale);
                batch_n = 0;
            }
            batch_tex  = atlas;
            batch_font = gf;

            q = &batch[batch_n++];
            q->x  = gx;             q->y  = gy;
            q->w  = (float)g->w * scale;
            q->h  = (float)g->h * scale;
            q->u0 = g->u0;          q->v0 = g->v0;
            q->u1 = g->u1;          q->v1 = g->v1;
            q->tint = color;
        }
        cx += x_advance;
    }

    if (batch_n > 0)
        text_flush_batch(r, view_id, batch, batch_n, batch_tex, batch_font,
                         scale);
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

    /* The SAME cached run the draw path uses.  A UI lays out and then draws,
     * so this used to be the second full shape of one string in one frame. */
    uint32_t glyph_count = 0;
    const JceTextFontChain chain = font_chain(font);
    const JceShapedGlyph *run = jce_text_shape(font, &chain, text,
                                               &glyph_count);
    if (!run) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    float max_w = 0, cx = 0;
    int lines = 1;

    for (uint32_t i = 0; i < glyph_count; i++) {
        if (run[i].codepoint == '\n') {
            if (cx > max_w) max_w = cx;
            cx = 0;
            lines++;
            continue;
        }
        cx += (float)(run[i].x_advance >> 6);
    }
    if (cx > max_w) max_w = cx;

    if (out_w) *out_w = max_w;
    if (out_h) *out_h = (float)(lines * font->line_height);
}

/* -- Math markup layout + draw ---------------------------------------
 *
 * A compact LaTeX-flavoured 2D formula notation for HUD / gallery text:
 *   \<name>        symbol substitution (\mu \pi \Delta \sqrt \sum \times ...)
 *   ^x  ^{..}      superscript (raised, smaller)
 *   _x  _{..}      subscript   (lowered, smaller)
 *   \frac{A}{B}    stacked fraction with a horizontal bar
 *   \sqrt{A}       radical with a vinculum bar over A
 *   \dot{x} \ddot{x} \bar{x} \vec{x} \hat{x} \tilde{x}   accents
 *   {..}           grouping
 *   any other UTF-8 is literal, so Unicode math also works.
 * Layout is a recursive box model: every sub-expression reports (width,
 * ascent, descent) so fractions/radicals/scripts stack correctly, and the
 * same walk measures (draw=false) or renders (draw=true). */

typedef struct { const char *name; uint32_t cp; } MathSymbol;
static const MathSymbol MATH_SYMBOLS[] = {
    {"alpha",0x03B1},{"beta",0x03B2},{"gamma",0x03B3},{"delta",0x03B4},
    {"epsilon",0x03B5},{"eta",0x03B7},{"theta",0x03B8},{"kappa",0x03BA},
    {"lambda",0x03BB},{"mu",0x03BC},{"nu",0x03BD},{"xi",0x03BE},{"pi",0x03C0},
    {"rho",0x03C1},{"sigma",0x03C3},{"tau",0x03C4},{"phi",0x03C6},
    {"chi",0x03C7},{"psi",0x03C8},{"omega",0x03C9},
    {"Delta",0x0394},{"Sigma",0x03A3},{"Omega",0x03A9},{"Phi",0x03A6},
    {"Gamma",0x0393},{"Lambda",0x039B},{"Pi",0x03A0},{"Theta",0x0398},
    {"Psi",0x03A8},{"Xi",0x039E},
    {"sqrt",0x221A},{"int",0x222B},{"sum",0x2211},{"prod",0x220F},
    {"partial",0x2202},{"nabla",0x2207},{"infty",0x221E},
    {"times",0x00D7},{"cdot",0x00B7},{"div",0x00F7},{"pm",0x00B1},{"mp",0x2213},
    {"approx",0x2248},{"neq",0x2260},{"leq",0x2264},{"geq",0x2265},{"equiv",0x2261},
    {"le",0x2264},{"ge",0x2265},{"ll",0x226A},{"gg",0x226B},
    {"to",0x2192},{"rightarrow",0x2192},{"Rightarrow",0x21D2},
    {"leftarrow",0x2190},{"propto",0x221D},{"deg",0x00B0},
    {"cdots",0x22EF},{"ldots",0x2026},{"in",0x2208},{"forall",0x2200},
    {"angle",0x2220},{"perp",0x22A5},{"parallel",0x2225},
    {"sim",0x223C},{"simeq",0x2243},{"cong",0x2245},{"ast",0x2217},
};

static uint32_t math_symbol_cp(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof(MATH_SYMBOLS) / sizeof(MATH_SYMBOLS[0]); ++i)
        if (strlen(MATH_SYMBOLS[i].name) == len &&
            strncmp(MATH_SYMBOLS[i].name, name, len) == 0)
            return MATH_SYMBOLS[i].cp;
    return 0;
}

/* A formula's glyphs batch exactly like a plain run's, with one extra rule:
 * a fraction bar or radical overline is an UNTEXTURED rect on a different
 * program, so it cannot join the batch and cannot be reordered across it.
 * math_hline therefore drains first.  With that, submit order is unchanged
 * from the per-glyph version, which is what makes this safe on the
 * SEQUENTIAL UI views where submit order IS draw order.
 *
 * 64 quads: formulas are short, and there is exactly one MathCtx per call --
 * math_run recurses but takes it by pointer, and math_measure_run toggles
 * m->draw in place rather than copying. */
enum { MATH_BATCH_QUADS = 64 };

typedef struct {
    const JceRenderer *r;
    JceFont           *font;
    uint16_t           view;
    uint32_t           color;
    bool               draw;
    JceQuad2D          batch[MATH_BATCH_QUADS];
    uint32_t           batch_n;
    JceTexture         batch_tex;
} MathCtx;

static void math_ctx_init(MathCtx *m, const JceRenderer *r, JceFont *font,
                          uint16_t view, uint32_t color, bool draw)
{
    m->r = r; m->font = font; m->view = view; m->color = color; m->draw = draw;
    m->batch_n = 0;
    m->batch_tex.idx = UINT16_MAX;
}

static void math_flush(MathCtx *m)
{
    if (m->batch_n == 0) return;
    jce_draw_textured_quads_view(m->r, m->view, m->batch, m->batch_n,
                                 m->batch_tex);
    m->batch_n = 0;
}

/* Queue one glyph quad.  w and h are passed rather than derived, because the
 * radical stretches its glyph vertically (h uses a fitted scale, w does not). */
static void math_push_glyph(MathCtx *m, const GlyphInfo *g, JceTexture atlas,
                            float gx, float gy, float w, float h)
{
    JceQuad2D *q;
    if (m->batch_n > 0 && (atlas.idx != m->batch_tex.idx
                           || m->batch_n == MATH_BATCH_QUADS))
        math_flush(m);
    m->batch_tex = atlas;
    q = &m->batch[m->batch_n++];
    q->x = gx; q->y = gy; q->w = w; q->h = h;
    q->u0 = g->u0; q->v0 = g->v0; q->u1 = g->u1; q->v1 = g->v1;
    q->tint = m->color;
}

/* Box metrics: width plus extents above (ascent) and below (descent) the
 * baseline.  All values already include the run's scale. */
typedef struct { float w, ascent, descent; } MBox;

/* Lay out one glyph as a box at (x, baseline); draw when m->draw. */
static MBox math_glyph_box(MathCtx *m, uint32_t cp, float x, float baseline,
                           float scale)
{
    MBox b = {0, 0, 0};
    const GlyphInfo *g = font_get_or_render_glyph(m->font, cp);
    if (!g) g = font_get_glyph(m->font, '?');
    if (!g) return b;
    b.w = (float)g->advance * scale;
    b.ascent  = (float)g->bearing_y * scale;
    b.descent = (float)(g->h - g->bearing_y) * scale;
    if (b.ascent < 0) b.ascent = 0;
    if (b.descent < 0) b.descent = 0;
    if (m->draw && m->r && g->w > 0 && g->h > 0) {
        float gx = x + (float)g->bearing_x * scale;
        float gy = baseline - (float)g->bearing_y * scale;
        JceTexture atlas = g->dynamic ? m->font->dyn_atlas : m->font->atlas;
        math_push_glyph(m, g, atlas, gx, gy,
                        (float)g->w * scale, (float)g->h * scale);
    }
    return b;
}

static void math_hline(MathCtx *m, float x, float y, float w, float t)
{
    if (m->draw && m->r && w > 0) {
        if (t < 1.0f) t = 1.0f;
        /* Untextured, different program: it can neither join the glyph batch
         * nor be reordered across it.  Draining here is what keeps the
         * batched formula identical in submit order to the unbatched one. */
        math_flush(m);
        jce_draw_filled_rect_view(m->r, m->view, x, y, w, t, m->color);
    }
}

/* Forward decl: lay out a horizontal run [s,len) at (x,baseline), scale. */
static MBox math_run(MathCtx *m, const char *s, size_t len,
                     float x, float baseline, float scale);

/* Measure a sub-run WITHOUT drawing (used to size fractions/radicals/scripts
 * before positioning), even while the parent is in draw mode.  Without this
 * the sizing pass would render the child at the throwaway (0,0) origin. */
static MBox math_measure_run(MathCtx *m, const char *s, size_t len, float scale)
{
    bool saved = m->draw;
    m->draw = false;
    MBox b = math_run(m, s, len, 0.0f, 0.0f, scale);
    m->draw = saved;
    return b;
}

/* Read a braced group at s[*pos]; returns inner [*inner,*ilen) and advances
 * *pos past '}'.  Without a brace, takes one unit (a \token or one glyph). */
static void math_read_group(const char *s, size_t len, size_t *pos,
                            const char **inner, size_t *ilen)
{
    size_t i = *pos;
    if (i < len && s[i] == '{') {
        size_t k = i + 1, depth = 1;
        while (k < len && depth) {
            if (s[k] == '{') depth++;
            else if (s[k] == '}') depth--;
            if (depth) k++;
        }
        *inner = s + i + 1; *ilen = k - (i + 1);
        *pos = (k < len) ? k + 1 : len;
        return;
    }
    if (i < len && s[i] == '\\') {
        size_t n = 0;
        while (i + 1 + n < len &&
               ((s[i+1+n] >= 'A' && s[i+1+n] <= 'Z') ||
                (s[i+1+n] >= 'a' && s[i+1+n] <= 'z'))) n++;
        *inner = s + i; *ilen = 1 + n; *pos = i + 1 + n;
        return;
    }
    if (i < len) {
        const char *p = s + i; utf8_decode(&p);
        size_t adv = (size_t)(p - (s + i));
        if (adv == 0) adv = 1;
        *inner = s + i; *ilen = adv; *pos = i + adv;
        return;
    }
    *inner = s + i; *ilen = 0; *pos = len;
}

static MBox math_run(MathCtx *m, const char *s, size_t len,
                     float x, float baseline, float scale)
{
    const float em = (float)m->font->line_height * scale;
    float pen = x;
    MBox box = {0, 0, 0};
    size_t i = 0;
    while (i < len) {
        MBox atom = {0, 0, 0};
        char ch = s[i];

        if (ch == '\\') {
            size_t j = i + 1, n = 0;
            while (j + n < len &&
                   ((s[j+n] >= 'A' && s[j+n] <= 'Z') ||
                    (s[j+n] >= 'a' && s[j+n] <= 'z'))) n++;
            if (n == 4 && strncmp(s + j, "frac", 4) == 0) {
                size_t pos = j + n;
                const char *A, *B; size_t la, lb;
                math_read_group(s, len, &pos, &A, &la);
                math_read_group(s, len, &pos, &B, &lb);
                float fs = scale * 0.94f;
                MBox nb = math_measure_run(m, A, la, fs);
                MBox db = math_measure_run(m, B, lb, fs);
                float pad = 2.0f * scale;
                float barw = (nb.w > db.w ? nb.w : db.w) + 2.0f * pad;
                float axis = baseline - 0.26f * em;
                float t = 0.06f * em; if (t < 1.0f) t = 1.0f;
                float gap = 0.14f * em;
                float num_base = axis - gap - nb.descent;
                float den_base = axis + t + gap + db.ascent;
                if (m->draw) {
                    math_run(m, A, la, pen + (barw - nb.w) * 0.5f, num_base, fs);
                    math_run(m, B, lb, pen + (barw - db.w) * 0.5f, den_base, fs);
                    math_hline(m, pen, axis, barw, t);
                }
                atom.w = barw;
                atom.ascent  = baseline - (num_base - nb.ascent);
                atom.descent = (den_base + db.descent) - baseline;
                i = pos;
            }
            else if (n == 4 && strncmp(s + j, "sqrt", 4) == 0 &&
                     j + n < len && s[j + n] == '{') {
                size_t pos = j + n;
                const char *A; size_t la;
                math_read_group(s, len, &pos, &A, &la);
                MBox ab = math_measure_run(m, A, la, scale);
                float gap = 0.12f * em;
                float t = 0.05f * em; if (t < 1.0f) t = 1.0f;
                const GlyphInfo *rg = font_get_or_render_glyph(m->font, 0x221A);
                float rad_w = rg ? (float)rg->advance * scale : 0.5f * em;
                float top = baseline - ab.ascent - gap - t;
                if (m->draw && rg && rg->w > 0 && rg->h > 0) {
                    float target_h = ab.ascent + ab.descent + gap + t;
                    float gscale = target_h / (float)rg->h;
                    if (gscale < scale) gscale = scale;
                    float gx = pen + (float)rg->bearing_x * scale;
                    float gy = baseline + ab.descent - (float)rg->bearing_y * gscale;
                    JceTexture atl = rg->dynamic ? m->font->dyn_atlas
                                                 : m->font->atlas;
                    math_push_glyph(m, rg, atl, gx, gy,
                                    (float)rg->w * scale,
                                    (float)rg->h * gscale);
                }
                if (m->draw) {
                    math_hline(m, pen + rad_w, top, ab.w + gap, t);
                    math_run(m, A, la, pen + rad_w + gap * 0.5f, baseline, scale);
                }
                atom.w = rad_w + ab.w + gap;
                atom.ascent = ab.ascent + gap + t;
                atom.descent = ab.descent;
                i = pos;
            }
            else if ((n == 3 && (strncmp(s+j,"dot",3)==0 || strncmp(s+j,"bar",3)==0 ||
                                 strncmp(s+j,"vec",3)==0 || strncmp(s+j,"hat",3)==0)) ||
                     (n == 4 && strncmp(s+j,"ddot",4)==0) ||
                     (n == 5 && strncmp(s+j,"tilde",5)==0)) {
                char kind0 = s[j];
                bool ddot = (n == 4);
                size_t pos = j + n;
                const char *A; size_t la;
                math_read_group(s, len, &pos, &A, &la);
                MBox ab = math_run(m, A, la, m->draw ? pen : 0, baseline, scale);
                float mark_y = baseline - ab.ascent - 0.10f * em;
                if (m->draw) {
                    if (strncmp(s+j,"bar",3)==0 && n==3) {
                        math_hline(m, pen, mark_y, ab.w, 0.05f * em);
                    } else if (kind0 == 'd' && !ddot) {
                        math_glyph_box(m, 0x02D9, pen + ab.w*0.5f - 0.12f*em,
                                       mark_y + 0.16f*em, scale);
                    } else if (ddot) {
                        math_glyph_box(m, 0x02D9, pen + ab.w*0.5f - 0.26f*em,
                                       mark_y + 0.16f*em, scale);
                        math_glyph_box(m, 0x02D9, pen + ab.w*0.5f + 0.02f*em,
                                       mark_y + 0.16f*em, scale);
                    } else if (strncmp(s+j,"vec",3)==0) {
                        math_glyph_box(m, 0x2192, pen + ab.w*0.5f - 0.20f*em,
                                       mark_y + 0.24f*em, scale * 0.7f);
                    } else if (strncmp(s+j,"hat",3)==0) {
                        math_glyph_box(m, 0x02C6, pen + ab.w*0.5f - 0.12f*em,
                                       mark_y + 0.20f*em, scale);
                    } else {
                        math_glyph_box(m, 0x02DC, pen + ab.w*0.5f - 0.12f*em,
                                       mark_y + 0.20f*em, scale);
                    }
                }
                atom.w = ab.w; atom.ascent = ab.ascent + 0.18f * em;
                atom.descent = ab.descent;
                i = pos;
            }
            else if (n > 0) {
                uint32_t cp = math_symbol_cp(s + j, n);
                if (cp) { atom = math_glyph_box(m, cp, pen, baseline, scale);
                          i = j + n; }
                else { atom = math_glyph_box(m, (uint32_t)(unsigned char)s[j],
                                             pen, baseline, scale); i = j + 1; }
            } else {
                if (j < len) { atom = math_glyph_box(m, (uint32_t)(unsigned char)s[j],
                                                     pen, baseline, scale); i = j + 1; }
                else i = len;
            }
        }
        else if (ch == '^' || ch == '_') {
            float ss = scale * 0.72f;
            size_t pos = i + 1;
            const char *A; size_t la;
            math_read_group(s, len, &pos, &A, &la);
            MBox sb = math_measure_run(m, A, la, ss);
            float dy = (ch == '^') ? -0.42f * em : 0.24f * em;
            if (m->draw) math_run(m, A, la, pen, baseline + dy, ss);
            atom.w = sb.w;
            atom.ascent = (ch == '^') ? (sb.ascent - dy) : sb.ascent;
            atom.descent = (ch == '_') ? (sb.descent + dy) : sb.descent;
            if (atom.ascent < 0) atom.ascent = 0;
            if (atom.descent < 0) atom.descent = 0;
            i = pos;
        }
        else if (ch == '{') {
            size_t pos = i;
            const char *A; size_t la;
            math_read_group(s, len, &pos, &A, &la);
            atom = math_run(m, A, la, pen, baseline, scale);
            i = pos;
        }
        else if (ch == '}') { i++; continue; }
        else if (ch == ' ' || ch == '\t') {
            const GlyphInfo *sp = font_get_or_render_glyph(m->font, ' ');
            atom.w = (sp ? (float)sp->advance : em * 0.3f) * scale;
            i++;
        }
        else {
            const char *p = s + i;
            uint32_t cp = utf8_decode(&p);
            size_t adv = (size_t)(p - (s + i));
            atom = math_glyph_box(m, cp, pen, baseline, scale);
            i += (adv > 0 ? adv : 1);
        }

        pen += atom.w;
        box.w = pen - x;
        if (atom.ascent  > box.ascent)  box.ascent  = atom.ascent;
        if (atom.descent > box.descent) box.descent = atom.descent;
    }
    return box;
}

void jce_text_measure_math(const JceFont *font, const char *markup,
                           float *out_w, float *out_h)
{
    if (!font || !markup) { if (out_w) *out_w = 0; if (out_h) *out_h = 0; return; }
    /* Not memset: MathCtx now carries a 64-quad staging array, and zeroing
     * 2.3 KB on a path that draws nothing is work for no one.  The cast is
     * because on-demand rasterization is a cache mutation. */
    MathCtx m;
    math_ctx_init(&m, NULL, (JceFont *)font, 0u, 0u, false);
    MBox b = math_run(&m, markup, strlen(markup), 0.0f, 0.0f, 1.0f);
    if (out_w) *out_w = b.w;
    if (out_h) *out_h = (float)font->line_height;
}

void jce_text_draw_math_view(const JceRenderer *r, JceFont *font,
                             uint16_t view_id, float x, float y, float scale,
                             const char *markup, uint32_t color)
{
    if (!r || !font || !markup || !jce_texture_valid(font->atlas)) return;
    MathCtx m;
    math_ctx_init(&m, r, font, view_id, color, true);
    /* y is the line's top (canvas convention); shift to a baseline so
     * ascenders sit inside the line box. */
    float baseline = y + (float)font->ascender * scale;
    (void)math_run(&m, markup, strlen(markup), x, baseline, scale);
    math_flush(&m);
}

