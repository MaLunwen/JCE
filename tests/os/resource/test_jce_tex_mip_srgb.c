/*
 * test_jce_tex_mip_srgb.c
 *
 * Mipmaps of an sRGB-encoded texture must be averaged in LINEAR light.
 *
 * The defect this locks down is invisible on the content that usually gets
 * tested. A box filter over raw sRGB bytes is EXACTLY correct wherever the
 * source is flat -- averaging four equal bytes returns that byte in any space
 * -- so a hand-built test image, a UI atlas, or a flat-shaded low-poly texture
 * all come out right and prove nothing. The error is a function of local
 * CONTRAST, and it only appears where the texture has any:
 *
 *     2x2 of {255, 255, 0, 0}   gamma-space 127   linear-space 188
 *     2x2 of {255, 0, 0, 0}     gamma-space  64   linear-space 137
 *     2x2 of {128,128,128,128}  both 128
 *
 * Measured across this repo's own textures before the fix: worst per-texel
 * error 73 levels, median texture's worst 27. On a leaf atlas the SIGNED mean
 * shift reaches -14.5 luma and +16 R-B by mip 7 -- distant foliage rendered
 * systematically darker and warmer than it should be, growing with mip level,
 * which is to say growing with distance.
 *
 * The tests therefore use contrast on purpose, and assert the flat case too --
 * because the fix must not disturb the case the old code got right.
 *
 * The linear/raw path matters just as much: normal maps, metallic-roughness,
 * occlusion and masks are NOT sRGB, and decoding them would corrupt data that
 * is currently correct. Every sRGB assertion below has a linear twin.
 */

#include "jce_tex_compress.h"
#include "jce_cook_policy.h"

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* A 2x2 RGBA8 image whose four texels take the given grey values. */
static void make2x2(uint8_t out[16], uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    const uint8_t v[4] = { a, b, c, d };
    for (int i = 0; i < 4; ++i) {
        out[i * 4 + 0] = v[i];
        out[i * 4 + 1] = v[i];
        out[i * 4 + 2] = v[i];
        out[i * 4 + 3] = 255;
    }
}

/* ── 1. THE POINT: maximum contrast, sRGB ──────────────────────────────
 *
 * Half white and half black. In linear light that is 0.5, which re-encodes to
 * 188 -- the mid-grey a human eye reads as halfway. The old code returned 127,
 * which is 61 levels too dark and is what makes a minified checkerboard, a
 * leaf canopy or a picket fence darken as it recedes. */

static void test_srgb_max_contrast(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    make2x2(src, 255, 255, 0, 0);
    jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, true);

    TEST_ASSERT_EQUAL_UINT32(1, w);
    TEST_ASSERT_EQUAL_UINT32(1, h);
    TEST_ASSERT_INT_WITHIN(1, 188, dst[0]);
    TEST_ASSERT_INT_WITHIN(1, 188, dst[1]);
    TEST_ASSERT_INT_WITHIN(1, 188, dst[2]);

    /* And it is NOT the gamma-space answer. Spelled out so the test fails
     * loudly rather than drifting if the tolerance above is ever widened. */
    TEST_ASSERT_TRUE(dst[0] > 160);
}

/* ── 2. The same input as LINEAR data must stay 127 ───────────────────
 *
 * A normal map with those bytes means two directions, not two brightnesses.
 * Averaging them in "linear light" would decode data that was never encoded. */

static void test_linear_data_is_untouched(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    make2x2(src, 255, 255, 0, 0);
    jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, false);
    TEST_ASSERT_EQUAL_UINT8(127, dst[0]);
    TEST_ASSERT_EQUAL_UINT8(127, dst[1]);
    TEST_ASSERT_EQUAL_UINT8(127, dst[2]);
}

/* ── 3. One bright texel in four ──────────────────────────────────────  */

static void test_srgb_single_bright_texel(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    make2x2(src, 255, 0, 0, 0);
    jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, true);
    TEST_ASSERT_INT_WITHIN(1, 137, dst[0]);

    jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, false);
    TEST_ASSERT_EQUAL_UINT8(63, dst[0]);
}

