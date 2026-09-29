/*
 * test_jce_water_ripple.c -- the disturbance layer.
 *
 * A wave solver is unusually easy to get PLAUSIBLE and unusually hard to get
 * right: a wrong sign, a Laplacian that is not self-adjoint, a CFL violation
 * and an unstable boundary all produce something that ripples. So the tests
 * here are built around the one property that separates the two -- ENERGY --
 * plus the handful of behaviours that are the reason to have a bed depth at
 * all.
 *
 * Everything is headless and deterministic: no clock, no RNG, no GPU.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_water_ripple.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

static JceWaterRipple *make(float damping, float depth, int res, float size)
{
    JceWaterRippleDesc d = jce_water_ripple_default_desc();
    d.resolution      = res;
    d.size_m          = size;
    d.default_depth_m = depth;
    d.damping         = damping;
    JceWaterRipple *r = jce_water_ripple_create(&d);
    TEST_ASSERT_NOT_NULL(r);
    return r;
}

/* ── 1. A rejected desc is ABSENT, not a flat pond ─────────────────────── */

static void test_create_rejects_nonsense(void)
{
    JceWaterRippleDesc d = jce_water_ripple_default_desc();

    /* The default must itself be valid, or every "change only what you care
     * about" caller starts from something broken. */
    JceWaterRipple *ok = jce_water_ripple_create(&d);
    TEST_ASSERT_NOT_NULL(ok);
    jce_water_ripple_destroy(ok);

    d = jce_water_ripple_default_desc(); d.resolution = 3;
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    d = jce_water_ripple_default_desc(); d.resolution = 4096;
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    d = jce_water_ripple_default_desc(); d.size_m = 0.0f;
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    d = jce_water_ripple_default_desc(); d.default_depth_m = 0.0f;
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    d = jce_water_ripple_default_desc(); d.default_depth_m = -1.0f;
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    d = jce_water_ripple_default_desc(); d.size_m = nanf("");
    TEST_ASSERT_NULL(jce_water_ripple_create(&d));
    TEST_ASSERT_NULL(jce_water_ripple_create(NULL));

    /* Every accessor answers for NULL rather than crashing: a caller polling
     * the layer before it exists must get a defensible number. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(NULL, 0.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_energy(NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_max_dt(NULL));
    TEST_ASSERT_EQUAL_INT(0, jce_water_ripple_resolution(NULL));
    TEST_ASSERT_NULL(jce_water_ripple_height_data(NULL));
    jce_water_ripple_step(NULL, 0.016f);          /* must not crash */
    jce_water_ripple_impulse(NULL, 0, 0, 1, 1);
    jce_water_ripple_destroy(NULL);
}

/* ── 2. Still water stays still ────────────────────────────────────────── */

static void test_undisturbed_stays_flat(void)
{
    JceWaterRipple *r = make(0.4f, 2.0f, 32, 16.0f);
    for (int i = 0; i < 200; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    const float *h = jce_water_ripple_height_data(r);
    const int cells = 32 * 32;
    for (int i = 0; i < cells; ++i)
        TEST_ASSERT_EQUAL_FLOAT(0.0f, h[i]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_energy(r));
    jce_water_ripple_destroy(r);
}

/* ── 3. THE test: energy must not grow ─────────────────────────────────────
 *
 * This is the one assertion that separates a correct scheme from a plausible
 * one. Every classic failure of an explicit wave solver -- a sign error, a
 * Laplacian that is not symmetric, a CFL violation, an unstable edge -- shows
 * up as energy GROWING, and it shows up here long before anything looks wrong
 * on a screen. */

static void test_energy_never_grows_undamped(void)
{
    JceWaterRipple *r = make(0.0f, 2.0f, 48, 24.0f);   /* damping OFF */
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 2.0f, 1.0f);

    const float e0 = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE_MESSAGE(e0 > 0.0f, "impulse deposited no energy");

    float worst = e0;
    for (int i = 0; i < 4000; ++i) {
        jce_water_ripple_step(r, 1.0f / 60.0f);
        const float e = jce_water_ripple_energy(r);
        TEST_ASSERT_TRUE_MESSAGE(e == e, "energy went NaN - the grid diverged");
        if (e > worst) worst = e;
    }
    /* Symplectic Euler conserves a SHADOW energy, not this one, so E
     * oscillates about it with an amplitude set by dt * omega_max. Measured on
     * a harsher grid than this one that residual is 5.70%; 20% leaves room for
     * it while staying far below any instability, which is exponential and
     * passes 3x within a few hundred steps (the asymmetric-Laplacian mutant
     * reaches 3.19). The bound is loose on purpose: a tight one here would be
     * pinning a property of dt, not of the scheme. */
    TEST_ASSERT_TRUE_MESSAGE(worst <= e0 * 1.20f,
        "undamped energy grew - the scheme is pumping, not conserving");
    jce_water_ripple_destroy(r);
}

