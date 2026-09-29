/* test_jce_particles_depth.c
 *
 * Validation for FEATURE 8.3 — per-property lifetime curves + flipbook UV
 * animation in the CPU particle simulation (jce_particles.c).  These tests
 * drive the REAL jce_particles_update path (spawn → age → curve eval → UV
 * advance) and the real read-back visitor, never a mock, and lock in:
 *
 *   - an eased size curve yields the EASED value (not the linear midpoint)
 *     at mid-life, computed through the real sim;
 *   - the default LINEAR curves reproduce the legacy pure-linear interp
 *     byte-for-byte (size + colour at mid-life == linear midpoint);
 *   - the velocity-over-age curve scales travelled distance, and the flat
 *     default leaves distance bit-identical to the legacy advance;
 *   - flipbook UV reports frame 0 at birth and the last frame near death,
 *     with the correct cell sub-rect, and wraps vs clamps per config;
 *   - an emitter with neither curves nor flipbook is byte-identical to the
 *     legacy sim (identity UV rect, linear values).
 */

#include <jce/renderer/jce_particles.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_easing.h>
#include <jce/os/core/jce_math.h>

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Single-particle read-back probe ───────────────────────────────── */

typedef struct {
    int      count;
    float    size;
    jce_vec4 color;
    jce_vec3 position;
    jce_vec2 uv_offset;
    jce_vec2 uv_scale;
    uint32_t frame;
} Probe;

static void probe_visit(const JceParticleView *p, void *user)
{
    Probe *pr = (Probe *)user;
    pr->count++;
    pr->size      = p->size;
    pr->color     = p->color;
    pr->position  = p->position;
    pr->uv_offset = p->uv_offset;
    pr->uv_scale  = p->uv_scale;
    pr->frame     = p->frame;
}

static Probe probe_one(JceParticleSystem *sys, JceEmitterHandle h)
{
    Probe pr;
    memset(&pr, 0, sizeof(pr));
    jce_particles_emitter_for_each(sys, h, probe_visit, &pr);
    return pr;
}

/* Burst-only, deterministic base desc: no free-run emission, fixed lifetime,
 * zero velocity + gravity so a particle stays put unless we ask otherwise. */
static void base_desc(JceParticleEmitterDesc *d)
{
    jce_particles_desc_default(d);
    d->max_particles = 16;
    d->emit_rate     = 0.0f;
    d->lifetime_min  = 1.0f;
    d->lifetime_max  = 1.0f;
    d->velocity_min  = jce_v3(0, 0, 0);
    d->velocity_max  = jce_v3(0, 0, 0);
    d->gravity       = jce_v3(0, 0, 0);
    d->size_start    = 1.0f;
    d->size_end      = 0.0f;
    d->color_start   = jce_v4(0, 0, 0, 1);   /* RGB 0→1, A 1→0 over life */
    d->color_end     = jce_v4(1, 1, 1, 0);
    d->sub_emitter   = NULL;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

/* The default (LINEAR) curves must reproduce the legacy linear interpolation:
 * at mid-life size == midpoint and colour == midpoint. */
static void test_default_curve_is_linear(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    base_desc(&d); /* size_curve / color_curve default to LINEAR */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));
    jce_particles_emitter_burst(sys, h, 1);

    /* One step of dt=0.5 → age 0.5, t = 0.5 (mid-life). */
    jce_particles_update(sys, 0.5f);

    Probe pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_INT(1, pr.count);

    /* Linear: size 1→0 at t=0.5 == 0.5; colour 0→1 == 0.5. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, pr.size);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, pr.color.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, pr.color.w);

    jce_particles_destroy(sys);
}

/* An eased size curve must yield the EASED value at mid-life, distinct from
 * the linear midpoint — proving the curve actually shapes the real sim. */