/* ── 4. Flat input is a no-op in either space ─────────────────────────
 *
 * The round trip through linear must not drift. This is also why the defect
 * survived so long: every flat region -- which is most of a stylised texture
 * atlas -- was already correct, so whole-image averages barely moved even
 * where individual texels were off by 70 levels. */

static void test_flat_input_round_trips(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    for (int v = 0; v <= 255; v += 17) {
        make2x2(src, (uint8_t)v, (uint8_t)v, (uint8_t)v, (uint8_t)v);
        jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, true);
        TEST_ASSERT_INT_WITHIN(1, v, dst[0]);
        jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, false);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)v, dst[0]);
    }
}

/* ── 5. ALPHA is never gamma-decoded, even on an sRGB texture ─────────
 *
 * sRGB describes the colour channels only; alpha is linear coverage. Decoding
 * it would change alpha-tested foliage coverage at every mip -- the canopy
 * would thin or thicken with distance, which is a worse artifact than the one
 * being fixed. */

static void test_alpha_stays_linear(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    memset(src, 0, sizeof src);
    for (int i = 0; i < 4; ++i) src[i * 4 + 0] = 128;   /* flat colour */
    src[0 * 4 + 3] = 255; src[1 * 4 + 3] = 255;
    src[2 * 4 + 3] = 0;   src[3 * 4 + 3] = 0;
    jce_tex_generate_mip_ex(src, 2, 2, dst, &w, &h, true);
    TEST_ASSERT_EQUAL_UINT8(127, dst[3]);              /* the linear mean */
}

/* ── 6. The old entry point still exists and still means "raw" ────────
 *
 * Callers that have not been given a semantic must keep the behaviour they
 * had. A silent switch to sRGB decoding would corrupt every normal map in the
 * project on the next cook, and nothing would report it. */

static void test_legacy_entry_point_is_raw(void)
{
    uint8_t src[16], dst[16];
    uint32_t w = 0, h = 0;
    make2x2(src, 255, 255, 0, 0);
    jce_tex_generate_mip(src, 2, 2, dst, &w, &h);
    TEST_ASSERT_EQUAL_UINT8(127, dst[0]);
}

/* ── 7. Non-square and 1-pixel edges still behave ─────────────────────
 *
 * The clamp at the last row/column predates this change and was itself a heap
 * overrun once; the sRGB path must not reintroduce it. */

static void test_degenerate_dimensions(void)
{
    uint8_t src[2 * 1 * 4], dst[2 * 1 * 4];
    uint32_t w = 0, h = 0;
    memset(src, 0, sizeof src);
    src[0] = 255; src[1] = 255; src[2] = 255; src[3] = 255;
    src[4] = 0;   src[5] = 0;   src[6] = 0;   src[7] = 255;
    jce_tex_generate_mip_ex(src, 2, 1, dst, &w, &h, true);
    TEST_ASSERT_EQUAL_UINT32(1, w);
    TEST_ASSERT_EQUAL_UINT32(1, h);
    TEST_ASSERT_INT_WITHIN(1, 188, dst[0]);
}

/* ── 8. The POLICY that decides which mode a texture gets ─────────────
 *
 * The classification is behaviour, so it is pinned here rather than left to
 * whoever next reads the suffix list. The polarity is the safety property:
 * an unknown name must answer FALSE, because calling a colour map linear
 * leaves it exactly as the cooker has always averaged it, while calling a
 * LINEAR map sRGB gamma-decodes values that were correct. */