static void test_energy_decays_when_damped(void)
{
    JceWaterRipple *r = make(0.8f, 2.0f, 48, 24.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 2.0f, 1.0f);
    const float e0 = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE(e0 > 0.0f);

    for (int i = 0; i < 600; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);
    const float e1 = jce_water_ripple_energy(r);

    TEST_ASSERT_TRUE_MESSAGE(e1 < e0 * 0.25f,
        "damped pond did not settle - damping is not reaching the solver");
    TEST_ASSERT_TRUE(e1 >= 0.0f);
    jce_water_ripple_destroy(r);
}

/* ── 4. The impulse is a VELOCITY, not a displacement ──────────────────── */

static void test_impulse_drives_velocity_not_height(void)
{
    JceWaterRipple *r = make(0.0f, 2.0f, 32, 16.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 2.0f, 1.0f);

    /* Immediately after the impulse and BEFORE any step, the surface has not
     * moved: only its velocity has. An implementation that displaced the
     * height would already show a dent here, and it would look fine on
     * screen -- which is exactly why this is asserted rather than eyeballed. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, 0.0f, 0.0f));
    TEST_ASSERT_TRUE_MESSAGE(jce_water_ripple_energy(r) > 0.0f,
        "a velocity-only impulse must still carry energy");

    /* One step later the surface HAS moved, and downward for a positive
     * (downward) impulse speed. */
    jce_water_ripple_step(r, 1.0f / 240.0f);
    TEST_ASSERT_TRUE_MESSAGE(jce_water_ripple_height(r, 0.0f, 0.0f) < 0.0f,
        "a downward impulse did not push the surface down");
    jce_water_ripple_destroy(r);
}

/* ── 5. The disturbance actually propagates, at about sqrt(gH) ──────────── */

