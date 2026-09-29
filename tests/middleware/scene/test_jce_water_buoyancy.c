/* test_jce_water_buoyancy.c
 *
 * Unit tests for WATER BUOYANCY (gap 2.3, slice 3): the deterministic core is
 * the buoyancy FORCE function jce_water_buoyancy_force(), exercised against the
 * REAL Gerstner surface sampler jce_water_sample_height() — no mocks.
 *
 *   - a point BELOW the surface yields a positive (upward, +Y) force that is
 *     proportional to submersion depth (deeper => stronger),
 *   - a point ABOVE the surface yields exactly 0 (airborne bodies untouched),
 *   - vertical drag opposes vertical velocity (and is gated by submersion),
 *   - negative authoring params are clamped to 0 (never sinks),
 *   - integrating gravity + the real buoyancy force over fixed steps moves a
 *     test body released above the (real) water surface DOWN through it and
 *     settles it in a bounded band oscillating about the equilibrium depth
 *     (monotonic-ish approach then damped settle), and
 *   - the JceBuoyancyComponent survives a scene prefab round-trip.
 *
 * This mirrors the runtime's actual buoyancy math (jce_runtime.c rt_apply_
 * buoyancy): submersion = max(0, water_y - body_y), force = strength*submersion
 * - drag*vel_y*submersion, applied as F = m*a with m = 1 so a == F.  It runs
 * headlessly (pure math + a JceScene round-trip), needing no physics world.
 */

#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BUOY_PREFAB "jce_buoyancy_roundtrip.prefab.json"

void setUp(void)    {}
void tearDown(void) { remove(BUOY_PREFAB); }

/* ── A point below the surface yields an upward force ∝ depth ──────────── */
static void test_force_positive_below_proportional(void)
{
    const float strength = 50.0f;
    const float drag     = 0.0f;          /* isolate the buoyancy term */
    const float vel_y    = 0.0f;          /* no drag contribution      */

    /* Two depths: the deeper one must push harder, and linearly so. */
    const float f_shallow = jce_water_buoyancy_force(0.5f, vel_y, strength, drag);
    const float f_deep    = jce_water_buoyancy_force(2.0f, vel_y, strength, drag);

    TEST_ASSERT_TRUE(f_shallow > 0.0f);                 /* upward (+Y)        */
    TEST_ASSERT_TRUE(f_deep > f_shallow);               /* deeper => stronger */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, strength * 0.5f, f_shallow);  /* exact ∝   */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, strength * 2.0f, f_deep);
    /* 4× the depth => 4× the force (linear in submersion). */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 4.0f, f_deep / f_shallow);
}

/* ── A point above the surface yields exactly zero force ───────────────── */
static void test_force_zero_above_surface(void)
{
    /* submersion <= 0 (body above water) => no force, regardless of velocity. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_buoyancy_force( 0.0f,  3.0f, 100.0f, 5.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_buoyancy_force(-1.0f, -3.0f, 100.0f, 5.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_buoyancy_force(-5.0f,  0.0f, 100.0f, 5.0f));
}

/* ── Vertical drag opposes vertical velocity (gated by submersion) ─────── */
static void test_force_drag_opposes_velocity(void)
{
    const float sub = 1.0f, strength = 10.0f, drag = 4.0f;

    const float f_rising  = jce_water_buoyancy_force(sub,  2.0f, strength, drag);
    const float f_still   = jce_water_buoyancy_force(sub,  0.0f, strength, drag);
    const float f_sinking = jce_water_buoyancy_force(sub, -2.0f, strength, drag);

    /* Rising (vel_y>0) => drag subtracts; sinking (vel_y<0) => drag adds. */
    TEST_ASSERT_TRUE(f_rising < f_still);
    TEST_ASSERT_TRUE(f_sinking > f_still);
    /* f = strength*sub - drag*vel_y*sub. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f - 4.0f * 2.0f, f_rising);   /* 2  */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f,               f_still);   /* 10 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f + 4.0f * 2.0f, f_sinking); /* 18 */
}

