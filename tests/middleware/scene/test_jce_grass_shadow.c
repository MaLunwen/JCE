/*
 * test_jce_grass_shadow.c — GrassField.cast_shadow decides whether a field's
 * blades reach the depth pass, and which of them.
 *
 * The field was authored, serialized, and drawn in the Inspector inside
 * BeginDisabled(true) with an unwired badge and a "(v1: reserved)" label.  So
 * the editor was honest -- and grass never darkened anything, which reads as
 * flat paint rather than as blades.
 *
 * The prior hypothesis was that the instanced shadow program did not exist.
 * It does: vs_shadow_inst.sc is built as `shadow_inst` and the sibling
 * vegetation scatter has been driving it from a persistent instance buffer for
 * some time.  Grass carries the identical machinery and simply never joined.
 *
 * COUNTS AND RANGES, no GPU and no pixels.  The run set IS the submit
 * decision, so "this field contributes N blades to this cascade" is exactly
 * the question the gate is really asking, and it is arithmetic.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <string.h>

/* Internal to the scene renderer; non-static symbols in the engine library. */
typedef struct { uint32_t start, count; } GrassRun;
bool     jce_grass_shadow_field_casts(const JceGrassFieldComponent *g,
                                      bool comp_enabled, bool visible,
                                      uint32_t resident_blades);
uint32_t jce_grass_shadow_runs(const uint32_t *cell_start,
                               uint16_t gx, uint16_t gz,
                               float min_x, float min_z, float cell_size,
                               float ymin, float ymax,
                               const jce_vec4 *planes,
                               jce_vec3 cam, float fade_end,
                               GrassRun *out, uint32_t max_runs);

/* ── (A) does the field cast at all ──────────────────────────────────── */

static bool casts(bool cast_shadow, bool enabled, bool visible, uint32_t res)
{
    JceGrassFieldComponent g;
    memset(&g, 0, sizeof g);
    g.cast_shadow = cast_shadow;
    return jce_grass_shadow_field_casts(&g, enabled, visible, res);
}

static void test_every_gate_must_agree_before_a_blade_is_submitted(void)
{
    TEST_ASSERT_FALSE_MESSAGE(casts(false, true, true, 4096),
        "cast_shadow == false is the zero value AND today's behaviour: every "
        "existing scene must render a byte-identical shadow map");
    TEST_ASSERT_FALSE_MESSAGE(casts(true, true, false, 4096),
        "an invisible field casts nothing");
    TEST_ASSERT_FALSE_MESSAGE(casts(true, false, true, 4096),
        "a disabled component casts nothing");
    TEST_ASSERT_FALSE_MESSAGE(casts(true, true, true, 0),
        "no persistent instance buffer -- LOW/MED tiers and "
        "JCE_PERSIST_GRASS=0 -- must degrade to no grass shadows, not to a "
        "submit of a buffer that was never built");
    TEST_ASSERT_TRUE_MESSAGE(casts(true, true, true, 4096),
        "and with all four true it casts: this is the feature");
}

/* ── (B) which cells, and how they merge ─────────────────────────────── */

/* 4x4 cells of 8 units, 16 blades each, origin at (0,0). */
enum { GX = 4, GZ = 4, PER = 16, NCELLS = GX * GZ };
static uint32_t g_cs[NCELLS + 1];

static void build_grid(void)
{
    for (int i = 0; i <= NCELLS; ++i) g_cs[i] = (uint32_t)(i * PER);
}

/* Planes of an axis-aligned box, in the layout jce_aabb_in_frustum wants
 * (xyz = inward normal, w = offset; a point is inside when n.p + w >= 0). */
static void box_planes(float x0, float x1, float z0, float z1,
                       jce_vec4 planes[6])
{
    planes[0] = (jce_vec4){  1, 0, 0, -x0 };
    planes[1] = (jce_vec4){ -1, 0, 0,  x1 };
    planes[2] = (jce_vec4){ 0,  1, 0, 1000.0f };
    planes[3] = (jce_vec4){ 0, -1, 0, 1000.0f };
    planes[4] = (jce_vec4){ 0, 0,  1, -z0 };
    planes[5] = (jce_vec4){ 0, 0, -1,  z1 };
}

static uint32_t total(const GrassRun *r, uint32_t n)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += r[i].count;
    return s;
}

static void test_a_cascade_containing_everything_is_one_run(void)
{
    build_grid();
    jce_vec4 p[6];
    box_planes(-100, 100, -100, 100, p);
    GrassRun runs[64];
    const uint32_t n = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                                             0.0f, 1.0f, p,
                                             jce_v3(0, 0, 0), 0.0f,
                                             runs, 64);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n,
        "cells are packed contiguous, so the whole field must merge into ONE "
        "submit rather than sixteen");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, runs[0].start, "run starts at blade 0");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(NCELLS * PER, runs[0].count,
        "and covers every blade");
}

