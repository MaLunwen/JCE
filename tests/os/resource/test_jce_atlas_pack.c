/*
 * test_jce_atlas_pack.c — the properties an atlas has to have, as arithmetic.
 *
 * Every one of these fails INVISIBLY in a picture, which is why the packer is
 * pure and why they are asserted here rather than looked at:
 *
 *   overlap ......... two sprites sharing a pixel draw one over the other.
 *                     At 1px it looks like an artist's edge; at 20px it looks
 *                     like the wrong sprite, and neither reads as "the packer
 *                     is wrong".
 *   containment ..... a rectangle past the atlas edge samples whatever the
 *                     texture wraps to, which on a repeating sampler is the
 *                     opposite edge -- a sprite with someone else's colour
 *                     down one side.
 *   padding ......... bilinear sampling reaches half a texel outside the
 *                     rectangle, so 0 padding bleeds a neighbour's colour
 *                     along the seam.  This is the artefact people report as
 *                     "the sprite has a line on it".
 *   determinism ..... an atlas that reshuffles when a directory lists in a
 *                     different order rewrites every sprite's UV on a cook
 *                     that changed nothing.  This tree gates reproducible
 *                     builds.
 *   refusal ......... an oversized sprite must come back placed=false.  A
 *                     silently dropped sprite renders as nothing, which looks
 *                     like an authoring mistake and is not one.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/resource/jce_atlas_pack.h>

#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

enum { N = 64 };

static JceAtlasItem      g_items[N];
static JceAtlasPlacement g_out[N];

/* A spread of sizes, deliberately including duplicates so the id tiebreak in
 * the sort is exercised rather than assumed unreachable. */
static size_t make_items(void)
{
    static const uint32_t w[] = { 64, 16, 33, 8, 64, 128, 7, 33, 250, 12 };
    static const uint32_t h[] = { 64, 48, 33, 8, 64,  16, 90, 33,  10, 12 };
    const size_t n = sizeof(w) / sizeof(w[0]);
    for (size_t i = 0; i < n; ++i) {
        g_items[i].id = (uint32_t)i;
        g_items[i].w  = w[i];
        g_items[i].h  = h[i];
    }
    return n;
}

static bool overlaps(const JceAtlasPlacement *a, const JceAtlasPlacement *b)
{
    if (!a->placed || !b->placed) return false;
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

static void test_nothing_overlaps_and_everything_is_inside(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    uint32_t aw = 0, ah = 0;
    const size_t n = make_items();

    const size_t placed = jce_atlas_pack(g_items, n, &d, g_out, &aw, &ah);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(n, placed,
        "every one of these fits in 4096x4096; a miss means the packer gave "
        "up, not that the atlas was full");

    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(g_out[i].placed);
        TEST_ASSERT_TRUE_MESSAGE(g_out[i].x + g_out[i].w <= aw,
            "a sprite runs past the right edge; on a repeating sampler it "
            "shows the opposite edge's pixels down one side");
        TEST_ASSERT_TRUE_MESSAGE(g_out[i].y + g_out[i].h <= ah,
            "a sprite runs past the bottom edge");
        for (size_t j = i + 1; j < n; ++j)
            TEST_ASSERT_FALSE_MESSAGE(overlaps(&g_out[i], &g_out[j]),
                "two sprites share pixels -- at one pixel this looks like an "
                "artist's edge, which is why it is asserted and not looked at");
    }
}

static void test_the_reported_extent_is_the_used_one(void)
{
    /* The caller allocates out_w x out_h, not max_width x max_height.  If the
     * extent were the max, every atlas would be 4096 square and a project
     * with six sprites would ship 64 MB of transparent pixels. */
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    uint32_t aw = 0, ah = 0;
    const size_t n = make_items();

    jce_atlas_pack(g_items, n, &d, g_out, &aw, &ah);
    TEST_ASSERT_TRUE_MESSAGE(aw > 0 && aw < d.max_width,
        "the reported width is the max, not the used extent");
    TEST_ASSERT_TRUE(ah > 0 && ah < d.max_height);

    /* And it really is a bound: nothing sits outside it (checked above) and
     * something touches it, or the extent is padded with nothing. */
    uint32_t max_r = 0, max_b = 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_out[i].x + g_out[i].w > max_r) max_r = g_out[i].x + g_out[i].w;
        if (g_out[i].y + g_out[i].h > max_b) max_b = g_out[i].y + g_out[i].h;
    }
    TEST_ASSERT_TRUE_MESSAGE(aw - max_r <= d.padding,
        "the atlas is wider than its content by more than the padding");
    TEST_ASSERT_TRUE(ah - max_b <= d.padding);
}

