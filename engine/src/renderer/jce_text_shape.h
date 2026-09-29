/*
 * jce_text_shape.h -- shape a string once, not once per frame.
 *
 * HarfBuzz shaping is a pure function of (font, string): it maps bytes to
 * glyph ids and 26.6 offsets, and nothing in the result depends on where the
 * text lands, what colour it is, or what scale it is drawn at -- jce_text.c
 * multiplies by `scale` AFTER shaping, on the fixed-point values this returns.
 * So a UI label that does not change its text has exactly one shaping result
 * for its whole lifetime.
 *
 * It was being recomputed every frame, twice.  jce_text_draw_scaled_view and
 * jce_text_measure each did hb_buffer_create -> add_utf8 ->
 * guess_segment_properties -> hb_shape -> hb_buffer_destroy, and a UI lays out
 * before it draws, so every visible string paid for two full shapes plus two
 * HarfBuzz buffer allocations per frame.  Unity, Unreal and Godot all cache
 * this; it is the expensive half of text rendering, and the glyph BATCHING
 * that this renderer already has does nothing about it, because batching is
 * about submits and this is about CPU before them.
 *
 * WHY A SEPARATE TRANSLATION UNIT.  The cache has to be reachable from a test,
 * and jce_text.c's own entry points cannot be: a JceFont needs a rasterised
 * bgfx atlas, which is why tests/renderer/test_jce_text_unicode_backend.c
 * already talks to HarfBuzz directly and says so.  Taking an hb_font_t rather
 * than a JceFont keeps this testable headless while costing the caller
 * nothing -- jce_text.c passes font->hb_font and the JceFont pointer as the
 * eviction key.
 *
 * IT ALSO ITEMIZES.  A font chain has a primary and up to a few fallbacks, and
 * HarfBuzz shapes with exactly ONE font, so a string that mixes scripts has to
 * be split into runs first -- each run the longest span one font covers -- and
 * each run shaped with its own font.  That split belongs here rather than in
 * the draw loop because it is an input to the cache key's answer, not an
 * output: the same bytes with a different chain are a different run.
 *
 * Layer: renderer (Layer 3) -- PRIVATE.  Not under engine/include/ on purpose:
 * nothing outside this renderer shapes text, and a public shaping API would
 * have to promise a glyph-run representation this one is free to change.
 *
 * Thread-safety: none, like the rest of jce_text.c -- one static table, called
 * from the thread that draws.
 */
#ifndef JCE_TEXT_SHAPE_H
#define JCE_TEXT_SHAPE_H

#include <hb.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One shaped glyph.
 *
 * `codepoint` is the Unicode codepoint DECODED AT SHAPE TIME from the cluster
 * HarfBuzz reported, not a glyph id: this renderer's atlas is indexed by
 * codepoint (font_get_or_render_glyph), and decoding here means the cached run
 * no longer refers into the caller's string at all -- a cached run and the
 * buffer it was shaped from have independent lifetimes.
 *
 * Offsets and advances are HarfBuzz's 26.6 fixed point, unscaled.  The caller
 * shifts and multiplies by its own `scale`, exactly as it did when it read
 * them straight out of hb_glyph_position_t. */
typedef struct {
    uint32_t codepoint;
    int32_t  x_offset;
    int32_t  y_offset;
    int32_t  x_advance;
    /* Which font in the chain produced this glyph: 0 = primary, 1..N = the
     * caller's fallbacks in order.  The DRAW side needs it to pick the atlas
     * the glyph was rasterised into; a run that ignored it would look correct
     * for Latin and draw tofu for everything else, which is the state this
     * exists to end. */
    uint8_t  font_slot;
} JceShapedGlyph;

/* The caller's font chain, as two questions and a context.
 *
 * Two callbacks rather than an array of hb_font_t so this stays testable
 * headless AND so FreeType stays out of this file: "does this font have a
 * glyph for U+4E2D" is an FT_Get_Char_Index question that jce_text.c can
 * answer and this module has no business knowing about. */
typedef struct {
    void *ctx;
    /* The lowest slot whose font covers `cp`, or 0 when nothing does -- a
     * .notdef from the primary is the honest answer to "no font has this",
     * and it is what shipped before there was a chain. */
    uint8_t     (*slot_for_codepoint)(void *ctx, uint32_t cp);
    /* The hb_font for a slot.  NULL is treated as "cannot shape this run". */
    hb_font_t  *(*font_for_slot)(void *ctx, uint8_t slot);
    /* Changes when the chain changes, so a cached run shaped against an older
     * chain is not served.  jce_text.c bumps it in jce_font_add_fallback. */
    uint32_t      generation;
} JceTextFontChain;

/* The shaped run for (owner, text), from the cache when it is there.
 *
 * `owner` is an identity for eviction only -- pass the JceFont.  It is part of
 * the key because two fonts shape the same bytes differently, and it is what
 * jce_text_shape_forget() matches on.  `chain->generation` is part of the key
 * too: adding a fallback changes what the same bytes shape to.
 *
 * Returns NULL (and *out_count = 0) only when the arguments are unusable.  An
 * empty string is a valid run of length 0 and returns non-NULL.
 *
 * LIFETIME: valid until the NEXT call to this function.  Both call sites
 * consume the run fully before shaping anything else.  The entry just returned
 * is additionally protected from eviction by the next call, so the one-run
 * contract holds even when the next shape collides with it in the table. */
const JceShapedGlyph *jce_text_shape(const void *owner,
                                     const JceTextFontChain *chain,
                                     const char *text, uint32_t *out_count);

/* Drop every run belonging to `owner`.  MUST be called when a font is closed:
 * the owner pointer is a key, and a later allocation at the same address would
 * otherwise inherit another font's runs. */
void jce_text_shape_forget(const void *owner);

/* Release the table.  Called from jce_text_shutdown. */
void jce_text_shape_shutdown(void);

/* How many lookups were served from the table, and how many actually ran
 * HarfBuzz.  Exposed because "it caches" is a claim about counts and there is
 * no other way to see it from outside: the pixels are identical either way,
 * which is the whole point and also what makes a broken cache invisible. */
void jce_text_shape_stats(uint64_t *out_hits, uint64_t *out_misses);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TEXT_SHAPE_H */
