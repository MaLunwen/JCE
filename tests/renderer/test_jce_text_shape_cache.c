/*
 * test_jce_text_shape_cache.c -- the shape cache must not change the answer,
 * and it must actually stop calling HarfBuzz.
 *
 * WHY BOTH HALVES, AND WHY THIS ORDER.  A cache that returns the wrong run is
 * caught by looking at the screen; a cache that returns the RIGHT run and
 * still re-shapes every frame is not caught by anything, because the pixels
 * are identical.  That second failure is the whole reason the counters exist,
 * and it is the one this file exists to make visible.
 *
 * WHY IT TALKS TO HarfBuzz DIRECTLY.  jce_text_shape takes an hb_font_t, not a
 * JceFont, precisely so it can be tested: a JceFont needs a rasterised bgfx
 * atlas, which is why the neighbouring test_jce_text_unicode_backend.c already
 * says "the JCE path needs a live bgfx atlas and cannot run headless".
 *
 * WHAT THIS FILE KILLS AND WHAT IT DOES NOT, measured rather than claimed.
 * Nine mutations were injected into jce_text_shape.c and this suite re-run
 * against each.  SIX DIE:
 *
 *   - the run offset is not added back to the cluster  -> the fallback run
 *     decodes from the start of the string, and the glyph COUNT and the
 *     ADVANCES stay perfectly right while every character is somebody else's
 *   - every glyph is tagged slot 0 (the fallback is ignored)  -> 3 tests
 *   - the string is never split into runs
 *   - the cache never hits (re-shapes every frame)     -> 3 tests
 *   - forget() does nothing
 *   - a long string is refused instead of shaped
 *
 * THREE SURVIVE, and none of them is a defect on its own:
 *
 *   1. Decoding the codepoint from the CALLER's buffer instead of the cache's
 *      copy.  Equivalent code -- the decode happens during the call, when both
 *      buffers hold the same bytes.  The risk it models (keeping a POINTER and
 *      decoding later) is not reachable by a one-line change, and
 *      test_the_run_does_not_refer_into_the_callers_buffer asserts the property
 *      directly instead.
 *
 *   2. Dropping the chain generation from the hash, OR from the comparison.
 *      Either alone still separates two chains, because the other half does;
 *      removing BOTH was injected as a ninth mutation and IS caught
 *      (test_a_new_chain_generation_is_a_new_run).  Redundant halves of one
 *      guard, kept because a hash collision is what makes the comparison
 *      matter.  The same shape as `s->owner == owner`, below.
 *
 *   3. Taking every glyph's advance from glyph 0 instead of glyph i.  Real,
 *      and invisible HERE because every advance in the reachable fixture is
 *      zero: there is no font file in this repository (`git ls-files` for
 *      .ttf/.otf is empty), reading the host's C:/Windows/Fonts would make the
 *      test about the machine, and an hb_font_t assembled out of font funcs did
 *      not work either -- the shaper maps glyphs through it correctly (glyph id
 *      72 for 'H', verified) and still positions every one at advance 0, with
 *      the singular AND plural advance funcs set and an explicit
 *      hb_font_set_scale.  Left uncovered rather than papered over, because it
 *      is the OTHER failure class this file opens with: a run whose advances
 *      all come from the first glyph stacks every character on one pixel, and
 *      is caught by opening the editor once.
 *
 * x_offset and y_offset are zero for the same reason as the advances and carry
 * the same caveat: compared against the reference, so a wrong SOURCE is
 * caught, a wrong VALUE could not be.
 */

#include "unity.h"

#include "renderer/jce_text_shape.h"

#include <hb.h>

#include <stdlib.h>
#include <time.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { jce_text_shape_shutdown(); }

static const char *OWNER_A = "font-a";
static const char *OWNER_B = "font-b";

/* -- the font chain, as the shaper sees it ---------------------------
 *
 * The real chain asks FreeType "does this face have a glyph for U+4E2D".
 * Headless there is no face, so the coverage question is answered by a table
 * instead -- which is also what makes the ITEMIZATION testable at all: the
 * split into runs is decided entirely by this answer, so a fake one exercises
 * the same code the real one drives. */
typedef struct {
    /* Codepoints slot 0 does NOT have; anything else is the primary's. */
    const uint32_t *missing;
    uint32_t        missing_count;
    uint8_t         slot_for_missing;   /* which fallback claims them */
} FakeChain;

static uint8_t fake_slot(void *ctx, uint32_t cp)
{
    const FakeChain *f = (const FakeChain *)ctx;
    for (uint32_t i = 0; i < f->missing_count; i++)
        if (f->missing[i] == cp) return f->slot_for_missing;
    return 0u;
}

