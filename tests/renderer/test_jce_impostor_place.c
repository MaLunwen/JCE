/*
 * test_jce_impostor_place.c
 *
 * Where an impostor card sits in the world, and how big it is.
 *
 * The bug this locks down was silent by construction: the renderer computed
 * the anchor as `translation + center * length(column 0)`, which agrees with
 * the correct answer for every instance that is unrotated and uniformly
 * scaled -- which is every instance anyone writes by hand while testing.  It
 * disagrees for scattered vegetation, where random yaw and per-instance scale
 * are the whole point, and there the failure has no error and no crash: the
 * card simply draws somewhere the model is not, at a size the model is not.
 *
 * So the tests below are deliberately built around transforms that DISTINGUISH
 * the two formulas.  An identity-transform test would have passed against the
 * broken code and proved nothing.
 */

#include <jce/renderer/jce_impostor.h>

#include <math.h>
#include <string.h>

#include "unity.h"

/* MSVC's <math.h> only defines M_PI under _USE_MATH_DEFINES, and this suite
 * must build on every toolchain the engine targets.  Spelled out rather than
 * defining the MSVC macro so the value is the same everywhere. */
#define TEST_PI 3.14159265358979323846f

void setUp(void)    {}
void tearDown(void) {}

/* Column-major, matching the engine: elements 12..14 are the translation. */
static void m_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Rotation about Y by `rad`, then a per-axis scale, then a translation. */
static void m_trs_yaw(float m[16], float rad,
                      float sx, float sy, float sz,
                      float tx, float ty, float tz)
{
    const float c = cosf(rad), s = sinf(rad);
    memset(m, 0, 16 * sizeof(float));
    /* column 0 = R*(sx,0,0), column 1 = R*(0,sy,0), column 2 = R*(0,0,sz) */
    m[0]  =  c * sx;  m[1]  = 0.0f;  m[2]  = -s * sx;
    m[4]  = 0.0f;     m[5]  = sy;    m[6]  = 0.0f;
    m[8]  =  s * sz;  m[9]  = 0.0f;  m[10] =  c * sz;
    m[12] = tx; m[13] = ty; m[14] = tz;
    m[15] = 1.0f;
}

static JceImpostorMeta meta_with(float cx, float cy, float cz, float r)
{
    JceImpostorMeta m;
    memset(&m, 0, sizeof m);
    m.grid_n = 8; m.cell_px = 128;
    m.center[0] = cx; m.center[1] = cy; m.center[2] = cz;
    m.radius = r;
    return m;
}

/* ── 1. Identity: the anchor is the local center, the radius unscaled ── */

static void test_identity_is_passthrough(void)
{
    float m[16]; m_identity(m);
    const JceImpostorMeta meta = meta_with(0.0f, 2.5f, 0.0f, 3.0f);
    float c[3], r = 0.0f;
    jce_impostor_card_place(m, &meta, c, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, c[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.5f, c[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, c[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, r);
}

/* ── 2. THE POINT: rotation moves the anchor ───────────────────────────
 *
 * A center offset along local +X, rotated 90 degrees about Y, must land on
 * world -Z.  The old formula ignored rotation entirely and would leave it on
 * +X -- so this is the assertion that separates correct from plausible. */

static void test_rotation_transforms_the_anchor(void)
{
    float m[16];
    m_trs_yaw(m, TEST_PI * 0.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    const JceImpostorMeta meta = meta_with(4.0f, 0.0f, 0.0f, 1.0f);
    float c[3], r = 0.0f;
    jce_impostor_card_place(m, &meta, c, &r);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, c[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, c[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -4.0f, c[2]);

    /* And it is NOT where the superseded formula would have put it. */
    TEST_ASSERT_FALSE(fabsf(c[0] - 4.0f) < 1e-3f);
}

/* ── 3. Translation composes after rotation, not before ──────────────── */

static void test_translation_applies_after_rotation(void)
{
    float m[16];
    m_trs_yaw(m, TEST_PI * 0.5f, 1.0f, 1.0f, 1.0f, 10.0f, 1.0f, -5.0f);
    const JceImpostorMeta meta = meta_with(4.0f, 0.0f, 0.0f, 1.0f);
    float c[3], r = 0.0f;
    jce_impostor_card_place(m, &meta, c, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, c[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  1.0f, c[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -9.0f, c[2]);   /* -5 + (-4) */
}

/* ── 4. Non-uniform scale: the radius takes the LARGEST axis ──────────
 *
 * A tree scaled 1 wide and 3 tall must get a card sized off the 3, or the
 * billboard crops the top of the model it is standing in for.  The old
 * formula read column 0 only and would have returned the 1. */

static void test_radius_uses_the_largest_axis(void)
{
    float m[16];
    m_trs_yaw(m, 0.0f, 1.0f, 3.0f, 1.0f, 0.0f, 0.0f, 0.0f);
    const JceImpostorMeta meta = meta_with(0.0f, 0.0f, 0.0f, 2.0f);
    float c[3], r = 0.0f;
    jce_impostor_card_place(m, &meta, c, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6.0f, r);          /* 2 * 3, not 2 * 1 */

    /* Largest, whichever axis carries it -- Z here. */
    m_trs_yaw(m, 0.0f, 1.0f, 1.0f, 5.0f, 0.0f, 0.0f, 0.0f);
    jce_impostor_card_place(m, &meta, c, &r);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, r);
}

/* ── 5. Rotation alone must not change the radius ─────────────────────
 *
 * Column lengths are rotation-invariant, so a spinning instance must keep a
 * constant card size.  If it did not, a scattered forest would pulse as the
 * camera moved -- the exact artifact this whole change is about. */

static void test_radius_is_rotation_invariant(void)
{
    const JceImpostorMeta meta = meta_with(0.0f, 1.0f, 0.0f, 2.5f);
    float first = -1.0f;
    for (int i = 0; i < 16; ++i) {
        float m[16];
        m_trs_yaw(m, (float)i * 0.4f, 1.3f, 1.3f, 1.3f, 3.0f, 0.0f, 7.0f);
        float c[3], r = 0.0f;
        jce_impostor_card_place(m, &meta, c, &r);
        if (first < 0.0f) first = r;
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, first, r);
    }
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.5f * 1.3f, first);
}

/* ── 6. NULL arguments are survivable ─────────────────────────────────  */

static void test_null_arguments_do_not_crash(void)
{
    float m[16]; m_identity(m);
    const JceImpostorMeta meta = meta_with(0.0f, 1.0f, 0.0f, 1.0f);
    float c[3] = { -1.0f, -1.0f, -1.0f }, r = -1.0f;

    jce_impostor_card_place(NULL, &meta, c, &r);
    jce_impostor_card_place(m, NULL, c, &r);
    jce_impostor_card_place(m, &meta, NULL, &r);
    jce_impostor_card_place(m, &meta, c, NULL);

    /* Every call above was rejected, so the outputs are untouched. */
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, c[0]);
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, r);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_is_passthrough);
    RUN_TEST(test_rotation_transforms_the_anchor);
    RUN_TEST(test_translation_applies_after_rotation);
    RUN_TEST(test_radius_uses_the_largest_axis);
    RUN_TEST(test_radius_is_rotation_invariant);
    RUN_TEST(test_null_arguments_do_not_crash);
    return UNITY_END();
}