static void test_policy_unknown_names_are_not_srgb(void)
{
    /* The shape 114 of this repository's textures actually have: GLB-embedded,
     * named after the model with an index. It says nothing, so it gets the
     * historical behaviour rather than a guess. */
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("assets/models/TreesPack_tex0.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("assets/models/PSX_BagMan_tex0.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("t.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb(NULL));
}

static void test_policy_colour_names_are_srgb(void)
{
    TEST_ASSERT_TRUE(jce_cook_path_is_srgb("a/rock_albedo.png"));
    TEST_ASSERT_TRUE(jce_cook_path_is_srgb("a/Rock_BaseColor.png"));
    TEST_ASSERT_TRUE(jce_cook_path_is_srgb("a/wood_diffuse.jpg"));
    TEST_ASSERT_TRUE(jce_cook_path_is_srgb("a/leaf_emissive.png"));
}

static void test_policy_linear_names_are_not_srgb(void)
{
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_normal.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_nrm.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_orm.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_roughness.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_ao.png"));
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/grade.lut.png"));

    /* Linear wins over a colour word in the same name: this is checked first
     * on purpose, because the alternative silently gamma-decodes a roughness
     * map that happened to be exported next to its colour map. */
    TEST_ASSERT_FALSE(jce_cook_path_is_srgb("a/rock_color_roughness.png"));
}

static void test_policy_matches_basename_not_directory(void)
{
    /* A colour texture under a directory called `normals/` used to be cooked
     * to BC5 -- a two-channel format -- and lose its blue channel outright.
     * The classification must come from the file, not the folder. */
    TEST_ASSERT_FALSE(jce_cook_path_is_normal_map("assets/normals/wood_albedo.png"));
    TEST_ASSERT_TRUE(jce_cook_path_is_srgb("assets/normals/wood_albedo.png"));
    TEST_ASSERT_TRUE(jce_cook_path_is_normal_map("assets/pbr/wood_normal.png"));
}

/* ── 9. The sidecar value the MODEL IMPORTER writes ───────────────────
 *
 * `<texture>.import.json` gets a "colorSpace" key beside every texture the
 * importer extracts from a GLB, tagged with the material slot it filled --
 * which is the only place in the pipeline that actually KNOWS.  Reading it is
 * what closes the gap for the 114 `<stem>_tex<N>.png` files whose names say
 * nothing.
 *
 * It is a NEW key on purpose.  The sidecar already carries an "srgb" boolean
 * written by the editor and read by nobody, defaulting to TRUE, so every
 * sidecar on disk already claims sRGB -- normal maps included.  Honouring THAT
 * key would corrupt precisely the data this change protects. */

static void test_colour_space_sidecar_values(void)
{
    bool v = false;

    TEST_ASSERT_TRUE(jce_cook_colour_space_parse("srgb", &v));
    TEST_ASSERT_TRUE(v);

    v = true;
    TEST_ASSERT_TRUE(jce_cook_colour_space_parse("linear", &v));
    TEST_ASSERT_FALSE(v);

    /* Authored by tools and by humans, so case must not matter. */
    v = false;
    TEST_ASSERT_TRUE(jce_cook_colour_space_parse("sRGB", &v));
    TEST_ASSERT_TRUE(v);
    v = true;
    TEST_ASSERT_TRUE(jce_cook_colour_space_parse("LINEAR", &v));
    TEST_ASSERT_FALSE(v);
}

static void test_colour_space_unknown_leaves_caller_alone(void)
{
    /* An unrecognised value must not be read as either of the two we know.
     * The caller's answer -- the filename heuristic -- survives untouched, so
     * a future third colour space degrades to today's behaviour instead of
     * being silently misfiled as one of these. */
    bool v = true;
    TEST_ASSERT_FALSE(jce_cook_colour_space_parse("rec2020", &v));
    TEST_ASSERT_TRUE(v);
    v = false;
    TEST_ASSERT_FALSE(jce_cook_colour_space_parse("rec2020", &v));
    TEST_ASSERT_FALSE(v);

    TEST_ASSERT_FALSE(jce_cook_colour_space_parse("", &v));
    TEST_ASSERT_FALSE(jce_cook_colour_space_parse(NULL, &v));
    TEST_ASSERT_FALSE(jce_cook_colour_space_parse("srgb", NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_colour_space_sidecar_values);
    RUN_TEST(test_colour_space_unknown_leaves_caller_alone);
    RUN_TEST(test_policy_unknown_names_are_not_srgb);
    RUN_TEST(test_policy_colour_names_are_srgb);
    RUN_TEST(test_policy_linear_names_are_not_srgb);
    RUN_TEST(test_policy_matches_basename_not_directory);
    RUN_TEST(test_srgb_max_contrast);
    RUN_TEST(test_linear_data_is_untouched);
    RUN_TEST(test_srgb_single_bright_texel);
    RUN_TEST(test_flat_input_round_trips);
    RUN_TEST(test_alpha_stays_linear);
    RUN_TEST(test_legacy_entry_point_is_raw);
    RUN_TEST(test_degenerate_dimensions);
    return UNITY_END();
}