static void test_wave_travels_at_shallow_water_speed(void)
{
    /* 4 m deep => c = sqrt(9.80665*4) = 6.264 m/s. Over 0.5 s a front should
     * reach about 3.1 m. The grid is 40 m so nothing can reflect back in that
     * time and confuse the reading. */
    JceWaterRipple *r = make(0.0f, 4.0f, 128, 40.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 1.0f, 1.0f);

    /* Before: a point 3 m out is untouched. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, 3.0f, 0.0f));
    /* And so is one far beyond where the front can reach in 0.5 s. */
    for (int i = 0; i < 30; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    const float near_pt = fabsf(jce_water_ripple_height(r, 3.0f,  0.0f));
    const float far_pt  = fabsf(jce_water_ripple_height(r, 12.0f, 0.0f));

    TEST_ASSERT_TRUE_MESSAGE(near_pt > 1e-6f,
        "nothing arrived 3 m away after 0.5 s - the wave is not propagating");
    /* The scheme is dispersive, so a little energy always runs ahead of the
     * ideal front; what must hold is that the bulk has NOT arrived at twice
     * the distance the speed allows. */
    TEST_ASSERT_TRUE_MESSAGE(far_pt < near_pt * 0.1f,
        "the disturbance travelled far faster than sqrt(gH) allows");
    jce_water_ripple_destroy(r);
}

/* ── 6. Depth is the whole reason this is shallow water ────────────────── */

static void test_deeper_water_carries_waves_faster(void)
{
    /* Same impulse, same grid, four times the depth => twice the speed. */
    JceWaterRipple *shallow = make(0.0f, 1.0f, 128, 40.0f);
    JceWaterRipple *deep    = make(0.0f, 4.0f, 128, 40.0f);
    jce_water_ripple_impulse(shallow, 0.0f, 0.0f, 1.0f, 1.0f);
    jce_water_ripple_impulse(deep,    0.0f, 0.0f, 1.0f, 1.0f);

    for (int i = 0; i < 30; ++i) {
        jce_water_ripple_step(shallow, 1.0f / 60.0f);
        jce_water_ripple_step(deep,    1.0f / 60.0f);
    }
    /* At 5 m, the deep front (c=6.26, reach 3.1 m... plus dispersion) has more
     * amplitude than the shallow one (c=3.13, reach 1.6 m). */
    const float a_shallow = fabsf(jce_water_ripple_height(shallow, 5.0f, 0.0f));
    const float a_deep    = fabsf(jce_water_ripple_height(deep,    5.0f, 0.0f));
    TEST_ASSERT_TRUE_MESSAGE(a_deep > a_shallow,
        "depth does not change the wave speed - this is not shallow water");

    /* And the CFL bound must follow the depth, or the stepper would be using
     * the wrong substep count for one of them. */
    TEST_ASSERT_TRUE(jce_water_ripple_max_dt(deep) <
                     jce_water_ripple_max_dt(shallow));
    jce_water_ripple_destroy(shallow);
    jce_water_ripple_destroy(deep);
}

/* ── 7. Land reflects, and cannot be disturbed ─────────────────────────── */

static void test_land_reflects_and_holds_still(void)
{
    const int N = 64;
    JceWaterRipple *r = make(0.0f, 2.0f, N, 32.0f);

    /* Right half is land. */
    float *depth = (float *)malloc(sizeof(float) * (size_t)N * (size_t)N);
    TEST_ASSERT_NOT_NULL(depth);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
            depth[j * N + i] = (i < N / 2) ? 2.0f : -1.0f;
    TEST_ASSERT_TRUE(jce_water_ripple_set_depth(r, depth, N * N));

    /* A depth map of the wrong length must be REJECTED whole, not applied
     * halfway -- a partially written bed is a cliff through the pond. */
    TEST_ASSERT_FALSE(jce_water_ripple_set_depth(r, depth, N * N - 1));

    jce_water_ripple_impulse(r, -8.0f, 0.0f, 2.0f, 1.0f);
    const float e0 = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE(e0 > 0.0f);

    float worst = e0;
    for (int i = 0; i < 1500; ++i) {
        jce_water_ripple_step(r, 1.0f / 60.0f);
        const float e = jce_water_ripple_energy(r);
        TEST_ASSERT_TRUE_MESSAGE(e == e, "NaN with a depth discontinuity");
        if (e > worst) worst = e;
    }
    /* THE reason the flux uses face-averaged c2: an asymmetric Laplacian has
     * no conserved energy and a depth STEP -- which is what a bank is -- pumps
     * it. If this ever fires, the operator stopped being self-adjoint. */
    /* See test_depth_step_does_not_pump_energy for why the bound is 1.20 and
     * why THIS test cannot catch an asymmetric Laplacian: at a water/land face
     * the neighbour's c2 is zero, so the face average and the neighbour's own
     * value both behave like a reflector. What this test does pin is that a
     * hard boundary does not destabilise the scheme. */
    TEST_ASSERT_TRUE_MESSAGE(worst <= e0 * 1.20f,
        "energy grew at the land boundary");

    /* Land never moves, however hard it is hit. */
    jce_water_ripple_impulse(r, 8.0f, 0.0f, 3.0f, 10.0f);
    jce_water_ripple_step(r, 1.0f / 60.0f);
    const float *h = jce_water_ripple_height_data(r);
    for (int j = 0; j < N; ++j)
        for (int i = N / 2; i < N; ++i)
            TEST_ASSERT_EQUAL_FLOAT(0.0f, h[j * N + i]);

    free(depth);
    jce_water_ripple_destroy(r);
}

/* ── 8. Stepping is deterministic and frame-rate independent in the large ─ */

static void test_stepping_is_deterministic(void)
{
    JceWaterRipple *a = make(0.4f, 2.0f, 48, 24.0f);
    JceWaterRipple *b = make(0.4f, 2.0f, 48, 24.0f);
    jce_water_ripple_impulse(a, 1.5f, -2.0f, 2.0f, 1.3f);
    jce_water_ripple_impulse(b, 1.5f, -2.0f, 2.0f, 1.3f);

    for (int i = 0; i < 300; ++i) {
        jce_water_ripple_step(a, 1.0f / 60.0f);
        jce_water_ripple_step(b, 1.0f / 60.0f);
    }
    /* Bit-identical, not close: two readers of the same simulation must agree
     * exactly or a replay diverges in the picture while every stored number
     * still matches. */
    TEST_ASSERT_EQUAL_MEMORY(jce_water_ripple_height_data(a),
                             jce_water_ripple_height_data(b),
                             sizeof(float) * 48u * 48u);
    jce_water_ripple_destroy(a);
    jce_water_ripple_destroy(b);
}

/* ── 9. A pathological dt is bounded, not fatal ────────────────────────── */

static void test_huge_dt_is_capped_not_unstable(void)
{
    JceWaterRipple *r = make(0.2f, 2.0f, 48, 24.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 2.0f, 1.0f);
    const float e0 = jce_water_ripple_energy(r);

    /* Ten seconds in one call. The substep cap must bind and the surplus must
     * be dropped -- stretching the substep past CFL instead would diverge. */
    for (int i = 0; i < 20; ++i) jce_water_ripple_step(r, 10.0f);

    const float e = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE_MESSAGE(e == e, "a huge dt produced NaN");
    TEST_ASSERT_TRUE_MESSAGE(e <= e0 * 1.20f,
        "a huge dt grew the energy");

    /* Non-finite and non-positive dt are no-ops, not rewinds. */
    const float before = jce_water_ripple_energy(r);
    jce_water_ripple_step(r, 0.0f);
    jce_water_ripple_step(r, -1.0f);
    jce_water_ripple_step(r, nanf(""));
    TEST_ASSERT_EQUAL_FLOAT(before, jce_water_ripple_energy(r));
    jce_water_ripple_destroy(r);
}

/* ── 10. Sampling: interpolated inside, exactly zero outside ───────────── */

static void test_sampling_is_zero_outside_the_grid(void)
{
    JceWaterRipple *r = make(0.0f, 2.0f, 32, 16.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 3.0f, 1.0f);
    for (int i = 0; i < 10; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    TEST_ASSERT_TRUE(fabsf(jce_water_ripple_height(r, 0.0f, 0.0f)) > 1e-6f);

    /* Outside is ABSENT, not clamped to the rim -- clamping would smear the
     * edge value across the whole world. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r,  100.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, -100.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, 0.0f,  100.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, 0.0f, -100.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_ripple_height(r, nanf(""), 0.0f));

    /* An impulse entirely off the grid is dropped, not clamped onto the rim:
     * a body walking out of the pond must not keep hammering its edge. */
    const float e = jce_water_ripple_energy(r);
    jce_water_ripple_impulse(r, 500.0f, 500.0f, 1.0f, 5.0f);
    TEST_ASSERT_EQUAL_FLOAT(e, jce_water_ripple_energy(r));
    jce_water_ripple_destroy(r);
}

/* ── 11. The CFL bound is the one the stepper obeys ────────────────────── */

static void test_max_dt_is_the_cfl_bound(void)
{
    JceWaterRipple *r = make(0.0f, 4.0f, 65, 64.0f);   /* dx = 1 m exactly */
    /* c = sqrt(g*4) = 6.2632; dt_max = dx / (c*sqrt2) = 1/8.857 = 0.11291 */
    const float expect = 1.0f / (sqrtf(9.80665f * 4.0f) * 1.41421356f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expect, jce_water_ripple_max_dt(r));

    /* Halving the spacing halves the bound: the cost of a finer grid is
     * quadratic, and this is the half that is not obvious. */
    JceWaterRipple *fine = make(0.0f, 4.0f, 129, 64.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expect * 0.5f,
                             jce_water_ripple_max_dt(fine));
    jce_water_ripple_destroy(fine);
    jce_water_ripple_destroy(r);
}

/* -- 12. A WATER-TO-WATER depth step is what tests the operator's symmetry --
 *
 * test_land_reflects_and_holds_still was written to catch an asymmetric
 * Laplacian and does not: at a water/LAND face the neighbour's c2 is zero, so
 * the face average (cc/2) and the neighbour's own value (0) both behave like a
 * reflector and the difference never accumulates. Replacing the face average
 * with the neighbour's c2 survived that test.
 *
 * A shelf between two DEPTHS is the discriminating case: 4 m beside 0.5 m
 * makes the two forms differ by a factor of four across the step, in opposite
 * directions on the two sides -- which is precisely the antisymmetry that
 * destroys the conserved energy. */

static float shelf_worst_energy(int shelf)
{
    const int N = 64;
    JceWaterRipple *r = make(0.0f, 4.0f, N, 32.0f);        /* damping OFF */

    if (shelf) {
        float *depth = (float *)malloc(sizeof(float) * (size_t)N * (size_t)N);
        TEST_ASSERT_NOT_NULL(depth);
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i)
                depth[j * N + i] = (i < N / 2) ? 4.0f : 0.5f;
        TEST_ASSERT_TRUE(jce_water_ripple_set_depth(r, depth, N * N));
        free(depth);
    }

    jce_water_ripple_impulse(r, -8.0f, 0.0f, 2.0f, 1.0f);
    const float e0 = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE(e0 > 0.0f);

    float worst = e0;
    for (int i = 0; i < 3000; ++i) {
        jce_water_ripple_step(r, 1.0f / 60.0f);
        const float e = jce_water_ripple_energy(r);
        TEST_ASSERT_TRUE_MESSAGE(e == e, "energy went NaN");
        if (e > worst) worst = e;
    }
    jce_water_ripple_destroy(r);
    return worst / e0;
}

static void test_depth_step_does_not_pump_energy(void)
{
    /* The shelf is compared against the SAME run without a shelf, not against
     * an absolute number, and that is what makes this test sharp.
     *
     * Symplectic Euler does not conserve E exactly -- it conserves a nearby
     * "shadow" energy, so E oscillates with an amplitude that grows with
     * dt * omega_max. Measured here that residual is 5.70%, and the
     * uniform-depth control gives 5.70% TOO, to four figures. So the residual
     * belongs to the time integrator and not to the bed, and the question this
     * test should ask is not "is E within x%" -- which would be pinning a
     * property of dt -- but "does a depth STEP add anything to it".
     *
     * Measured, 3000 steps, this grid:
     *
     *   face-averaged c2 (shipped)     worst E/E0 = 1.0570, settling to 1.0047
     *   uniform depth, no shelf        worst E/E0 = 1.0570
     *   neighbour c2 (asymmetric)      worst E/E0 = 3.1857, still climbing
     *
     * The asymmetric form is not a rounding difference; it has no conserved
     * quantity at all, so a depth discontinuity -- which is what every shelf,
     * bank and shoreline IS -- pumps energy in without bound. */
    const float flat  = shelf_worst_energy(0);
    const float shelf = shelf_worst_energy(1);

    TEST_ASSERT_TRUE_MESSAGE(shelf <= flat * 1.10f,
        "a depth step pumped energy that a flat bed did not - the Laplacian "
        "is not self-adjoint (face-averaged c2 is what makes it symmetric)");
    /* And the flat control must itself be sane, or the ratio above could be
     * satisfied by two equally broken runs. */
    TEST_ASSERT_TRUE_MESSAGE(flat <= 1.20f,
        "even a flat bed grew - this is not the shelf");
}

/* -- 13. Heavy damping must damp, not invert ---------------------------- */

static void test_heavy_damping_cannot_destabilise(void)
{
    /* k*dt > 2 is where the obvious form, v -= k*dt*v, changes sign and then
     * amplifies -- so a caller asking for very heavy damping would get the
     * opposite of damping. With dx = 32/63 and H = 2 m the substep is about
     * 0.08 s, so k = 100 puts k*dt at 8, far past that boundary. */
    JceWaterRipple *r = make(100.0f, 2.0f, 48, 32.0f);
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 3.0f, 2.0f);
    const float e0 = jce_water_ripple_energy(r);
    TEST_ASSERT_TRUE(e0 > 0.0f);

    float worst = e0;
    for (int i = 0; i < 400; ++i) {
        jce_water_ripple_step(r, 1.0f / 60.0f);
        const float e = jce_water_ripple_energy(r);
        TEST_ASSERT_TRUE_MESSAGE(e == e, "heavy damping produced NaN");
        if (e > worst) worst = e;
    }
    TEST_ASSERT_TRUE_MESSAGE(worst <= e0 * 1.20f,
        "heavy damping AMPLIFIED - damping must be a division, not a "
        "subtraction that flips sign once k*dt passes 1");
    TEST_ASSERT_TRUE_MESSAGE(jce_water_ripple_energy(r) < e0 * 0.01f,
        "heavy damping did not settle the pond");
    jce_water_ripple_destroy(r);
}