static void test_eased_size_curve_at_midlife(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    base_desc(&d);
    d.size_curve = JCE_EASE_QUAD_IN;   /* ease(0.5) = 0.25, not 0.5 */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    TEST_ASSERT_TRUE(jce_emitter_valid(h));
    jce_particles_emitter_burst(sys, h, 1);

    jce_particles_update(sys, 0.5f); /* t = 0.5 */

    Probe pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_INT(1, pr.count);

    /* size = s0 + (s1-s0)*ease(0.5) = 1 + (0-1)*0.25 = 0.75. */
    float expect = 1.0f + (0.0f - 1.0f) * jce_ease(JCE_EASE_QUAD_IN, 0.5f);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.75f, expect); /* sanity on the model */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, expect, pr.size);

    /* And it is NOT the linear value (0.5) — the curve really bent it. */
    TEST_ASSERT_TRUE(fabsf(pr.size - 0.5f) > 0.1f);

    jce_particles_destroy(sys);
}

/* The velocity-over-age curve scales travelled distance; the flat default
 * (1→1) leaves distance bit-identical to the legacy advance. */
static void test_velocity_scale_curve(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    /* Reference emitter: default flat velocity scale (1→1), constant velocity.
     * Lifetime 1.0 so the normalised age (hence the curve) actually sweeps a
     * meaningful range; we step only to age 0.9 to keep it alive. */
    JceParticleEmitterDesc base;
    base_desc(&base);
    base.lifetime_min = 1.0f;
    base.lifetime_max = 1.0f;
    base.velocity_min = jce_v3(1, 0, 0);
    base.velocity_max = jce_v3(1, 0, 0);

    JceEmitterHandle hb = jce_particles_emitter_add(sys, &base);
    jce_particles_emitter_burst(sys, hb, 1);

    /* Scaled emitter: identical, but velocity scaled to 0 across life via a
     * QUAD curve so it travels strictly LESS than the reference. */
    JceParticleEmitterDesc sc = base;
    sc.velocity_curve       = JCE_EASE_QUAD_IN;
    sc.velocity_scale_start = 1.0f;
    sc.velocity_scale_end   = 0.0f;

    JceEmitterHandle hs = jce_particles_emitter_add(sys, &sc);
    jce_particles_emitter_burst(sys, hs, 1);

    for (int i = 0; i < 9; i++) jce_particles_update(sys, 0.1f); /* age 0.9 */

    Probe pb = probe_one(sys, hb);
    Probe ps = probe_one(sys, hs);
    TEST_ASSERT_EQUAL_INT(1, pb.count);
    TEST_ASSERT_EQUAL_INT(1, ps.count);

    /* Reference advanced velocity*time = 1 * 0.9s = 0.9 along X (the flat
     * default scale must NOT alter the legacy integration). */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.9f, pb.position.x);

    /* The decelerating curve travelled strictly less. */
    TEST_ASSERT_TRUE(ps.position.x < pb.position.x - 0.05f);
    TEST_ASSERT_TRUE(ps.position.x > 0.0f);

    jce_particles_destroy(sys);
}

/* No curves + no flipbook → byte-identical to legacy: identity UV rect,
 * frame 0, linear values. */
static void test_no_flipbook_identity_uv(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    base_desc(&d); /* flipbook_rows/cols default to 0 */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    jce_particles_emitter_burst(sys, h, 1);
    jce_particles_update(sys, 0.5f);

    Probe pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_INT(1, pr.count);

    /* Identity sub-rect: full [0,0]-[1,1] texture, frame 0. */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, pr.uv_scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, pr.uv_scale.y);
    TEST_ASSERT_EQUAL_UINT32(0u, pr.frame);

    jce_particles_destroy(sys);
}

/* Frames-over-life flipbook (fps == 0): a 2x2 sheet (4 frames) plays exactly
 * once over the lifetime.  Frame 0 at birth; last cell near death; cell
 * sub-rects are correct. */