static hb_font_t *fake_font(void *ctx, uint8_t slot)
{
    (void)ctx; (void)slot;
    /* One font for every slot: this test is about the SPLIT and the slot
     * tagging, not about two fonts producing different metrics -- which is
     * what the header's note about advances explains cannot be tested here. */
    return hb_font_get_empty();
}

static JceTextFontChain chain_of(FakeChain *f, uint32_t generation)
{
    JceTextFontChain c;
    c.ctx                = f;
    c.slot_for_codepoint = f ? fake_slot : NULL;
    c.font_for_slot      = fake_font;
    c.generation         = generation;
    return c;
}

static JceTextFontChain plain_chain(void)
{
    return chain_of(NULL, 0u);
}

/* Shape the same input with a fresh HarfBuzz buffer -- the code the cache
 * replaced -- so the assertion compares against the ORIGINAL, not against a
 * second run of the thing under test. */
static unsigned int shape_reference(const char *text,
                                    hb_glyph_info_t **out_info,
                                    hb_glyph_position_t **out_pos,
                                    hb_buffer_t **out_buf)
{
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf8(b, text, -1, 0, -1);
    hb_buffer_guess_segment_properties(b);
    hb_shape(hb_font_get_empty(), b, NULL, 0);
    unsigned int n = 0;
    *out_info = hb_buffer_get_glyph_infos(b, &n);
    *out_pos  = hb_buffer_get_glyph_positions(b, &n);
    *out_buf  = b;
    return n;
}

static void test_the_cached_run_equals_what_harfbuzz_produced(void)
{
    const JceTextFontChain c = plain_chain();
    const char *text = "Hello, world";

    hb_glyph_info_t *gi; hb_glyph_position_t *gp; hb_buffer_t *b;
    unsigned int want = shape_reference(text, &gi, &gp, &b);

    uint32_t got = 0;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c,
                                               text, &got);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(want, got);
    /* The per-glyph CODEPOINT assertions below are the ones that carry this
     * test (see the header for why the advances cannot), so check they are not
     * vacuous: a string whose characters were all identical would make them
     * pass whatever the cache did. */
    TEST_ASSERT_TRUE_MESSAGE(want >= 3 &&
        !(run[0].codepoint == run[1].codepoint &&
          run[1].codepoint == run[2].codepoint),
        "every glyph in the fixture string has the same codepoint, so the "
        "per-glyph assertions below cannot fail");
    for (unsigned int i = 0; i < want; i++) {
        TEST_ASSERT_EQUAL_INT32(gp[i].x_offset,  run[i].x_offset);
        TEST_ASSERT_EQUAL_INT32(gp[i].y_offset,  run[i].y_offset);
        TEST_ASSERT_EQUAL_INT32(gp[i].x_advance, run[i].x_advance);
        /* The codepoint is decoded from the cluster; for pure ASCII the
         * cluster IS the byte index, so this is the character itself. */
        TEST_ASSERT_EQUAL_UINT32((uint32_t)(unsigned char)text[gi[i].cluster],
                                 run[i].codepoint);
    }
    hb_buffer_destroy(b);
}

/* THE POINT OF THE WHOLE MODULE.  Without this the feature is invisible. */
static void test_the_second_lookup_does_not_run_harfbuzz(void)
{
    const JceTextFontChain c = plain_chain();
    uint64_t h0, m0, h1, m1;
    jce_text_shape_stats(&h0, &m0);

    uint32_t n1 = 0, n2 = 0;
    const JceShapedGlyph *a = jce_text_shape(OWNER_A, &c,
                                             "Play", &n1);
    const JceShapedGlyph *b = jce_text_shape(OWNER_A, &c,
                                             "Play", &n2);
    jce_text_shape_stats(&h1, &m1);

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_PTR(a, b);          /* same storage, not a re-shape */
    TEST_ASSERT_EQUAL_UINT32(n1, n2);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(m0 + 1, m1,
        "the second lookup of an unchanged string ran HarfBuzz again -- which "
        "is exactly the state this cache was written to end, and it produces "
        "identical pixels, so nothing else in this tree would notice");
    TEST_ASSERT_EQUAL_UINT64(h0 + 1, h1);
}