/* -- 14. The substep cap bounds WORK, and does so exactly ----------------
 *
 * Not stability: without the cap the substep is dt/ceil(dt/safe), which is
 * never larger than safe, so an uncapped huge dt is stable and merely
 * expensive. The property to pin is therefore the one that is actually there
 * -- that a huge dt advances by exactly RIPPLE_MAX_SUBSTEPS substeps of the
 * safe size and no more.
 *
 * Asserted by construction rather than by timing: one pond gets a single
 * enormous step, another gets the capped number of safe-sized ones, and the
 * two grids must come out bit-identical. */

static void test_substep_cap_advances_exactly_the_capped_amount(void)
{
    JceWaterRipple *a = make(0.0f, 2.0f, 32, 16.0f);
    JceWaterRipple *b = make(0.0f, 2.0f, 32, 16.0f);
    jce_water_ripple_impulse(a, 0.0f, 0.0f, 2.0f, 1.0f);
    jce_water_ripple_impulse(b, 0.0f, 0.0f, 2.0f, 1.0f);

    const float safe = jce_water_ripple_max_dt(a) * 0.9f;

    jce_water_ripple_step(a, 1000.0f);           /* absurd: the cap binds */
    for (int i = 0; i < 16; ++i)                 /* RIPPLE_MAX_SUBSTEPS */
        jce_water_ripple_step(b, safe);

    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(jce_water_ripple_height_data(a),
                                     jce_water_ripple_height_data(b),
                                     sizeof(float) * 32u * 32u,
        "a capped step did not advance by exactly the capped number of safe "
        "substeps");
    jce_water_ripple_destroy(a);
    jce_water_ripple_destroy(b);
}