static void test_padding_separates_neighbours_and_the_edge(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    d.padding = 4u;
    uint32_t aw = 0, ah = 0;
    const size_t n = make_items();
    jce_atlas_pack(g_items, n, &d, g_out, &aw, &ah);

    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(g_out[i].x >= d.padding,
            "a sprite touches the left edge; bilinear sampling reaches half a "
            "texel outside it and picks up the wrap");
        TEST_ASSERT_TRUE(g_out[i].y >= d.padding);
    }

    /* Grow every rectangle by the padding and re-test for overlap: that is
     * what "separated by padding" means, and testing the gap pairwise any
     * other way is the same arithmetic written less clearly. */
    for (size_t i = 0; i < n; ++i) {
        JceAtlasPlacement a = g_out[i];
        a.w += d.padding; a.h += d.padding;
        for (size_t j = i + 1; j < n; ++j) {
            JceAtlasPlacement b = g_out[j];
            b.w += d.padding; b.h += d.padding;
            TEST_ASSERT_FALSE_MESSAGE(overlaps(&a, &b),
                "two sprites are closer than the padding; their colours bleed "
                "into each other along the seam");
        }
    }
}

static void test_input_order_does_not_change_the_atlas(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    JceAtlasPlacement first[N];
    uint32_t aw1 = 0, ah1 = 0, aw2 = 0, ah2 = 0;
    const size_t n = make_items();

    jce_atlas_pack(g_items, n, &d, first, &aw1, &ah1);

    /* Reverse the input.  A directory listing that enumerates the other way
     * must not move a single sprite, or a cook that changed nothing rewrites
     * every UV. */
    JceAtlasItem rev[N];
    for (size_t i = 0; i < n; ++i) rev[i] = g_items[n - 1 - i];
    jce_atlas_pack(rev, n, &d, g_out, &aw2, &ah2);

    TEST_ASSERT_EQUAL_UINT32(aw1, aw2);
    TEST_ASSERT_EQUAL_UINT32(ah1, ah2);
    for (size_t i = 0; i < n; ++i) {
        /* g_out is in REVERSED order, so item i of `first` is item n-1-i here. */
        const JceAtlasPlacement *a = &first[i];
        const JceAtlasPlacement *b = &g_out[n - 1 - i];
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->id, b->id,
            "placements are written in the CALLER's order, not the sorted one");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(a->x, b->x,
            "the same sprites in a different input order landed elsewhere; a "
            "cook that changed nothing would rewrite every UV");
        TEST_ASSERT_EQUAL_UINT32(a->y, b->y);
    }
}

static void test_an_oversized_sprite_is_refused_not_clipped(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    d.max_width  = 64u;
    d.max_height = 64u;
    uint32_t aw = 0, ah = 0;

    JceAtlasItem items[3];
    items[0].id = 0; items[0].w = 16; items[0].h = 16;
    items[1].id = 1; items[1].w = 500; items[1].h = 8;   /* wider than the atlas */
    items[2].id = 2; items[2].w = 8;  items[2].h = 16;

    const size_t placed = jce_atlas_pack(items, 3, &d, g_out, &aw, &ah);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2u, placed,
        "the oversized sprite must be refused, and the other two must still "
        "be placed -- one bad input is not a failed pack");
    TEST_ASSERT_TRUE(g_out[0].placed);
    TEST_ASSERT_FALSE_MESSAGE(g_out[1].placed,
        "an oversized sprite came back placed; clipped or wrapped, it is "
        "wrong in a way that looks like an authoring mistake");
    TEST_ASSERT_TRUE(g_out[2].placed);
    TEST_ASSERT_TRUE(aw <= d.max_width && ah <= d.max_height);
}

static void test_the_degenerate_inputs(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    uint32_t aw = 123, ah = 456;

    TEST_ASSERT_EQUAL_size_t(0u, jce_atlas_pack(NULL, 4, &d, g_out, &aw, &ah));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, aw,
        "a refused pack must report a zero extent, not leave the caller's "
        "variable holding whatever was in it");
    TEST_ASSERT_EQUAL_UINT32(0u, ah);

    JceAtlasItem zero = { 7u, 0u, 0u };
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0u,
        jce_atlas_pack(&zero, 1, &d, g_out, &aw, &ah),
        "a zero-sized sprite is not placed; it has no pixels to blit");
    TEST_ASSERT_FALSE(g_out[0].placed);

    /* NULL desc means the documented defaults, not a crash. */
    const size_t n = make_items();
    TEST_ASSERT_EQUAL_size_t(n, jce_atlas_pack(g_items, n, NULL, g_out, &aw, &ah));
}

static void test_power_of_two_rounds_up_and_stays_in_bounds(void)
{
    JceAtlasPackDesc d = jce_atlas_pack_desc_default();
    d.power_of_two = true;
    uint32_t aw = 0, ah = 0;
    const size_t n = make_items();
    jce_atlas_pack(g_items, n, &d, g_out, &aw, &ah);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, aw & (aw - 1u), "width is not a power of two");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, ah & (ah - 1u), "height is not a power of two");
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(g_out[i].x + g_out[i].w <= aw);
        TEST_ASSERT_TRUE(g_out[i].y + g_out[i].h <= ah);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_overlaps_and_everything_is_inside);
    RUN_TEST(test_the_reported_extent_is_the_used_one);
    RUN_TEST(test_padding_separates_neighbours_and_the_edge);
    RUN_TEST(test_input_order_does_not_change_the_atlas);
    RUN_TEST(test_an_oversized_sprite_is_refused_not_clipped);
    RUN_TEST(test_the_degenerate_inputs);
    RUN_TEST(test_power_of_two_rounds_up_and_stays_in_bounds);
    return UNITY_END();
}
