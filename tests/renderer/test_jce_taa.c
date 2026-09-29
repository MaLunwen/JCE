/* test_jce_taa.c
 *
 * Pure-math unit tests for the Temporal Anti-Aliasing CPU helper
 * (engine/src/renderer/jce_taa.c).  No bgfx is touched: every entry
 * point exercised here only walks jce_math and the JceTaaState struct.
 *
 * These tests verify the TAA sampling and projection contracts:
 *   - Halton-(2,3) sequence values (1-indexed van der Corput).
 *   - jce_taa_advance() jitter scale  = (halton(idx)-0.5) * 2/dim,
 *     with the Halton index = (frame_index & 7) + 1  (period-8 sequence),
 *     and frame_index a free-running counter.
 *   - jce_taa_apply_jitter() produces a depth-independent NDC offset
 *     for perspective and orthographic projections; zero jitter is a no-op.
 *   - jce_taa_record_camera() stores the matrices verbatim + flags valid.
 *   - determinism across two fresh states.
 *   - first-frame: zero-init state has prev_valid == false.
 */

#include <jce/renderer/jce_taa.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-5f

/* ── 1. Halton known values ──────────────────────────────────────────── */

static void test_halton_base2_known_values(void)
{
    /* van der Corput base-2: 1/2, 1/4, 3/4, 1/8 ... */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.5f,   jce_taa_halton(1u, 2u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.25f,  jce_taa_halton(2u, 2u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.75f,  jce_taa_halton(3u, 2u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.125f, jce_taa_halton(4u, 2u));
}

static void test_halton_base3_known_values(void)
{
    /* van der Corput base-3: 1/3, 2/3, 1/9 ... */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f / 3.0f, jce_taa_halton(1u, 3u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f / 3.0f, jce_taa_halton(2u, 3u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f / 9.0f, jce_taa_halton(3u, 3u));
}

static void test_halton_zero_and_degenerate_bases(void)
{
    /* i == 0 → 0 for any base (the while loop never runs). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_taa_halton(0u, 2u));
    /* base < 2 is rejected → 0. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_taa_halton(5u, 1u));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, jce_taa_halton(5u, 0u));
}

/* ── 2. advance: jitter bounded + scale + previous rotation + index ──── */

static void test_advance_jitter_scale_and_bounds(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));

    const uint32_t w = 1920u, h = 1080u;
    jce_taa_advance(&s, w, h);

    /* First advance: frame_index started at 0 → masked idx = (0 & 7)+1 = 1. */
    float hx = jce_taa_halton(1u, 2u) - 0.5f;
    float hy = jce_taa_halton(1u, 3u) - 0.5f;
    float expect_x = hx * 2.0f / (float)w;
    float expect_y = hy * 2.0f / (float)h;
    TEST_ASSERT_FLOAT_WITHIN(EPS, expect_x, s.current_jitter[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, expect_y, s.current_jitter[1]);

    /* |halton-0.5| < 0.5, so |jitter| < 1/dim (== 2*0.5/dim) in NDC. */
    TEST_ASSERT_TRUE(fabsf(s.current_jitter[0]) < (1.0f / (float)w) + EPS);
    TEST_ASSERT_TRUE(fabsf(s.current_jitter[1]) < (1.0f / (float)h) + EPS);

    /* frame_index incremented once. */
    TEST_ASSERT_EQUAL_UINT32(1u, s.frame_index);
}

static void test_advance_rotates_previous(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));

    jce_taa_advance(&s, 1920u, 1080u);
    float first_x = s.current_jitter[0];
    float first_y = s.current_jitter[1];

    jce_taa_advance(&s, 1920u, 1080u);
    /* previous_jitter on the 2nd advance == the 1st advance's current. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, first_x, s.previous_jitter[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, first_y, s.previous_jitter[1]);
    TEST_ASSERT_EQUAL_UINT32(2u, s.frame_index);
}

static void test_advance_sequence_period_is_eight(void)
{
    /* The masked Halton index (frame_index & 7)+1 makes the jitter sequence
       repeat every 8 advances.  Capture 8 then assert the next 8 match. */
    JceTaaState s;
    memset(&s, 0, sizeof(s));

    float seq[8][2];
    for (int i = 0; i < 8; i++) {
        jce_taa_advance(&s, 1920u, 1080u);
        seq[i][0] = s.current_jitter[0];
        seq[i][1] = s.current_jitter[1];
    }
    for (int i = 0; i < 8; i++) {
        jce_taa_advance(&s, 1920u, 1080u);
        TEST_ASSERT_FLOAT_WITHIN(EPS, seq[i][0], s.current_jitter[0]);
        TEST_ASSERT_FLOAT_WITHIN(EPS, seq[i][1], s.current_jitter[1]);
    }
    /* frame_index keeps counting (free-running, not wrapped at 8). */
    TEST_ASSERT_EQUAL_UINT32(16u, s.frame_index);
}

static void test_advance_zero_dim_is_safe(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));
    jce_taa_advance(&s, 0u, 0u);
    /* Division guarded → exactly zero jitter, no NaN/inf. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s.current_jitter[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, s.current_jitter[1]);
}

static void test_advance_null_is_safe(void)
{
    jce_taa_advance(NULL, 1920u, 1080u);  /* must not crash */
    TEST_PASS();
}