/* -- 15. Water that becomes land must DRAIN, not freeze ------------------
 *
 * The bed is not fixed: a tide, a terrain sculpt or a filling lock all change
 * it at runtime, and a cell that was water can become land while it is holding
 * a wave. Nothing else in this file covers that -- a mutation removing the
 * height pin survived every other test, because a cell that is land from the
 * start never receives velocity anyway and so never moves whether it is pinned
 * or not. The pin only earns its place on the TRANSITION.
 *
 * Left unpinned the old height stays there forever: a standing puddle on top
 * of a rock, which reads as a rendering bug rather than a solver one. */

static void test_water_that_becomes_land_drains(void)
{
    const int N = 32;
    JceWaterRipple *r = make(0.0f, 2.0f, N, 16.0f);

    /* Disturb the whole pond, then let it develop so every cell holds
     * something. */
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 7.0f, 1.0f);
    for (int i = 0; i < 20; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    const float *h = jce_water_ripple_height_data(r);
    int wet = 0;
    for (int i = 0; i < N * N; ++i) if (h[i] != 0.0f) ++wet;
    TEST_ASSERT_TRUE_MESSAGE(wet > N * N / 4,
        "the pond is not disturbed enough for this test to mean anything");

    /* Now raise the bed under the right half above the waterline. */
    float *depth = (float *)malloc(sizeof(float) * (size_t)N * (size_t)N);
    TEST_ASSERT_NOT_NULL(depth);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i)
            depth[j * N + i] = (i < N / 2) ? 2.0f : -0.5f;
    TEST_ASSERT_TRUE(jce_water_ripple_set_depth(r, depth, N * N));

    jce_water_ripple_step(r, 1.0f / 60.0f);

    h = jce_water_ripple_height_data(r);
    for (int j = 0; j < N; ++j)
        for (int i = N / 2; i < N; ++i)
            TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, h[j * N + i],
                "water left standing on a cell that became land");

    free(depth);
    jce_water_ripple_destroy(r);
}

