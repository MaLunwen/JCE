/* See jce_text_shape.h for what this is and why it is not in jce_text.c. */
#include "renderer/jce_text_shape.h"

#include "os/core/jce_memory.h"

#include <SDL3/SDL.h>   /* SDL_StepUTF8 -- the decoder jce_text.c uses */

#include <stdbool.h>
#include <string.h>

/* DIRECT-MAPPED, 256 slots, and the simplicity is the point.
 *
 * An LRU needs bookkeeping on every HIT, which is the path that has to stay
 * cheap; direct mapping does nothing on a hit but compare. The cost is that a
 * collision evicts rather than probes, and for the workload -- a screen of UI
 * labels, tens of distinct strings -- 256 slots make that rare. If it ever
 * stops being rare the symptom is a miss count that does not fall, which
 * jce_text_shape_stats reports; a silent one is not possible. */
enum { SHAPE_SLOTS = 256 };

/* Strings longer than this bypass the table and land in a single scratch slot
 * that is always overwritten.
 *
 * Not a refusal: a long string still shapes and still draws, it just does not
 * displace 1 KB of cacheable labels for something that is almost certainly a
 * paragraph drawn once. The bound is on MEMORY, and it has to exist: the key
 * is the string, so an uncapped key is an uncapped allocation driven by
 * whatever the game decides to render. */
enum { SHAPE_MAX_TEXT = 512 };

typedef struct {
    uint64_t        hash;      /* 0 = empty slot                       */
    const void     *owner;
    uint32_t        generation; /* the chain this run was shaped against */
    char           *text;      /* NUL-terminated copy; the key         */
    uint32_t        text_len;
    JceShapedGlyph *glyphs;
    uint32_t        count;
    uint32_t        cap;       /* allocated glyphs, >= count           */
} ShapeSlot;

/* ONE file-scope object, not six.
 *
 * The pieces below are one cache and have one lifetime -- shutdown frees the
 * slots, the scratch slot and the HarfBuzz buffer together, and forget()
 * touches three of them at once.  Six loose statics would be six independent
 * things to reason about and six entries in the duplication audit's
 * global-state count, which is a budget this tree keeps for a reason. */
static struct {
    ShapeSlot        slots[SHAPE_SLOTS];
    ShapeSlot        bypass;    /* for text longer than the cap          */
    hb_buffer_t     *buf;       /* reused; create/destroy per call was a
                                 * malloc pair per string per frame on
                                 * its own                               */
    const ShapeSlot *last;      /* protected from eviction               */
    uint64_t         hits, misses;
    /* Returned for a run of length 0.  An empty string is a legal input with a
     * legal, empty result, and NULL is reserved for "the arguments were
     * unusable" -- collapsing the two would make the caller's error check
     * wrong in exactly the case that is not an error.  Never written; it lives
     * in here rather than beside it so this file keeps ONE global. */
    JceShapedGlyph   empty;
} s_cache;

/* FNV-1a over the bytes, mixed with the owner AND the chain generation.  The
 * owner is in the HASH as well as in the comparison so two fonts drawing the
 * same label land in different slots instead of evicting each other every
 * frame; the generation is in it because adding a fallback changes what the
 * same bytes shape to, and a stale run would be served forever otherwise. */
static uint64_t shape_hash(const void *owner, uint32_t gen,
                           const char *text, uint32_t len)
{
    uint64_t h = 1469598103934665603ull;
    uintptr_t o = (uintptr_t)owner;
    for (unsigned i = 0; i < sizeof o; i++) {
        h ^= (uint64_t)((o >> (i * 8)) & 0xffu);
        h *= 1099511628211ull;
    }
    for (unsigned i = 0; i < sizeof gen; i++) {
        h ^= (uint64_t)((gen >> (i * 8)) & 0xffu);
        h *= 1099511628211ull;
    }
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)text[i];
        h *= 1099511628211ull;
    }
    /* 0 marks an empty slot, so it cannot also be a valid hash. */
    return h ? h : 1ull;
}

static void slot_release(ShapeSlot *s)
{
    JCE_FREE(s->text);
    JCE_FREE(s->glyphs);
    memset(s, 0, sizeof *s);
}