/* ── 3. apply_jitter: touches only m[8]/m[9] ─────────────────────────── */

static jce_mat4 make_known_proj(void)
{
    /* A plausible perspective matrix; exact values don't matter — we only
       check which entries the patcher modifies. */
    jce_mat4 m = jce_m4_identity();
    m.raw[0][0] = 1.357f;   /* m[0]  */
    m.raw[1][1] = 2.414f;   /* m[5]  */
    m.raw[2][2] = -1.002f;  /* m[10] */
    m.raw[2][3] = -1.0f;    /* m[11] */
    m.raw[3][2] = -0.2f;    /* m[14] */
    m.raw[3][3] = 0.0f;     /* m[15] */
    return m;
}

static void test_apply_jitter_offsets_only_two_entries(void)
{
    jce_mat4 base = make_known_proj();
    jce_mat4 patched = base;

    float jitter[2] = { 0.0123f, -0.0456f };
    jce_taa_apply_jitter(&patched, jitter);

    const float *b = JCE_M4_PTR(base);
    const float *p = JCE_M4_PTR(patched);

    for (int i = 0; i < 16; i++) {
        if (i == 8) {
            TEST_ASSERT_FLOAT_WITHIN(EPS, b[8] + jitter[0], p[8]);
        } else if (i == 9) {
            TEST_ASSERT_FLOAT_WITHIN(EPS, b[9] + jitter[1], p[9]);
        } else {
            /* All other 14 entries are bit-untouched. */
            TEST_ASSERT_EQUAL_FLOAT(b[i], p[i]);
        }
    }
}

/* Test projected positions, not particular matrix slots: the NDC displacement
 * must be independent of depth for both board-game and perspective cameras. */
static void test_jitter_is_depth_independent(void)
{
    const float jitter[2] = {0.0004f, -0.0007f};
    const float depths[] = {-1.0f, -20.0f, -150.0f};
    for (int kind = 0; kind < 2; ++kind) {
        jce_mat4 base = kind ? make_known_proj() : jce_m4_identity();
        jce_mat4 patched = base;
        jce_taa_apply_jitter(&patched, jitter);
        for (int d = 0; d < 3; ++d) {
            const float point[4] = {0.2f, -0.3f, depths[d], 1.0f};
            float before[4] = {0}, after[4] = {0};
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col) {
                    before[row] += base.raw[col][row] * point[col];
                    after[row] += patched.raw[col][row] * point[col];
                }
            for (int axis = 0; axis < 2; ++axis)
                TEST_ASSERT_FLOAT_WITHIN(EPS, -jitter[axis],
                    after[axis] / after[3] - before[axis] / before[3]);
            TEST_ASSERT_EQUAL_FLOAT(before[2], after[2]);
            TEST_ASSERT_EQUAL_FLOAT(before[3], after[3]);
        }
    }
}

