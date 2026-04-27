/*
 * jce_text.h  GPU-accelerated text rendering via glyph atlas.
 *
 * Loads a TTF font from PAK, pre-renders ASCII glyphs to a single
 * texture atlas, and draws strings as textured quads.
 */

#ifndef JCE_TEXT_H
#define JCE_TEXT_H


#include <jce/os/core/jce_defs.h>

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
JceFont *jce_font_open_ex(const JcePakArchive *pak, const char *asset_path,
                           float pt_size,
                           const uint32_t *extra_cps, int extra_count);

/* Close a font and free its atlas texture. */
JCE_API void jce_font_close(JceFont *font);

/* Draw a UTF-8 text string at (x, y) in logical coordinates.
   color: ABGR packed via jce_rgba(). */
void jce_text_draw(const JceRenderer *r, JceFont *font,
                   float x, float y,
                   const char *text, uint32_t color);

/* Draw text with a uniform scale factor. */
void jce_text_draw_scaled(const JceRenderer *r, JceFont *font,
                          float x, float y, float scale,
                          const char *text, uint32_t color);

/* Get the line height in pixels (at native pt_size). */
JCE_API int jce_font_line_height(const JceFont *font);

/* Measure a string's bounding box in pixels (at native pt_size). */
void jce_text_measure(const JceFont *font, const char *text,
                      float *out_w, float *out_h);

JCE_EXTERN_C_END

#endif /* JCE_TEXT_H */
