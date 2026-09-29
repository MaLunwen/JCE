/* test_jce_ui_canvas_scaler.c
 *
 * THE CANVAS SCALER'S MATCH CURVE, and the one value that must not move.
 *
 * jce_ui_canvas_scale_for is the single derivation the renderer uses, so this
 * asks the engine rather than recomputing the formula -- a test that derives
 * the answer a second way is asserting against its own arithmetic, which this
 * tree has paid for (a hinge axis disagreed with the clamp by 22 degrees for
 * exactly that reason).
 *
 * The load-bearing assertion is the BIT-IDENTITY one: every scene written
 * before match_width_or_height existed parses to 0.5, and at 0.5 the pow()
 * form is mathematically identical to sqrtf(rsx*rsy) but NOT bit-identical.
 * A one-ULP difference there moves a layout by a pixel in projects nobody
 * touched, which is the kind of regression that gets reported as "the UI
 * shifted" months later.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * The line that stood here said the opposite, and acting on it is why this
 * file sat untracked on a worktree eleven branches share.  Settle it with
 * `git check-ignore -v <path>`, never from memory.
 */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_ui_canvas.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* A 1920x1080 reference shown on a 2560x1080 ultrawide: the two axis ratios
 * differ (1.333 vs 1.0), which is the only case where match matters at all. */
#define REF_W 1920.0f
#define REF_H 1080.0f
#define SCR_W 2560.0f
#define SCR_H 1080.0f

static JceCanvasComponent canvas(float match)
{
    JceCanvasComponent c;
    memset(&c, 0, sizeof c);
    c.render_mode = JCE_CANVAS_OVERLAY;
    c.reference_resolution[0] = REF_W;
    c.reference_resolution[1] = REF_H;
    c.scale_factor = 1.0f;
    c.match_width_or_height = match;
    return c;
}

void setUp(void) {}
void tearDown(void) {}

static void test_half_is_bit_identical_to_the_historical_geometric_mean(void)
{
    const JceCanvasComponent c = canvas(0.5f);
    const float got = jce_ui_canvas_scale_for(&c, SCR_W, SCR_H);
    const float want = sqrtf((SCR_W / REF_W) * (SCR_H / REF_H));

    printf("  match 0.5 -> %.9g (historical sqrt %.9g)\n",
           (double)got, (double)want);
    /* EQUAL_MEMORY, not EQUAL_FLOAT: the point is the bits, because a ULP
     * here is a pixel in a project that changed nothing. */
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want, &got, sizeof want,
        "match 0.5 no longer reproduces sqrtf(rsx*rsy) bit for bit, so every "
        "scene written before this field existed just moved");

    /* ONE PAIR OF NUMBERS DOES NOT TEST THE SHORTCUT.  Measured: with the
     * exact-0.5 branch removed, this fixture alone still passed -- powf(x,.5)
     * * powf(y,.5) happened to land on the same bits.  Sweeping real
     * reference/screen combinations is what makes the assertion about the
     * branch rather than about one lucky ratio. */
    static const float refs[][2] = {
        {1920,1080},{1280,720},{1080,1920},{800,600},{2560,1440},{375,812},
    };
    static const float scrs[][2] = {
        {2560,1080},{3840,2160},{1366,768},{1920,1200},{1170,2532},{800,1280},
    };
    int swept = 0;
    for (size_t ri = 0; ri < sizeof refs / sizeof refs[0]; ri++) {
        for (size_t si = 0; si < sizeof scrs / sizeof scrs[0]; si++) {
            JceCanvasComponent k = canvas(0.5f);
            k.reference_resolution[0] = refs[ri][0];
            k.reference_resolution[1] = refs[ri][1];
            const float g = jce_ui_canvas_scale_for(&k, scrs[si][0], scrs[si][1]);
            const float w = sqrtf((scrs[si][0] / refs[ri][0]) *
                                  (scrs[si][1] / refs[ri][1]));
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&w, &g, sizeof w,
                "match 0.5 stopped being bit-identical to the historical "
                "geometric mean for some reference/screen pair");
            swept++;
        }
    }
    printf("  swept %d reference/screen pairs at match 0.5\n", swept);
}

static void test_zero_matches_width_and_one_matches_height(void)
{
    const JceCanvasComponent w = canvas(0.0f);
    const JceCanvasComponent h = canvas(1.0f);

    const float gw = jce_ui_canvas_scale_for(&w, SCR_W, SCR_H);
    const float gh = jce_ui_canvas_scale_for(&h, SCR_W, SCR_H);
    printf("  match 0 -> %.6f (sw/rw %.6f) | match 1 -> %.6f (sh/rh %.6f)\n",
           (double)gw, (double)(SCR_W / REF_W),
           (double)gh, (double)(SCR_H / REF_H));

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, SCR_W / REF_W, gw,
        "match 0 must scale by WIDTH alone");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, SCR_H / REF_H, gh,
        "match 1 must scale by HEIGHT alone");
    /* And they must differ, or this screen could not tell the ends apart and
     * the test above would pass on a scaler that ignores match entirely. */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(gw - gh) > 0.1f,
        "the fixture screen does not distinguish width from height matching, "
        "so nothing here is about match");
}

static void test_the_curve_is_monotonic_between_the_ends(void)
{
    /* Interpolation in LOG space, so intermediate values sit between the
     * ends and move one way.  A scaler that clamped to 0.5 for everything --
     * the exact-0.5 shortcut applied too widely -- passes both tests above
     * and fails this one. */
    float last = 0.0f;
    for (int i = 0; i <= 10; i++) {
        const JceCanvasComponent c = canvas((float)i / 10.0f);
        const float v = jce_ui_canvas_scale_for(&c, SCR_W, SCR_H);
        if (i > 0)
            TEST_ASSERT_TRUE_MESSAGE(v < last,
                "the match curve is not monotonic, so intermediate values are "
                "not interpolating between width and height");
        last = v;
    }
}

static void test_an_unscaled_canvas_is_exactly_one(void)
{
    /* The negative half: world space and an unauthored reference resolution
     * both mean "not scaled", and the renderer leaves ui_scale at 1 for them.
     * Returning anything else here would scale a canvas the renderer does
     * not. */
    JceCanvasComponent world = canvas(0.0f);
    world.render_mode = JCE_CANVAS_WORLD;
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_ui_canvas_scale_for(&world, SCR_W, SCR_H));

    JceCanvasComponent unauthored = canvas(0.0f);
    unauthored.reference_resolution[0] = 0.0f;
    unauthored.reference_resolution[1] = 0.0f;
    TEST_ASSERT_EQUAL_FLOAT(1.0f,
        jce_ui_canvas_scale_for(&unauthored, SCR_W, SCR_H));

    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_ui_canvas_scale_for(NULL, SCR_W, SCR_H));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_half_is_bit_identical_to_the_historical_geometric_mean);
    RUN_TEST(test_zero_matches_width_and_one_matches_height);
    RUN_TEST(test_the_curve_is_monotonic_between_the_ends);
    RUN_TEST(test_an_unscaled_canvas_is_exactly_one);
    return UNITY_END();
}