static void test_apply_zero_jitter_is_identity(void)
{
    jce_mat4 base = make_known_proj();
    jce_mat4 patched = base;
    float jitter[2] = { 0.0f, 0.0f };
    jce_taa_apply_jitter(&patched, jitter);
    /* Byte-identical: zero offset leaves the proj unchanged. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&base, &patched, sizeof(jce_mat4)));
}

static void test_apply_jitter_null_is_safe(void)
{
    jce_mat4 base = make_known_proj();
    float jitter[2] = { 0.5f, 0.5f };
    jce_taa_apply_jitter(NULL, jitter);        /* must not crash */
    jce_taa_apply_jitter(&base, NULL);         /* must not crash, no change */
    jce_mat4 ref = make_known_proj();
    TEST_ASSERT_EQUAL_INT(0, memcmp(&ref, &base, sizeof(jce_mat4)));
}

/* ── 4. record_camera: stores matrices + sets valid ──────────────────── */

static void test_record_camera_stores_and_validates(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));
    TEST_ASSERT_FALSE(s.prev_valid);

    jce_mat4 view = make_known_proj();   /* any two distinct matrices */
    jce_mat4 proj = jce_m4_identity();
    proj.raw[0][0] = 3.14159f;

    jce_taa_record_camera(&s, &view, &proj);

    TEST_ASSERT_TRUE(s.prev_valid);
    TEST_ASSERT_EQUAL_INT(0, memcmp(&s.prev_view, &view, sizeof(jce_mat4)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(&s.prev_proj, &proj, sizeof(jce_mat4)));
}

static void test_record_camera_null_is_safe(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));
    jce_mat4 m = jce_m4_identity();
    jce_taa_record_camera(NULL, &m, &m);  /* must not crash */
    jce_taa_record_camera(&s, NULL, &m);  /* must not crash, no state change */
    jce_taa_record_camera(&s, &m, NULL);
    TEST_ASSERT_FALSE(s.prev_valid);
}

/* ── 5. determinism across two fresh states ──────────────────────────── */

static void test_two_states_advance_identically(void)
{
    JceTaaState a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    for (int i = 0; i < 20; i++) {
        jce_taa_advance(&a, 1600u, 900u);
        jce_taa_advance(&b, 1600u, 900u);
    }
    /* Same inputs N times → bit-identical jitter + counters. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(a.current_jitter,  b.current_jitter,  sizeof(a.current_jitter)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(a.previous_jitter, b.previous_jitter, sizeof(a.previous_jitter)));
    TEST_ASSERT_EQUAL_UINT32(a.frame_index, b.frame_index);
}

/* ── 6. first-frame: zero-init state is "history invalid" ─────────────── */

static void test_first_frame_prev_invalid(void)
{
    JceTaaState s;
    memset(&s, 0, sizeof(s));
    /* Zero-init valid: renderer must treat history as invalid on frame 1. */
    TEST_ASSERT_FALSE(s.prev_valid);
    /* advance() must NOT validate prev (only record_camera does). */
    jce_taa_advance(&s, 1920u, 1080u);
    TEST_ASSERT_FALSE(s.prev_valid);
}

int main(void)
{
    UNITY_BEGIN();
    /* 1. halton */
    RUN_TEST(test_halton_base2_known_values);
    RUN_TEST(test_halton_base3_known_values);
    RUN_TEST(test_halton_zero_and_degenerate_bases);
    /* 2. advance */
    RUN_TEST(test_advance_jitter_scale_and_bounds);
    RUN_TEST(test_advance_rotates_previous);
    RUN_TEST(test_advance_sequence_period_is_eight);
    RUN_TEST(test_advance_zero_dim_is_safe);
    RUN_TEST(test_advance_null_is_safe);
    /* 3. apply_jitter */
    RUN_TEST(test_apply_jitter_offsets_only_two_entries);
    RUN_TEST(test_jitter_is_depth_independent);
    RUN_TEST(test_apply_zero_jitter_is_identity);
    RUN_TEST(test_apply_jitter_null_is_safe);
    /* 4. record_camera */
    RUN_TEST(test_record_camera_stores_and_validates);
    RUN_TEST(test_record_camera_null_is_safe);
    /* 5. determinism */
    RUN_TEST(test_two_states_advance_identically);
    /* 6. first-frame */
    RUN_TEST(test_first_frame_prev_invalid);
    return UNITY_END();
}