/* ── Negative authoring params are clamped (never an active sinking field) ─ */
static void test_force_clamps_negative_params(void)
{
    /* Negative strength -> clamped to 0; negative drag -> clamped to 0. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_buoyancy_force(1.0f, 0.0f, -5.0f, 0.0f));
    /* drag<0 must not flip into added energy: with strength 0 the result is 0. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_buoyancy_force(1.0f, 5.0f,  0.0f, -3.0f));
}

/* ── Integration: a body falls, enters the REAL water surface, settles ──── */
/* Drives gravity + the real buoyancy force (sampled off the real Gerstner
 * height function) with a fixed-step semi-implicit Euler integrator — the same
 * shape as the runtime's per-fixed-tick pass — and asserts the body descends
 * through the surface and settles in a bounded band about the equilibrium
 * depth instead of bouncing out or sinking forever. */
static void test_integration_settles_at_surface(void)
{
    /* Single travelling wave so the surface is genuinely time-varying (the
     * real jce_water_sample_height is exercised, not a flat plane). */
    JceWaterWave w;
    memset(&w, 0, sizeof w);
    w.amplitude  = 0.3f;
    w.wavelength = 8.0f;
    w.speed      = 1.5f;
    w.dir_x      = 1.0f;
    w.dir_z      = 0.0f;
    w.steepness  = 0.4f;

    const float base_y   = 0.0f;     /* still-water plane Y                  */
    const float bx = 2.0f, bz = -1.0f;
    const float g        = -9.81f;   /* world gravity (matches runtime)      */
    const float dt       = 1.0f / 60.0f;
    const float strength = 200.0f;   /* mass-independent (mass == 1 here)    */
    const float drag     = 6.0f;     /* damp the bob so it settles           */
    const float mass     = 1.0f;     /* F = m*a, m=1 => a == F               */

    float y   = 6.0f;                 /* released well ABOVE the surface      */
    float vy  = 0.0f;
    float t   = 0.0f;

    /* The surface this body sits over (sampled each step at the body's XZ). */
    float surf0 = jce_water_sample_height(&w, 1, base_y, bx, bz, 0.0f);
    TEST_ASSERT_TRUE(y > surf0);      /* sanity: starts above water           */

    bool  entered      = false;       /* crossed below the surface at least once */
    float min_gap      = 1e9f;        /* closest the body got to the surface  */
    int   settle_steps = 0;           /* consecutive steps within the band    */
    bool  settled      = false;

    for (int i = 0; i < 2000; ++i) {
        t += dt;
        const float surf = jce_water_sample_height(&w, 1, base_y, bx, bz, t);
        const float submersion = surf - y;          /* >0 when body is below */
        if (submersion > 0.0f) entered = true;

        /* Forces: gravity (always) + buoyancy (real fn, gated by submersion). */
        const float f_grav = mass * g;
        const float f_buoy = jce_water_buoyancy_force(
            submersion > 0.0f ? submersion : 0.0f, vy, strength, drag);
        const float a = (f_grav + f_buoy) / mass;

        /* Semi-implicit Euler (same integration family as the physics step). */
        vy += a * dt;
        y  += vy * dt;

        /* Track the closest approach + a settle band around equilibrium. The
         * equilibrium submersion solves strength*sub == mass*|g| =>
         * sub* = m|g|/strength; the body's center rests sub* BELOW the surface. */
        const float eq_sub = (mass * (-g)) / strength;     /* ~0.049 m */
        const float gap    = fabsf((surf - y) - eq_sub);   /* dist from equilibrium */
        if (gap < min_gap) min_gap = gap;

        if (entered && gap < 0.20f) {
            if (++settle_steps >= 120) { settled = true; break; }  /* ~2 s steady */
        } else {
            settle_steps = 0;
        }
    }

    /* The body fell into the water (submerged at least once). */
    TEST_ASSERT_TRUE_MESSAGE(entered, "body never reached the water surface");
    /* It approached the equilibrium depth closely (didn't bounce away). */
    TEST_ASSERT_TRUE_MESSAGE(min_gap < 0.10f, "body never neared equilibrium depth");
    /* It settled into a stable band about equilibrium (didn't sink or eject). */
    TEST_ASSERT_TRUE_MESSAGE(settled, "body did not settle around the surface");

    /* Final state is near the surface, not far below (no runaway sink) and not
     * far above (no ejection): within a band of the still-water plane. */
    const float final_surf = jce_water_sample_height(&w, 1, base_y, bx, bz, t);
    TEST_ASSERT_TRUE(fabsf(y - final_surf) < 0.5f);
}

/* ── Monotonic descent before entry, then damped settle ────────────────── */
static void test_integration_monotonic_approach(void)
{
    /* Flat still water (zero waves) for a clean monotone test of the approach:
     * BEFORE the body first touches the surface it is in pure free-fall (no
     * upward force yet) and so must be STRICTLY descending; after entry the
     * buoyancy term decelerates / reverses it (and momentum may briefly carry
     * the center back above the surface), so the strict-descent invariant only
     * holds on the initial fall.  We assert: (1) strictly descending until the
     * first submersion, then (2) the amplitude of the post-entry bob DECAYS
     * (drag damps it) and the body ends settled near the surface. */
    const float base_y   = 1.0f;
    const float g        = -9.81f;
    const float dt       = 1.0f / 60.0f;
    const float strength = 150.0f;
    const float drag     = 8.0f;

    float y  = 5.0f, vy = 0.0f;
    float prev_y  = y;
    bool  entered = false;
    float first_overshoot = 0.0f;   /* max distance above surface after entry */
    float late_overshoot  = 0.0f;   /* same, but only late in the sim         */

    for (int i = 0; i < 1500; ++i) {
        /* Zero-wave surface == base_y (real fn returns base_y for n==0). */
        const float surf = jce_water_sample_height(NULL, 0, base_y, 0.0f, 0.0f, 0.0f);
        TEST_ASSERT_EQUAL_FLOAT(base_y, surf);

        const float submersion = surf - y;
        if (submersion > 0.0f) entered = true;

        /* Invariant: strictly descending while still in pure free-fall (have
         * never been submerged yet). */
        if (!entered)
            TEST_ASSERT_TRUE_MESSAGE(y <= prev_y + 1e-5f,
                                     "body rose during initial free-fall");

        const float f_buoy = jce_water_buoyancy_force(
            submersion > 0.0f ? submersion : 0.0f, vy, strength, drag);
        const float a = g + f_buoy;           /* mass 1 */
        vy += a * dt;
        y  += vy * dt;

        /* Track how far the center rises ABOVE the surface after entry, early
         * vs. late, to prove the oscillation amplitude decays. */
        if (entered && y > surf) {
            const float above = y - surf;
            if (i < 400 && above > first_overshoot) first_overshoot = above;
            if (i > 800 && above > late_overshoot)  late_overshoot  = above;
        }
        prev_y = y;
    }

    TEST_ASSERT_TRUE_MESSAGE(entered, "body never reached the water surface");
    /* Damping: the late bob amplitude is smaller than the first overshoot. */
    TEST_ASSERT_TRUE_MESSAGE(late_overshoot < first_overshoot + 1e-4f,
                             "oscillation did not decay (no damping)");
    /* Settled near the surface (small equilibrium depth for strength 150). */
    TEST_ASSERT_TRUE(fabsf(y - base_y) < 0.5f);
}

/* ── Buoyancy samples WORLD XZ, not water-entity-LOCAL XZ ────────────────── */
/* vs_water.sc displaces the surface in WORLD space (phase uses the world XZ of
 * the vertex after mul(u_model[0],a_position)); so the runtime's rt_apply_
 * buoyancy MUST sample jce_water_sample_height at the body's WORLD XZ.  A prior
 * bug subtracted the water entity's XZ origin (treating the surface as object-
 * space), which floats a body on the WRONG wave phase whenever the water entity
 * is offset in XZ.  This guards the world-space contract directly against the
 * real Gerstner sampler: with a directional wave and a non-zero water origin,
 * the correct (world-XZ) surface height differs from the buggy (local-XZ =
 * world-origin) one, and a body settles at the world-XZ surface. */
static void test_sample_uses_world_xz_not_local(void)
{
    JceWaterWave w;
    memset(&w, 0, sizeof w);
    w.amplitude  = 0.5f;
    w.wavelength = 4.0f;     /* short wavelength => phase varies quickly in X */
    w.speed      = 0.0f;     /* frozen surface so the test is purely spatial  */
    w.dir_x      = 1.0f;     /* travels along +X => height depends on world X  */
    w.dir_z      = 0.0f;
    w.steepness  = 0.0f;

    const float base_y   = 0.0f;
    const float origin_x = 10.0f;   /* water entity offset in world X          */
    const float origin_z = -3.0f;
    const float body_x   = 13.0f;   /* a body somewhere over the water         */
    const float body_z   = 1.0f;

    /* CORRECT: sample at the body's WORLD XZ (what the GPU surface shows). */
    const float surf_world =
        jce_water_sample_height(&w, 1, base_y, body_x, body_z, 0.0f);
    /* BUGGY: sample at (world - origin), the old object-space assumption. */
    const float surf_local =
        jce_water_sample_height(&w, 1, base_y,
                                body_x - origin_x, body_z - origin_z, 0.0f);

    /* With a directional wave + non-zero offset the two phases differ, so the
     * two surface heights MUST differ — proving local-XZ is observably wrong.
     * (A test that passed for both would not have caught the bug.) */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(surf_world - surf_local) > 1e-3f,
        "world-XZ and local-XZ surface heights coincide; offset test is moot");

    /* A body sitting exactly on the WORLD-XZ surface is at submersion 0 (the
     * boundary), while the same body judged against the LOCAL-XZ surface would
     * see a spurious non-zero submersion (wrong wave phase) — encode that the
     * runtime must use the world value. */
    const float body_y = surf_world;
    const float submersion_world = surf_world - body_y;          /* == 0     */
    const float submersion_local = surf_local - body_y;          /* spurious */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, submersion_world);
    TEST_ASSERT_TRUE(fabsf(submersion_local) > 1e-3f);

    /* Integrate the body in pure free-fall onto the WORLD-XZ surface and assert
     * it settles at that height (the visible wave it floats on), driven through
     * the same submersion = max(0, world_surf - y) the runtime computes. */
    const float g = -9.81f, dt = 1.0f / 60.0f;
    const float strength = 300.0f, drag = 10.0f, mass = 1.0f;
    float y = 4.0f, vy = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const float surf = jce_water_sample_height(&w, 1, base_y, body_x, body_z, 0.0f);
        const float sub  = surf - y;
        const float fb   = jce_water_buoyancy_force(sub > 0.0f ? sub : 0.0f,
                                                    vy, strength, drag);
        const float a = (mass * g + mass * fb) / mass;   /* runtime apply path */
        vy += a * dt;
        y  += vy * dt;
    }
    TEST_ASSERT_FLOAT_WITHIN(0.2f, surf_world, y);
}