/* -- 16. A SLOPING bed refracts: the front bends toward the shallows -------
 *
 * test_deeper_water_carries_waves_faster pins that c depends on H, using two
 * separate ponds. This pins the consequence that actually makes a surface read
 * as water: within ONE pond, a bed that slopes makes the same wavefront travel
 * further on the deep side than the shallow side, so it turns.
 *
 * That is the entire reason the solver carries a depth map rather than a
 * constant, and it is what the terrain bathymetry feeds. With a uniform bed
 * the front stays a circle and the map is decoration.
 */

static void test_a_sloping_bed_turns_the_wavefront(void)
{
    const int N = 128;
    JceWaterRipple *r = make(0.0f, 4.0f, N, 64.0f);   /* damping OFF */

    /* A SHORE, not a gentle basin: 4 m deep out to x = -4, ramping to 0.25 m by
     * x = +4, flat beyond both.
     *
     * The width of the ramp is the whole test. Spread the same 4 m -> 0.25 m
     * over the full 64 m pond and the depth at the sample points is only
     * 2.5 m vs 1.8 m -- a speed ratio of 1.18, which is real but too small to
     * separate from the scheme's own dispersion. Measured reach ratios as the
     * ramp narrows:
     *
     *     half-width 32 m   1.06      (the first version of this test)
     *     half-width  8 m   1.50
     *     half-width  4 m   2.00      <- used here
     *     step             2.56
     *
     * Monotone in the steepness, which is itself the evidence that the bed is
     * driving it rather than anything else in the solver. */
    float *depth = (float *)malloc(sizeof(float) * (size_t)N * (size_t)N);
    TEST_ASSERT_NOT_NULL(depth);
    {
        const float half_w = 4.0f;
        const float step_m = 64.0f / (float)(N - 1);
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                const float wx = -32.0f + step_m * (float)i;
                float f = (wx + half_w) / (2.0f * half_w);
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                depth[j * N + i] = 4.0f + f * (0.25f - 4.0f);
            }
        }
    }
    TEST_ASSERT_TRUE(jce_water_ripple_set_depth(r, depth, N * N));

    /* Disturb the CENTRE, halfway down the ramp, and read symmetric points to
     * either side along X. */
    /* WHAT TO MEASURE: how far the front got, not how tall it is.
     *
     * The first version of this asserted that the deep side had the larger
     * amplitude at a fixed distance, and it failed -- against a solver that was
     * right. The shallow side is TALLER, and that is not a bug: a wave entering
     * shallower water slows down and grows, because the same energy flux has to
     * pass through a slower, shallower channel. Green's law puts the amplitude
     * at H^(-1/4), so a bed dropping 4 m -> 0.25 m raises the wave about 2x
     * exactly where it is travelling 4x slower.
     *
     * Refraction is about the front POSITION, so measure the reach: the
     * furthest point on each side that the disturbance has arrived at. That is
     * the quantity c = sqrt(gH) controls, and it is what makes a wave bend
     * toward a shore.
     *
     * Timing from the physics: c is 6.26 m/s deep and 1.57 m/s shallow, so in
     * 1.5 s the fronts are near 9 m and 2.4 m. Sampling out to 14 m brackets
     * both with room to spare. */
    jce_water_ripple_impulse(r, 0.0f, 0.0f, 1.5f, 1.0f);
    for (int i = 0; i < 90; ++i) jce_water_ripple_step(r, 1.0f / 60.0f);

    const float ARRIVED = 2e-4f;      /* well above the solver's own zero */
    float deep_reach = 0.0f, shallow_reach = 0.0f;
    for (float x = 0.5f; x <= 14.0f; x += 0.5f) {
        if (fabsf(jce_water_ripple_height(r, -x, 0.0f)) > ARRIVED) deep_reach    = x;
        if (fabsf(jce_water_ripple_height(r,  x, 0.0f)) > ARRIVED) shallow_reach = x;
    }

    TEST_ASSERT_TRUE_MESSAGE(deep_reach > 1.0f,
        "nothing propagated into the deep side at all");
    TEST_ASSERT_TRUE_MESSAGE(deep_reach > shallow_reach * 1.5f,
        "the front reached as far into the shallows as into the deep water - "
        "the bed is not reaching the wave speed, so waves will never bend "
        "toward a shore");

    /* And the bed must not have broken conservation: a sloping bed is a
     * continuum of depth discontinuities, which is precisely what an
     * asymmetric Laplacian pumps energy at. */
    const float e0 = jce_water_ripple_energy(r);
    float worst = e0;
    for (int i = 0; i < 1500; ++i) {
        jce_water_ripple_step(r, 1.0f / 60.0f);
        const float e = jce_water_ripple_energy(r);
        TEST_ASSERT_TRUE_MESSAGE(e == e, "NaN over a sloping bed");
        if (e > worst) worst = e;
    }
    TEST_ASSERT_TRUE_MESSAGE(worst <= e0 * 1.20f,
        "energy grew over a sloping bed");

    free(depth);
    jce_water_ripple_destroy(r);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_rejects_nonsense);
    RUN_TEST(test_undisturbed_stays_flat);
    RUN_TEST(test_energy_never_grows_undamped);
    RUN_TEST(test_energy_decays_when_damped);
    RUN_TEST(test_impulse_drives_velocity_not_height);
    RUN_TEST(test_wave_travels_at_shallow_water_speed);
    RUN_TEST(test_deeper_water_carries_waves_faster);
    RUN_TEST(test_land_reflects_and_holds_still);
    RUN_TEST(test_stepping_is_deterministic);
    RUN_TEST(test_huge_dt_is_capped_not_unstable);
    RUN_TEST(test_sampling_is_zero_outside_the_grid);
    RUN_TEST(test_max_dt_is_the_cfl_bound);
    RUN_TEST(test_depth_step_does_not_pump_energy);
    RUN_TEST(test_heavy_damping_cannot_destabilise);
    RUN_TEST(test_substep_cap_advances_exactly_the_capped_amount);
    RUN_TEST(test_water_that_becomes_land_drains);
    RUN_TEST(test_a_sloping_bed_turns_the_wavefront);
    return UNITY_END();
}