static void test_flipbook_frames_over_life(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    JceParticleEmitterDesc d;
    base_desc(&d);
    d.lifetime_min  = 1.0f;
    d.lifetime_max  = 1.0f;
    d.flipbook_rows = 2u;
    d.flipbook_cols = 2u;        /* 4 frames */
    d.flipbook_fps  = 0.0f;      /* stretch across life */
    d.flipbook_loop = false;     /* clamp at last cell */

    JceEmitterHandle h = jce_particles_emitter_add(sys, &d);
    jce_particles_emitter_burst(sys, h, 1);

    /* Each cell is 0.5 x 0.5 of the atlas. */
    const float cell = 0.5f;

    /* At birth (age 0) the spawn set frame 0 → top-left cell. */
    Probe pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_INT(1, pr.count);
    TEST_ASSERT_EQUAL_UINT32(0u, pr.frame);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_scale.y);

    /* age 0.25 → t=0.25 → floor(0.25*4)=frame 1 → col 1,row 0. */
    jce_particles_update(sys, 0.25f);
    pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_UINT32(1u, pr.frame);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.y);

    /* age 0.50 → frame 2 → col 0,row 1. */
    jce_particles_update(sys, 0.25f);
    pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_UINT32(2u, pr.frame);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, pr.uv_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_offset.y);

    /* age 0.95 → t=0.95 → floor(3.8)=frame 3 (last) → col 1,row 1. */
    jce_particles_update(sys, 0.45f);
    pr = probe_one(sys, h);
    TEST_ASSERT_EQUAL_UINT32(3u, pr.frame);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_offset.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, cell, pr.uv_offset.y);

    jce_particles_destroy(sys);
}

/* Clamp vs wrap: with fps driving the index past the sheet end, loop=false
 * clamps to the last frame and loop=true wraps modulo. */
static void test_flipbook_clamp_and_wrap(void)
{
    JceParticleSystem *sys = jce_particles_create(jce_allocator_default());
    TEST_ASSERT_NOT_NULL(sys);

    /* 1x3 sheet (3 frames) at 10 fps; long lifetime so it never dies. */
    JceParticleEmitterDesc clampd;
    base_desc(&clampd);
    clampd.lifetime_min  = 100.0f;
    clampd.lifetime_max  = 100.0f;
    clampd.flipbook_rows = 1u;
    clampd.flipbook_cols = 3u;
    clampd.flipbook_fps  = 10.0f;
    clampd.flipbook_loop = false;   /* clamp */

    JceParticleEmitterDesc wrapd = clampd;
    wrapd.flipbook_loop = true;     /* wrap */

    JceEmitterHandle hc = jce_particles_emitter_add(sys, &clampd);
    JceEmitterHandle hw = jce_particles_emitter_add(sys, &wrapd);
    jce_particles_emitter_burst(sys, hc, 1);
    jce_particles_emitter_burst(sys, hw, 1);

    /* Advance age to 0.45s → raw index floor(0.45*10)=4, beyond [0..2]. */
    for (int i = 0; i < 9; i++) jce_particles_update(sys, 0.05f);

    Probe pc = probe_one(sys, hc);
    Probe pw = probe_one(sys, hw);
    TEST_ASSERT_EQUAL_INT(1, pc.count);
    TEST_ASSERT_EQUAL_INT(1, pw.count);

    /* Clamp: pinned to last frame (index 2). */
    TEST_ASSERT_EQUAL_UINT32(2u, pc.frame);
    /* Wrap: 4 % 3 == 1. */
    TEST_ASSERT_EQUAL_UINT32(1u, pw.frame);

    /* Cell width for a 1x3 sheet is 1/3. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f / 3.0f, pc.uv_scale.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, pc.uv_scale.y);

    jce_particles_destroy(sys);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_curve_is_linear);
    RUN_TEST(test_eased_size_curve_at_midlife);
    RUN_TEST(test_velocity_scale_curve);
    RUN_TEST(test_no_flipbook_identity_uv);
    RUN_TEST(test_flipbook_frames_over_life);
    RUN_TEST(test_flipbook_clamp_and_wrap);
    return UNITY_END();
}