static void test_a_clipped_cascade_takes_half(void)
{
    build_grid();
    jce_vec4 p[6];
    GrassRun runs[64];

    /* THE MARGIN IS NOT INCIDENTAL and this pins it.  Cells are grown by
     * cell*0.25 + 0.5 = 2.5 before the frustum test, the same slack the colour
     * pass uses, because a blade sways and has width -- its cell's box is not
     * the box its geometry occupies.  So column 2 ([16,24] -> [13.5,26.5])
     * survives a clip at x < 15, and the honest count there is THREE columns,
     * not two.  Getting this wrong in the test first is what named it. */
    box_planes(-100, 15.0f, -100, 100, p);
    uint32_t n = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                                       0.0f, 1.0f, p,
                                       jce_v3(0, 0, 0), 0.0f, runs, 64);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3 * GZ * PER, total(runs, n),
        "the sway margin must widen each cell by 2.5, so a clip at x < 15 "
        "still reaches the column that starts at 16");

    /* Clip below 16 - 2.5 and the third column really is gone. */
    box_planes(-100, 13.0f, -100, 100, p);
    n = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                              0.0f, 1.0f, p, jce_v3(0, 0, 0), 0.0f, runs, 64);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "a clipped cascade still takes something");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(NCELLS * PER / 2, total(runs, n),
        "exactly the blades of the two surviving columns");
    /* Runs must be contiguous, non-overlapping and ascending, or the submit
     * would bind overlapping instance slices. */
    for (uint32_t i = 1; i < n; ++i)
        TEST_ASSERT_TRUE_MESSAGE(runs[i].start >= runs[i - 1].start +
                                                  runs[i - 1].count,
            "runs must not overlap");
}

static void test_the_fade_distance_is_the_same_one_the_colour_pass_uses(void)
{
    build_grid();
    jce_vec4 p[6];
    box_planes(-100, 100, -100, 100, p);
    GrassRun far_runs[64], near_runs[64];
    const uint32_t nf = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                                              0.0f, 1.0f, p,
                                              jce_v3(0, 0, 0), 0.0f,
                                              far_runs, 64);
    const uint32_t nn = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                                              0.0f, 1.0f, p,
                                              jce_v3(0, 0, 0), 4.0f,
                                              near_runs, 64);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(NCELLS * PER, total(far_runs, nf),
        "fade_end == 0 means no fade, so everything casts");
    TEST_ASSERT_TRUE_MESSAGE(total(near_runs, nn) < total(far_runs, nf),
        "a short fade must drop the far cells -- otherwise a blade casts a "
        "shadow it is too far away to be drawn with, and the shadow map and "
        "the colour pass disagree about the same field");
    TEST_ASSERT_TRUE_MESSAGE(total(near_runs, nn) > 0,
        "and it must not drop the cell the camera is standing in");
}

static void test_a_run_cap_truncates_rather_than_overruns(void)
{
    build_grid();
    jce_vec4 p[6];
    /* Alternating columns kept would need several runs; ask for one. */
    box_planes(-100, 100, -100, 100, p);
    GrassRun one[1];
    const uint32_t n = jce_grass_shadow_runs(g_cs, GX, GZ, 0.0f, 0.0f, 8.0f,
                                             0.0f, 1.0f, p,
                                             jce_v3(0, 0, 0), 0.0f, one, 1);
    TEST_ASSERT_TRUE_MESSAGE(n <= 1, "must never write past max_runs");
}

static void test_degenerate_input_is_refused(void)
{
    jce_vec4 p[6];
    box_planes(-100, 100, -100, 100, p);
    GrassRun runs[8];
    TEST_ASSERT_EQUAL_UINT32(0, jce_grass_shadow_runs(NULL, GX, GZ, 0, 0, 8,
        0, 1, p, jce_v3(0, 0, 0), 0.0f, runs, 8));
    build_grid();
    TEST_ASSERT_EQUAL_UINT32(0, jce_grass_shadow_runs(g_cs, 0, GZ, 0, 0, 8,
        0, 1, p, jce_v3(0, 0, 0), 0.0f, runs, 8));
    TEST_ASSERT_EQUAL_UINT32(0, jce_grass_shadow_runs(g_cs, GX, GZ, 0, 0, 0.0f,
        0, 1, p, jce_v3(0, 0, 0), 0.0f, runs, 8));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_gate_must_agree_before_a_blade_is_submitted);
    RUN_TEST(test_a_cascade_containing_everything_is_one_run);
    RUN_TEST(test_a_clipped_cascade_takes_half);
    RUN_TEST(test_the_fade_distance_is_the_same_one_the_colour_pass_uses);
    RUN_TEST(test_a_run_cap_truncates_rather_than_overruns);
    RUN_TEST(test_degenerate_input_is_refused);
    return UNITY_END();
}