static void test_a_different_string_is_a_different_run(void)
{
    const JceTextFontChain c = plain_chain();
    uint32_t na = 0, nb = 0;
    const JceShapedGlyph *a = jce_text_shape(OWNER_A, &c,
                                             "Play", &na);
    uint32_t first = a[0].codepoint;
    const JceShapedGlyph *b = jce_text_shape(OWNER_A, &c,
                                             "Quit", &nb);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)'P', first);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)'Q', b[0].codepoint);
}

/* Two fonts drawing the same label must not be one entry: the owner is part
 * of the key because the same bytes shape differently per font. */
/* NOT FULLY COVERED, and the mutation run says so: deleting `s->owner ==
 * owner` from the hit comparison does not fail this test, because the OWNER IS
 * ALSO IN THE HASH, so two owners land in different slots and the comparison
 * clause is never the thing that separates them.  It only matters on a slot
 * collision, and a collision cannot be engineered from outside: the key mixes
 * the owner POINTER, which moves between runs.  The clause stays because it is
 * what makes a collision safe; this test covers the hash, not the clause. */
static void test_two_owners_do_not_share_an_entry(void)
{
    const JceTextFontChain c = plain_chain();
    uint64_t h0, m0, h1, m1;
    uint32_t n = 0;
    jce_text_shape(OWNER_A, &c, "Options", &n);
    jce_text_shape_stats(&h0, &m0);
    jce_text_shape(OWNER_B, &c, "Options", &n);
    jce_text_shape_stats(&h1, &m1);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(m0 + 1, m1,
        "a second font reused the first font's shaped run for the same bytes");
    TEST_ASSERT_EQUAL_UINT64(h0, h1);
}

/* A closed font's runs must go with it -- the key is its address, and the next
 * allocation can land there. */
static void test_forget_drops_that_owners_runs(void)
{
    const JceTextFontChain c = plain_chain();
    uint32_t n = 0;
    jce_text_shape(OWNER_A, &c, "Resume", &n);
    jce_text_shape(OWNER_B, &c, "Resume", &n);

    jce_text_shape_forget(OWNER_A);

    uint64_t h0, m0, h1, m1;
    jce_text_shape_stats(&h0, &m0);
    jce_text_shape(OWNER_A, &c, "Resume", &n);   /* must miss */
    jce_text_shape_stats(&h1, &m1);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(m0 + 1, m1,
        "a forgotten owner's run was still served -- a closed font's entries "
        "would then be handed to whatever is allocated at its address next");

    jce_text_shape_stats(&h0, &m0);
    jce_text_shape(OWNER_B, &c, "Resume", &n);   /* must hit */
    jce_text_shape_stats(&h1, &m1);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(h0 + 1, h1,
        "forgetting one owner dropped another owner's runs");
}

/* The bound has to exist (the key is the string, so an uncapped key is an
 * uncapped allocation), and crossing it must still SHAPE, not refuse. */
static void test_a_very_long_string_still_shapes(void)
{
    const JceTextFontChain c = plain_chain();
    char big[900];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';

    uint32_t n = 0;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c,
                                               big, &n);
    TEST_ASSERT_NOT_NULL_MESSAGE(run,
        "a string past the cache bound was refused instead of shaped");
    TEST_ASSERT_EQUAL_UINT32(sizeof big - 1, n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)'a', run[0].codepoint);

    /* ...and it must not be cached, or the bound would not be a bound. */
    uint64_t h0, m0, h1, m1;
    jce_text_shape_stats(&h0, &m0);
    jce_text_shape(OWNER_A, &c, big, &n);
    jce_text_shape_stats(&h1, &m1);
    TEST_ASSERT_EQUAL_UINT64(m0 + 1, m1);
}

/* THE RUN MUST OUTLIVE THE STRING IT WAS SHAPED FROM.
 *
 * The codepoint is decoded at shape time so a cached run does not refer into
 * the caller's buffer; both call sites in jce_text.c hand this function a
 * pointer they do not own for longer than the call.  A version that kept
 * decoding from the caller's memory survived every other test in this file,
 * because every other test shapes a string literal that never goes away. */
static void test_the_run_does_not_refer_into_the_callers_buffer(void)
{
    const JceTextFontChain c = plain_chain();
    char *heap = (char *)malloc(8);
    TEST_ASSERT_NOT_NULL(heap);
    memcpy(heap, "Zebra", 6);

    uint32_t n = 0;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c, heap, &n);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(5, n);

    memset(heap, '!', 6);          /* the caller reuses its buffer */
    free(heap);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)'Z', run[0].codepoint,
        "the cached run still reads the caller's string, which the caller is "
        "free to overwrite or free the moment this function returns");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)'b', run[2].codepoint);
}

