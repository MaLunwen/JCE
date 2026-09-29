/*
 * jce_text.h  GPU-accelerated text rendering via glyph atlas.
 *
 * Loads a TTF font from PAK, pre-renders ASCII glyphs to a single
 * texture atlas, and draws strings as textured quads.
 */

#ifndef JCE_TEXT_H
#define JCE_TEXT_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_primitives.h>   /* JceRectXform */

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceFont     JceFont;
typedef struct JceRenderer JceRenderer;
typedef struct JcePakArchive  JcePakArchive;

/* Lifetime / ownership notes
 * ---------------------------
 *  - JceFont owns its glyph atlas texture and an internal copy of the
 *    decompressed TTF byte buffer; callers must not free the PAK entry
 *    on the font's behalf.
 *  - Font instances are not thread-safe; create/draw/close on one
 *    thread (typically the render thread).
 *  - FreeType / HarfBuzz are private implementation details: no
 *    FT_ or hb_ type ever appears in this header. The font subsystem
 *    lazily initializes a process-wide FreeType library on first
 *    jce_font_open*; jce_text_shutdown() releases it (optional, called
 *    automatically at engine shutdown when wired in). */

/* Release process-wide font subsystem resources (FT_Library).
 * Idempotent. Safe to call after all JceFont have been closed.
 * Calling jce_font_open* again after shutdown is allowed and will
 * re-initialize lazily. */
JCE_API void jce_text_shutdown(void);

/* Open a font from a PAK asset (e.g. "fonts/JCE.ttf").
   pt_size: point size for glyph rasterization.
   Returns NULL on failure.  Only ASCII glyphs are pre-rendered. */
JCE_API JceFont *jce_font_open(const JcePakArchive *pak, const char *asset_path, float pt_size);

/* Open a font with additional Unicode codepoints beyond ASCII.
   extra_cps / extra_count: non-ASCII codepoints to pre-render
   (e.g. CJK characters for i18n). */
JCE_API JceFont *jce_font_open_ex(const JcePakArchive *pak, const char *asset_path,
                           float pt_size,
                           const uint32_t *extra_cps, int extra_count);

/* Open a font from a host-filesystem file (project-loose asset).  Same
   contract as jce_font_open_ex but reads via jce_fs_host_read_all instead
   of a PAK — the editor uses this so canvas UI resolves project-authored
   fonts that are not baked into its embedded pak. */
JCE_API JceFont *jce_font_open_file_ex(const char *host_path, float pt_size,
                               const uint32_t *extra_cps, int extra_count);

/* Open a font from an in-memory TTF/OTF blob.  Same contract as
   jce_font_open_ex, but the bytes come from the caller (e.g. read from the
   active VFS / a mounted content bundle) — the canvas uses this so bundle-Play
   resolves fonts that live only in the mounted bundle, not on the host FS or in
   the embedded pak.  The blob is copied; the caller keeps ownership of `data`. */
JCE_API JceFont *jce_font_open_mem_ex(const void *data, size_t size, const char *label,
                              float pt_size,
                              const uint32_t *extra_cps, int extra_count);

/* Most fonts a font may fall back to.  Four covers the case this exists for
 * -- a Latin UI face in front of CJK, plus an emoji or symbol face -- and the
 * bound is what keeps the shaped-glyph slot one byte and the walk per missing
 * codepoint short. */
#define JCE_FONT_MAX_FALLBACKS 4

/* Append `fallback` to `font`'s fallback chain.
 *
 * A font that lacks a codepoint rasterises .notdef -- a visible tofu box -- so
 * a Latin UI font in front of a Chinese, Japanese or Korean string draws a box
 * for EVERY character.  With a chain, the shaper splits the string into runs by
 * which font covers each codepoint and shapes each run with its own font, so
 * the fallback contributes real advances rather than borrowed ones.
 *
 * THE PRIMARY ALWAYS WINS when it covers the character, so adding a fallback
 * cannot change how a string that already rendered looks.
 *
 * NOT OWNED: `fallback` stays the caller's to close, and one CJK face can back
 * every UI font in the project.  Close the fallback AFTER the fonts that name
 * it.  Refuses a cycle, a duplicate is a no-op that returns true, and a
 * fallback that itself has fallbacks is refused rather than flattened.
 * Returns false when the chain is full or the arguments are unusable. */
JCE_API bool jce_font_add_fallback(JceFont *font, JceFont *fallback);

/* Close a font and free its atlas texture.  Its fallbacks are NOT closed. */
JCE_API void jce_font_close(JceFont *font);

/* Draw a UTF-8 text string at (x, y) in logical coordinates.
   color: ABGR packed via jce_rgba(). */
JCE_API void jce_text_draw(const JceRenderer *r, JceFont *font,
                   float x, float y,
                   const char *text, uint32_t color);

/* Draw text with a uniform scale factor. */
JCE_API void jce_text_draw_scaled(const JceRenderer *r, JceFont *font,
                          float x, float y, float scale,
                          const char *text, uint32_t color);

/* View-targeted variant of jce_text_draw_scaled: submits glyph quads into
   an explicit bgfx `view_id` instead of the fixed JCE_VIEW_UI overlay, so
   off-screen UI passes can draw text into their own framebuffer's view.
   Engine-internal (not part of the public consumer API). */
JCE_API void jce_text_draw_scaled_view(const JceRenderer *r, JceFont *font,
                               uint16_t view_id,
                               float x, float y, float scale,
                               const char *text, uint32_t color);