/* Decode one UTF-8 codepoint, advancing `p`.  SDL_StepUTF8 is what jce_text.c
 * used before this module existed; using the same decoder means the run split
 * cannot disagree with the draw loop about where a character starts. */
static uint32_t next_cp(const char **p)
{
    return (uint32_t)SDL_StepUTF8(p, NULL);
}

/* Append the shaped glyphs of ONE run to `out`, growing it.
 *
 * `run_off` is the run's byte offset inside the whole string, and it has to be
 * added back to every cluster: HarfBuzz reports clusters relative to the buffer
 * it was given, so a second run's clusters would otherwise decode from the
 * start of the string and every fallback glyph would come out as some other
 * character.  This is the one place itemization can go wrong silently -- the
 * glyph COUNT and the advances stay right, only the codepoints are somebody
 * else's. */
static bool append_run(JceShapedGlyph **out, uint32_t *count, uint32_t *cap,
                       const char *whole, uint32_t run_off, uint32_t run_len,
                       hb_font_t *font, uint8_t slot)
{
    if (!font || run_len == 0) return true;

    hb_buffer_reset(s_cache.buf);
    hb_buffer_add_utf8(s_cache.buf, whole + run_off, (int)run_len, 0,
                       (int)run_len);
    hb_buffer_guess_segment_properties(s_cache.buf);
    hb_shape(font, s_cache.buf, NULL, 0);

    unsigned int n = 0;
    hb_glyph_info_t     *gi = hb_buffer_get_glyph_infos(s_cache.buf, &n);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(s_cache.buf, &n);
    if (n == 0) return true;
    if (!gi || !gp) return false;

    if (*count + n > *cap) {
        uint32_t want = *cap ? *cap * 2u : 32u;
        while (want < *count + n) want *= 2u;
        JceShapedGlyph *grown = (JceShapedGlyph *)JCE_MALLOC(
            (size_t)want * sizeof(JceShapedGlyph));
        if (!grown) return false;
        if (*count) memcpy(grown, *out, (size_t)*count * sizeof(JceShapedGlyph));
        JCE_FREE(*out);
        *out = grown;
        *cap = want;
    }

    for (unsigned int i = 0; i < n; i++) {
        const char *p = whole + run_off + gi[i].cluster;
        JceShapedGlyph *g = &(*out)[(*count)++];
        g->codepoint = next_cp(&p);
        g->x_offset  = gp[i].x_offset;
        g->y_offset  = gp[i].y_offset;
        g->x_advance = gp[i].x_advance;
        g->font_slot = slot;
    }
    return true;
}

/* Shape into `s`, replacing whatever it held.  Returns false and leaves the
 * slot empty when anything fails, so a caller that checks the return can never
 * read a half-filled run.
 *
 * THE STRING IS SPLIT FIRST.  Each run is the longest span of codepoints the
 * SAME slot covers, and runs are shaped independently with their own font --
 * HarfBuzz shapes with exactly one font, so this is not an optimisation, it is
 * the only way a fallback can contribute real metrics.  A version that shaped
 * the whole string with the primary and merely rasterised the missing glyphs
 * elsewhere would put full-width CJK glyphs on Latin advances. */