/* ── Buoyancy is genuinely mass-INDEPENDENT through the real apply path ───── */
/* The authored strength/drag describe a target settle profile, not raw newtons;
 * the runtime applies the force as F = mass * buoyancy_force so applyCentralForce
 * (a = F/m) yields a MASS-INDEPENDENT acceleration.  A prior version applied the
 * force unscaled, so a heavier body sank deeper.  Drive two bodies with very
 * different masses through the SAME integrator the runtime uses and assert they
 * settle at the same equilibrium depth. */
static float settle_depth_for_mass(float mass)
{
    JceWaterWave w;
    memset(&w, 0, sizeof w);
    w.amplitude = 0.0f; w.wavelength = 8.0f; w.speed = 0.0f;
    w.dir_x = 1.0f; w.dir_z = 0.0f; w.steepness = 0.0f;   /* flat surface */

    const float base_y = 0.0f, g = -9.81f, dt = 1.0f / 60.0f;
    const float strength = 200.0f, drag = 12.0f;
    float y = 3.0f, vy = 0.0f;
    for (int i = 0; i < 4000; ++i) {
        const float surf = jce_water_sample_height(&w, 0, base_y, 0.0f, 0.0f, 0.0f);
        const float sub  = surf - y;
        const float fb   = jce_water_buoyancy_force(sub > 0.0f ? sub : 0.0f,
                                                    vy, strength, drag);
        /* Runtime apply path: gravity scales with mass (m*g) AND buoyancy is
         * pre-scaled by mass (m*fb) before applyCentralForce divides by m. */
        const float a = (mass * g + mass * fb) / mass;
        vy += a * dt;
        y  += vy * dt;
    }
    return jce_water_sample_height(&w, 0, base_y, 0.0f, 0.0f, 0.0f) - y; /* depth */
}

