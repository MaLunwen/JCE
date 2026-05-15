/*
 * jce_sdf_font.h  Signed-distance-field font runtime.
 *
 * TextMeshPro equivalent at the data layer.  Stores a font's glyph
 * table (one entry per code point) backed by an SDF atlas texture
 * + the typesetting helpers needed to lay out a UTF-8 string into a
 * quad list (positions + UVs).
 *
 * SDF rendering itself lives in the shader (sampling distance,
 * smoothstep around the 0.5 isoline) — this module owns the atlas
 * metadata + layout maths.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SDF_FONT_H
#define JCE_SDF_FONT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SDF_FONT_MAX_GLYPHS 4096

typedef struct {
    uint32_t codepoint;
    /* Pixel-space rect of the glyph within the atlas. */
    uint16_t atlas_x, atlas_y;
    uint16_t width, height;
    /* Layout metrics — pixels at the atlas's native pt size. */
    int16_t  bearing_x;     /* left side bearing */
    int16_t  bearing_y;     /* baseline-to-top distance */
    uint16_t advance;       /* pen advance after this glyph */
    bool     active;
} JceSdfGlyph;

typedef struct {
    char        name[64];
    char        atlas_path[160];
    uint16_t    atlas_w;
    uint16_t    atlas_h;
    uint16_t    em_size;        /* pixels per em at native pt size */
    int16_t     ascent;
    int16_t     descent;
    int16_t     line_gap;
    float       sdf_pixel_range;/* atlas-side SDF "spread" in px */

    JceSdfGlyph glyphs[JCE_SDF_FONT_MAX_GLYPHS];
    uint32_t    glyph_count;
} JceSdfFont;

JCE_API void jce_sdf_font_init(JceSdfFont *f, const char *name,
                                const char *atlas_path,
                                uint16_t atlas_w, uint16_t atlas_h);

/* Add/update one glyph; returns false on overflow. */
JCE_API bool jce_sdf_font_add_glyph(JceSdfFont *f, const JceSdfGlyph *g);

/* Lookup by codepoint.  Returns NULL when unknown. */
JCE_API const JceSdfGlyph *jce_sdf_font_find(const JceSdfFont *f,
                                              uint32_t codepoint);

/* Layout helpers.
 * `out_quads_pos` and `out_quads_uv` must each hold cap * 8 floats
 * (cap quads × 4 corners × xy).  Returns the number of quads
 * emitted.  Caller-supplied `point_size` is the desired draw height
 * in pixels (scale = point_size / em_size).
 *
 * Newline handling: `\n` resets x to `origin_x` and advances y by
 * one line-height = ascent + descent + line_gap (scaled). */
JCE_API uint32_t jce_sdf_layout_utf8(const JceSdfFont *f,
                                       const char *utf8_text,
                                       float origin_x, float origin_y,
                                       float point_size,
                                       float *out_quads_pos,
                                       float *out_quads_uv,
                                       uint32_t cap_quads);

/* Compute pixel size (w, h) the string would occupy at `point_size`.
 * Useful for centring / autosize before laying out. */
JCE_API void jce_sdf_measure_utf8(const JceSdfFont *f,
                                    const char *utf8_text,
                                    float point_size,
                                    float *out_w,
                                    float *out_h);

JCE_EXTERN_C_END

#endif /* JCE_SDF_FONT_H */