static bool slot_fill(ShapeSlot *s, const void *owner,
                      const JceTextFontChain *chain,
                      const char *text, uint32_t len)
{
    if (!s_cache.buf) {
        s_cache.buf = hb_buffer_create();
        if (!s_cache.buf) return false;
    }

    char *tcopy = (char *)JCE_MALLOC(len + 1u);
    if (!tcopy) return false;
    memcpy(tcopy, text, len);
    tcopy[len] = '\0';

    JceShapedGlyph *runs = NULL;
    uint32_t count = 0, cap = 0;
    bool ok = true;

    uint32_t run_off = 0;
    uint8_t  run_slot = 0;
    bool     have_run = false;
    const char *p = tcopy;

    while (*p) {
        const char *at = p;
        uint32_t cp = next_cp(&p);
        if (cp == 0) break;
        uint8_t slot = chain->slot_for_codepoint
                     ? chain->slot_for_codepoint(chain->ctx, cp) : 0u;
        if (!have_run) {
            run_off  = (uint32_t)(at - tcopy);
            run_slot = slot;
            have_run = true;
        } else if (slot != run_slot) {
            ok = append_run(&runs, &count, &cap, tcopy, run_off,
                            (uint32_t)(at - tcopy) - run_off,
                            chain->font_for_slot
                                ? chain->font_for_slot(chain->ctx, run_slot)
                                : NULL,
                            run_slot);
            if (!ok) break;
            run_off  = (uint32_t)(at - tcopy);
            run_slot = slot;
        }
    }
    if (ok && have_run)
        ok = append_run(&runs, &count, &cap, tcopy, run_off,
                        (uint32_t)(p - tcopy) - run_off,
                        chain->font_for_slot
                            ? chain->font_for_slot(chain->ctx, run_slot)
                            : NULL,
                        run_slot);

    if (!ok) {
        JCE_FREE(tcopy);
        JCE_FREE(runs);
        return false;
    }

    slot_release(s);
    s->hash       = shape_hash(owner, chain->generation, text, len);
    s->owner      = owner;
    s->generation = chain->generation;
    s->text       = tcopy;
    s->text_len   = len;
    s->glyphs     = runs;
    s->count      = count;
    s->cap        = cap;
    return true;
}

const JceShapedGlyph *jce_text_shape(const void *owner,
                                     const JceTextFontChain *chain,
                                     const char *text, uint32_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!chain || !chain->font_for_slot || !text) return NULL;
    if (!chain->font_for_slot(chain->ctx, 0)) return NULL;

    const size_t raw = strlen(text);
    if (raw > (size_t)0x7fffffff) return NULL;
    const uint32_t len = (uint32_t)raw;

    if (len > SHAPE_MAX_TEXT) {
        if (!slot_fill(&s_cache.bypass, owner, chain, text, len)) return NULL;
        s_cache.misses++;
        s_cache.last = &s_cache.bypass;
        if (out_count) *out_count = s_cache.bypass.count;
        return s_cache.bypass.glyphs ? s_cache.bypass.glyphs : &s_cache.empty;
    }

    const uint64_t h = shape_hash(owner, chain->generation, text, len);
    ShapeSlot *s = &s_cache.slots[(size_t)(h & (SHAPE_SLOTS - 1))];

    if (s->hash == h && s->owner == owner &&
        s->generation == chain->generation && s->text_len == len &&
        memcmp(s->text, text, len) == 0) {
        s_cache.hits++;
        s_cache.last = s;
        if (out_count) *out_count = s->count;
        return s->glyphs ? s->glyphs : &s_cache.empty;
    }

    /* A collision would evict the run the caller is still holding from the
     * PREVIOUS call, which is the one thing the header promises not to do. */
    if (s == s_cache.last) s = &s_cache.bypass;

    if (!slot_fill(s, owner, chain, text, len)) return NULL;
    s_cache.misses++;
    s_cache.last = s;
    if (out_count) *out_count = s->count;
    return s->glyphs ? s->glyphs : &s_cache.empty;
}

void jce_text_shape_forget(const void *owner)
{
    for (int i = 0; i < SHAPE_SLOTS; i++) {
        if (s_cache.slots[i].hash && s_cache.slots[i].owner == owner) {
            if (s_cache.last == &s_cache.slots[i]) s_cache.last = NULL;
            slot_release(&s_cache.slots[i]);
        }
    }
    if (s_cache.bypass.hash && s_cache.bypass.owner == owner) {
        if (s_cache.last == &s_cache.bypass) s_cache.last = NULL;
        slot_release(&s_cache.bypass);
    }
}

void jce_text_shape_shutdown(void)
{
    for (int i = 0; i < SHAPE_SLOTS; i++)
        slot_release(&s_cache.slots[i]);
    slot_release(&s_cache.bypass);
    if (s_cache.buf) { hb_buffer_destroy(s_cache.buf); s_cache.buf = NULL; }
    s_cache.last = NULL;
    s_cache.hits = s_cache.misses = 0;
}

void jce_text_shape_stats(uint64_t *out_hits, uint64_t *out_misses)
{
    if (out_hits)   *out_hits   = s_cache.hits;
    if (out_misses) *out_misses = s_cache.misses;
}