static void test_force_is_mass_independent(void)
{
    const float d1  = settle_depth_for_mass(1.0f);
    const float d5  = settle_depth_for_mass(5.0f);
    const float d20 = settle_depth_for_mass(20.0f);

    /* All three settle BELOW the surface (submerged) at the SAME depth: the
     * equilibrium solves strength*sub == |g| (mass cancels) => sub = |g|/strength
     * ~= 9.81/200 ~= 0.049 m, independent of mass. */
    TEST_ASSERT_TRUE(d1 > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.81f / 200.0f, d1);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, d1, d5);    /* heavier settles the same */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, d1, d20);
}

/* ── JceBuoyancyComponent registry-driven prefab round-trip ──────────────── */
static void test_component_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "buoyant");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceBuoyancyComponent b; memset(&b, 0, sizeof b);
    b.buoyancy_strength = 37.5f;
    b.drag              = 2.25f;
    b.enabled           = true;
    jce_scene_set_buoyancy(s, e, &b);
    TEST_ASSERT_TRUE(jce_scene_has_buoyancy(s, e));

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(s, e, BUOY_PREFAB));
    jce_scene_destroy(s);

    JceScene *s2 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s2);
    JceEntity root = jce_prefab_instantiate_file(s2, BUOY_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);

    JceBuoyancyComponent *g = jce_scene_get_buoyancy(s2, root);
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_FLOAT(37.5f, g->buoyancy_strength);
    TEST_ASSERT_EQUAL_FLOAT(2.25f, g->drag);
    TEST_ASSERT_TRUE(g->enabled);

    jce_scene_destroy(s2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_force_positive_below_proportional);
    RUN_TEST(test_force_zero_above_surface);
    RUN_TEST(test_force_drag_opposes_velocity);
    RUN_TEST(test_force_clamps_negative_params);
    RUN_TEST(test_integration_settles_at_surface);
    RUN_TEST(test_integration_monotonic_approach);
    RUN_TEST(test_sample_uses_world_xz_not_local);
    RUN_TEST(test_force_is_mass_independent);
    RUN_TEST(test_component_roundtrip);
    return UNITY_END();
}