/* An empty string is a legal input with a legal, empty result.  NULL means
 * "unusable arguments" and must keep meaning only that. */
static void test_an_empty_string_is_a_run_of_zero_not_a_failure(void)
{
    const JceTextFontChain c = plain_chain();
    uint32_t n = 7;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c,
                                               "", &n);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(0, n);

    n = 7;
    TEST_ASSERT_NULL(jce_text_shape(OWNER_A, NULL, "x", &n));
    TEST_ASSERT_EQUAL_UINT32(0, n);
    n = 7;
    TEST_ASSERT_NULL(jce_text_shape(OWNER_A, &c, NULL, &n));
    TEST_ASSERT_EQUAL_UINT32(0, n);
}

/* THE SAVING, AS A NUMBER, WITH A DELIBERATELY LOOSE THRESHOLD.
 *
 * A count assertion ("the second lookup did not run HarfBuzz") proves the
 * cache works; it does not prove the cache is worth having.  This times the
 * two paths over the same strings.
 *
 * The threshold is 2x and the measured ratio is far above it, on purpose: this
 * runs against hb_font_get_empty(), which is the CHEAPEST shape HarfBuzz can
 * perform -- no cmap, no GSUB, no GPOS, no kerning.  A real font does strictly
 * more work per shape and none of it per cache hit, so this is a lower bound
 * on the saving and a threshold that cannot go flaky on a slow machine.
 *
 * A timing assertion has to justify itself in a suite that runs on whatever
 * hardware is present.  This one measures a RATIO between two loops in the
 * same process, back to back, so it is insensitive to clock speed.  MEASURED
 * on this machine over three runs at 200k iterations: 14.3x, 19.0x and 20.0x,
 * against a threshold of 2x -- seven times the margin, and every one of those
 * numbers is a lower bound because the font is the empty one. */
static void test_a_cache_hit_is_much_cheaper_than_a_shape(void)
{
    const JceTextFontChain c = plain_chain();
    enum { ITERS = 200000 };
    static const char *const LABELS[] = {
        "Play", "Options", "Quit", "Resume", "Save Game", "Load Game",
        "Graphics", "Audio", "Controls", "Back"
    };
    enum { NLABELS = (int)(sizeof LABELS / sizeof LABELS[0]) };

    /* Warm every entry so the timed loop is all hits. */
    uint32_t n = 0;
    for (int i = 0; i < NLABELS; i++)
        jce_text_shape(OWNER_A, &c, LABELS[i], &n);

    clock_t t0 = clock();
    for (int k = 0; k < ITERS; k++)
        jce_text_shape(OWNER_A, &c, LABELS[k % NLABELS], &n);
    clock_t t1 = clock();

    /* The path this replaced, verbatim: a fresh buffer and a full shape. */
    for (int k = 0; k < ITERS; k++) {
        hb_buffer_t *b = hb_buffer_create();
        hb_buffer_add_utf8(b, LABELS[k % NLABELS], -1, 0, -1);
        hb_buffer_guess_segment_properties(b);
        hb_shape(hb_font_get_empty(), b, NULL, 0);
        hb_buffer_destroy(b);
    }
    clock_t t2 = clock();

    double cached = (double)(t1 - t0);
    double shaped = (double)(t2 - t1);
    TEST_ASSERT_TRUE_MESSAGE(shaped > 0.0,
        "the reference loop took no measurable time -- raise ITERS");
    TEST_ASSERT_TRUE_MESSAGE(cached * 2.0 < shaped,
        "a cache hit is not even twice as cheap as re-shaping, against the "
        "cheapest font HarfBuzz has -- the cache is not paying for itself");
}

/* -- itemization: the half that ends tofu ---------------------------- */

/* A string the primary fully covers must stay ONE run in slot 0, or every
 * existing Latin string would start paying for a split it does not need. */
static void test_a_covered_string_is_all_primary(void)
{
    static const uint32_t none[1] = { 0 };
    FakeChain f = { none, 0, 1 };
    JceTextFontChain c = chain_of(&f, 1u);

    uint32_t n = 0;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c, "Menu", &n);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    for (uint32_t i = 0; i < n; i++)
        TEST_ASSERT_EQUAL_UINT8(0, run[i].font_slot);
}

/* THE POINT.  A codepoint the primary lacks is tagged with the fallback that
 * claims it -- and the codepoints on BOTH sides of the switch must still be
 * the right ones, which is where a per-run cluster offset goes wrong: HarfBuzz
 * reports clusters relative to the buffer it was handed, so a second run whose
 * offset was not added back decodes from the start of the string and every
 * fallback glyph comes out as some other character, with the count and the
 * advances still perfectly right. */