/* The same draw through a 2D rect transform, so a UI label turns and scales
 * with the element it belongs to.  `xf` NULL is the call above, unchanged.
 * The pivot is ABSOLUTE (see JceRectXform): every glyph of the line turns
 * about the ELEMENT's centre, not about its own. */
JCE_API void jce_text_draw_scaled_view_xf(const JceRenderer *r, JceFont *font,
                               uint16_t view_id,
                               float x, float y, float scale,
                               const char *text, uint32_t color,
                               const JceRectXform *xf);

/* Get the line height in pixels (at native pt_size). */
/* Rasterise glyphs as a SIGNED DISTANCE FIELD for fonts opened AFTER this
 * call.  0 (the default) keeps the coverage bitmaps a font has always had.
 *
 * WHY IT EXISTS.  A bitmap atlas stores the glyph at ONE size, so drawing it
 * larger interpolates coverage and the edge becomes a ramp as wide as the
 * magnification -- a heading at 4x is visibly soft, and the only fix in that
 * model is another atlas per size.  A distance field is a smooth function, so
 * bilinear filtering between texels lands ON the outline and one atlas is
 * crisp at every size.  That is what lets a single 512x512 atlas serve a whole
 * UI, which matters most on the machines that can least afford several.
 *
 * WHY IT IS A MODE AND NOT A PARAMETER.  The atlas is rasterised during open,
 * so the choice has to be made before the font exists; there are four openers
 * and this is one addition rather than four.  Same shape as
 * jce_ui_canvas_set_default_font, which this subsystem already uses for a
 * process-scoped authoring choice.  A CACHE keyed by (path, size) must add
 * this to its key -- two components asking for the same font at the same size,
 * one of them SDF, are asking for two different atlases.
 *
 * spread_px is how far the field reaches from the outline; larger costs
 * atlas area and buys smoother scaling and thicker outline effects.  Clamped
 * to [0, 64]. */
JCE_API void jce_font_set_sdf_spread(int spread_px);

/* Outline and drop shadow for DISTANCE-FIELD text, applied to subsequent
 * draws until changed.
 *
 * WHY THE FIELD MAKES THESE CHEAP.  Once a glyph is stored as distance rather
 * than coverage, an outline is a SECOND THRESHOLD on the same number and a
 * drop shadow is the same read at an offset.  Any other way of getting them --
 * a second draw, a second atlas, a blur pass -- costs a pass to produce what
 * one already-loaded value answers.  That is why TextMeshPro, Godot's MSDF
 * fonts and Slate all put them on the field.
 *
 * WHY A PARAMETER AND NOT A MODE.  jce_font_set_sdf_spread next door IS a
 * mode, and it has to be: the spread is consumed when the font is OPENED, and
 * there is no draw call to hang it on.  A style is consumed by the draw, so it
 * is simply an argument -- which means there is no process state to leave set,
 * and therefore nothing for the next label to inherit.
 *
 * IGNORED BY A BITMAP FONT, on purpose rather than by accident: a coverage
 * atlas has no distance to threshold a second time, so there is nothing an
 * outline could mean there.  NULL means no effects. */
typedef struct {
    float outline_width;    /* in PIXELS of the drawn glyph; 0 = no outline */
    float outline_color[4];
    float shadow_offset[2]; /* in PIXELS of the drawn glyph */
    float shadow_color[4];  /* .w == 0 disables it, second sample included */
} JceTextStyle;

/* jce_text_draw_scaled_view_xf plus the style above; `style` NULL is exactly
 * that function.  Borrowed for the call, like `xf` beside it. */
JCE_API void jce_text_draw_styled_view_xf(const JceRenderer *r, JceFont *font,
                                          uint16_t view_id,
                                          float x, float y, float scale,
                                          const char *text, uint32_t color,
                                          const JceRectXform *xf,
                                          const JceTextStyle *style);

/* The spread this font was OPENED with; 0 when its glyphs hold coverage.
 * The draw path asks so it can pick the matching program: a distance field
 * drawn by the coverage shader is a grey block, and the reverse is a hard
 * alpha test. */
JCE_API int jce_font_sdf_spread(const JceFont *font);

JCE_API int jce_font_line_height(const JceFont *font);

/* Measure a string's bounding box in pixels (at native pt_size). */
JCE_API void jce_text_measure(const JceFont *font, const char *text,
                      float *out_w, float *out_h);

/* ── Math markup ───────────────────────────────────────────────────────
 * Render a compact LaTeX-flavoured inline notation for formulas:
 *   \<name> symbol substitution (\mu \pi \Delta \sqrt \int \sum \times
 *           \approx \leq \geq \to \infty ...); ^x/^{..} superscript;
 *   _x/_{..} subscript; {..} grouping; any other UTF-8 is literal (so
 *   Unicode math such as √ μ ² also passes through).  Single line.
 * Backed by the on-demand glyph atlas, so Greek/symbol glyphs render even
 * when they were never pre-baked into the font. */
JCE_API void jce_text_measure_math(const JceFont *font, const char *markup,
                           float *out_w, float *out_h);
JCE_API void jce_text_draw_math_view(const JceRenderer *r, JceFont *font,
                             uint16_t view_id, float x, float y, float scale,
                             const char *markup, uint32_t color);

JCE_EXTERN_C_END

#endif /* JCE_TEXT_H */