static void test_an_uncovered_codepoint_switches_slot(void)
{
    /* "A" U+4E2D "B" -- one ASCII, one CJK the primary does not have, one
     * ASCII, so the split happens twice and the second Latin run is the one
     * whose clusters have to be rebased. */
    static const uint32_t missing[] = { 0x4E2Du };
    FakeChain f = { missing, 1, 2 };
    JceTextFontChain c = chain_of(&f, 1u);

    uint32_t n = 0;
    /* Split at the 'B': a hex escape is GREEDY, so "\xADB" is one character
     * constant of 0x2779 and not U+4E2D followed by 'B'.  MSVC says so; a
     * compiler that merely truncated would have shipped a different string
     * than this test claims to be about. */
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c,
                                               "A\xE4\xB8\xAD" "B", &n);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(3, n);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)'A', run[0].codepoint);
    TEST_ASSERT_EQUAL_UINT8(0, run[0].font_slot);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0x4E2Du, run[1].codepoint,
        "the fallback run decoded the wrong character -- its clusters are "
        "relative to its own buffer and the run offset was not added back");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, run[1].font_slot,
        "the codepoint the primary lacks was not handed to the fallback, so "
        "it will be rasterised out of the primary's atlas as a tofu box");

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)'B', run[2].codepoint,
        "the run AFTER the switch decoded from the wrong offset");
    TEST_ASSERT_EQUAL_UINT8(0, run[2].font_slot);
}

/* Consecutive uncovered codepoints are ONE run, not one run each: a paragraph
 * of Chinese must not become one HarfBuzz shape per character. */
static void test_consecutive_uncovered_codepoints_are_one_run(void)
{
    static const uint32_t missing[] = { 0x4E2Du, 0x6587u };
    FakeChain f = { missing, 2, 1 };
    JceTextFontChain c = chain_of(&f, 1u);

    uint64_t m0, m1, h;
    jce_text_shape_stats(&h, &m0);
    uint32_t n = 0;
    const JceShapedGlyph *run = jce_text_shape(OWNER_A, &c,
                                               "\xE4\xB8\xAD\xE6\x96\x87", &n);
    jce_text_shape_stats(&h, &m1);
    TEST_ASSERT_NOT_NULL(run);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, run[0].font_slot);
    TEST_ASSERT_EQUAL_UINT8(1, run[1].font_slot);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(m0 + 1, m1,
        "the lookup counted as more than one cache miss");
}

/* Adding a fallback changes what the same bytes shape to.  A run cached
 * against the old chain must not be served, or a label drawn before the
 * fallback was added keeps its tofu for the life of the process. */
static void test_a_new_chain_generation_is_a_new_run(void)
{
    static const uint32_t missing[] = { 0x4E2Du };
    FakeChain before = { missing, 0, 1 };   /* nothing missing yet */
    FakeChain after  = { missing, 1, 1 };   /* the fallback now claims it */

    JceTextFontChain c0 = chain_of(&before, 1u);
    uint32_t n = 0;
    const JceShapedGlyph *a = jce_text_shape(OWNER_A, &c0, "\xE4\xB8\xAD", &n);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(0, a[0].font_slot);

    JceTextFontChain c1 = chain_of(&after, 2u);
    const JceShapedGlyph *b = jce_text_shape(OWNER_A, &c1, "\xE4\xB8\xAD", &n);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, b[0].font_slot,
        "the run cached against the previous chain was served after a "
        "fallback was added -- the tofu would never go away");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_cached_run_equals_what_harfbuzz_produced);
    RUN_TEST(test_the_second_lookup_does_not_run_harfbuzz);
    RUN_TEST(test_a_different_string_is_a_different_run);
    RUN_TEST(test_two_owners_do_not_share_an_entry);
    RUN_TEST(test_forget_drops_that_owners_runs);
    RUN_TEST(test_a_very_long_string_still_shapes);
    RUN_TEST(test_the_run_does_not_refer_into_the_callers_buffer);
    RUN_TEST(test_an_empty_string_is_a_run_of_zero_not_a_failure);
    RUN_TEST(test_a_covered_string_is_all_primary);
    RUN_TEST(test_an_uncovered_codepoint_switches_slot);
    RUN_TEST(test_consecutive_uncovered_codepoints_are_one_run);
    RUN_TEST(test_a_new_chain_generation_is_a_new_run);
    RUN_TEST(test_a_cache_hit_is_much_cheaper_than_a_shape);
    return UNITY_END();
}
