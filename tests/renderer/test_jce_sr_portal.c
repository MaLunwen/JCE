/*
 * test_jce_sr_portal.c — a CLOSED occlusion portal hides what is behind it.
 *
 * JceOcclusionPortalComponent.open and .portal_id were authored, serialised,
 * shown in the Inspector (behind an "unwired" badge) and read by nothing.
 *
 * The safety property matters more than the culling: this test is written so
 * that EVERY case where occlusion is not provable asserts NOT-occluded.  A
 * conservative occluder that culls something visible is a rendering bug that
 * shows up as geometry vanishing; one that culls nothing is merely slow.
 *
 * Scene layout, camera at the origin looking toward -Z:
 *
 *      eye ──────────► -Z
 *       |        [portal 10x10x1 @ z=-5]        [target @ z=-20]
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_camera.h>

#include <stdbool.h>
#include <string.h>

/* Internal to the scene renderer; declared here rather than pulling in
 * engine/src/middleware/scene/jce_sr_portal.h, which a test has no business
 * including.  These are non-static symbols in the engine library.
 *
 * The set-level API is the reason this test is headless at all: the portal set
 * is a camera and a few boxes, so the occlusion proof can be checked with no
 * JceRenderer, no GPU, and no window. */
struct SrPortalSet;
struct SrPortalSet *sr_portal_set_create(void);
void                sr_portal_set_destroy(struct SrPortalSet *set);
void sr_portal_set_build(struct SrPortalSet *set, JceScene *scene,
                         const JceCamera *camera, float aspect,
                         bool homogeneous_depth);
bool sr_portal_set_occludes(const struct SrPortalSet *set,
                            jce_vec3 wmin, jce_vec3 wmax);

typedef struct {
    JceScene           *scene;
    JceCamera          *cam;
    struct SrPortalSet *set;
    JceEntity           portal;
} Fixture;

static void fx_open(Fixture *f, bool portal_open, jce_vec3 portal_size,
                    jce_vec3 portal_pos)
{
    f->scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(f->scene);

    f->portal = jce_scene_create_entity(f->scene, "Portal");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.position = portal_pos;
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(f->scene, f->portal, &tf);

    JceOcclusionPortalComponent op;
    memset(&op, 0, sizeof op);
    op.size      = portal_size;
    op.open      = portal_open;
    op.portal_id = 7;
    jce_scene_set_occlusion_portal(f->scene, f->portal, &op);

    JceCameraDesc cd;
    memset(&cd, 0, sizeof cd);
    cd.mode       = JCE_CAMERA_PERSPECTIVE;
    cd.position   = jce_v3(0.0f, 0.0f, 0.0f);
    cd.target     = jce_v3(0.0f, 0.0f, -1.0f);
    cd.up         = jce_v3(0.0f, 1.0f, 0.0f);
    cd.fov_deg    = 60.0f;
    cd.near_plane = 0.1f;
    cd.far_plane  = 500.0f;
    f->cam = jce_camera_create(&cd);
    TEST_ASSERT_NOT_NULL(f->cam);

    f->set = sr_portal_set_create();
    TEST_ASSERT_NOT_NULL(f->set);
    sr_portal_set_build(f->set, f->scene, f->cam, 16.0f / 9.0f, false);
}

static void fx_close(Fixture *f)
{
    sr_portal_set_destroy(f->set);
    jce_camera_destroy(f->cam);
    jce_scene_destroy(f->scene);
}

/* A 1x1x1 box centred at `c`. */
static void unit_box(jce_vec3 c, jce_vec3 *mn, jce_vec3 *mx)
{
    *mn = jce_v3(c.x - 0.5f, c.y - 0.5f, c.z - 0.5f);
    *mx = jce_v3(c.x + 0.5f, c.y + 0.5f, c.z + 0.5f);
}

static const jce_vec3 k_wall  = { 10.0f, 10.0f, 1.0f };   /* full extent */
static const jce_vec3 k_at_z5 = { 0.0f, 0.0f, -5.0f };

/* ── the feature ─────────────────────────────────────────────────────── */

static void test_closed_portal_hides_what_is_behind_it(void)
{
    Fixture f;
    fx_open(&f, false, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    unit_box(jce_v3(0.0f, 0.0f, -20.0f), &mn, &mx);
    TEST_ASSERT_TRUE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "a 10x10 closed portal 5 m ahead must hide a 1 m box 20 m ahead");
    fx_close(&f);
}

static void test_open_portal_hides_nothing(void)
{
    Fixture f;
    fx_open(&f, true, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    unit_box(jce_v3(0.0f, 0.0f, -20.0f), &mn, &mx);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "`open` is the field this whole change exists for: an OPEN portal "
        "must occlude nothing, or the flag is not what decides");
    fx_close(&f);
}

/* Pins the geometry the edge case below leans on: 15 m off-axis at z = -20 is
 * INSIDE the closed portal's shadow, so "16..22 is partly outside" is a claim
 * about where the edge is, not about the test being too weak to cull. */
static void test_object_well_inside_the_shadow_is_hidden(void)
{
    Fixture f;
    fx_open(&f, false, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    unit_box(jce_v3(15.0f, 0.0f, -20.0f), &mn, &mx);
    TEST_ASSERT_TRUE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "the portal's shadow is 40 m wide at z = -20; a box at x = 15 is "
        "inside it");
    fx_close(&f);
}

/* ── the safety property: everything unproven stays visible ──────────── */

static void test_object_beside_the_portal_stays_visible(void)
{
    Fixture f;
    fx_open(&f, false, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    /* 20 m out and 30 m to the side: far outside the portal's cone. */
    unit_box(jce_v3(30.0f, 0.0f, -20.0f), &mn, &mx);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "an object beside the portal is plainly visible; culling it would "
        "make geometry vanish");
    fx_close(&f);
}

static void test_object_in_front_of_the_portal_stays_visible(void)
{
    Fixture f;
    fx_open(&f, false, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    unit_box(jce_v3(0.0f, 0.0f, -2.0f), &mn, &mx);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "the depth half of the test must bite: an object BETWEEN the eye and "
        "the portal is in front of it, not behind it");
    fx_close(&f);
}

static void test_object_peeking_past_the_edge_stays_visible(void)
{
    Fixture f;
    fx_open(&f, false, k_wall, k_at_z5);
    jce_vec3 mn, mx;
    /* The portal is 10 m wide at z = -5, so its cone is 40 m wide at z = -20:
     * the shadow edge is x = 20.  This box spans 16..22 -- its CENTRE (19) is
     * inside the cone and its far corner is outside, so a centre-only or
     * rect-overlap test would call it hidden while a strip of it is in view. */
    mn = jce_v3(16.0f, -0.5f, -20.5f);
    mx = jce_v3(22.0f,  0.5f, -19.5f);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "partial coverage is not coverage -- the screen-space containment "
        "test must require ALL corners inside, not the centre");
    fx_close(&f);
}

static void test_camera_inside_the_portal_culls_nothing(void)
{
    Fixture f;
    /* A portal box swallowing the eye: some corner is behind the eye plane,
     * so its projection is not a bounded convex quad and no guarantee holds. */
    fx_open(&f, false, jce_v3(40.0f, 40.0f, 40.0f), jce_v3(0.0f, 0.0f, 0.0f));
    jce_vec3 mn, mx;
    unit_box(jce_v3(0.0f, 0.0f, -100.0f), &mn, &mx);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "a portal straddling the eye plane must be dropped, not guessed at");
    fx_close(&f);
}

static void test_no_portals_is_never_occluded(void)
{
    Fixture f;
    /* Degenerate size: gathered as nothing, so the set stays empty. */
    fx_open(&f, false, jce_v3(0.0f, 0.0f, 0.0f), k_at_z5);
    jce_vec3 mn, mx;
    unit_box(jce_v3(0.0f, 0.0f, -20.0f), &mn, &mx);
    TEST_ASSERT_FALSE_MESSAGE(sr_portal_set_occludes(f.set, mn, mx),
        "a scene with no usable portal must take the zero-cost path");
    fx_close(&f);
}

/* ── portal_id: the pairing key gets an operation that pairs ─────────── */

static void test_set_open_by_id_moves_the_whole_group(void)
{
    JceScene *s = jce_scene_create();
    JceOcclusionPortalComponent op;

    for (int i = 0; i < 3; ++i) {
        JceEntity e = jce_scene_create_entity(s, "P");
        memset(&op, 0, sizeof op);
        op.size      = jce_v3(1.0f, 1.0f, 1.0f);
        op.open      = true;
        op.portal_id = (i == 2) ? 9 : 7;    /* two in group 7, one in group 9 */
        jce_scene_set_occlusion_portal(s, e, &op);
    }

    int n = jce_scene_occlusion_portals_set_open(s, 7, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n,
        "closing group 7 must move exactly the two portals carrying that id");

    n = jce_scene_occlusion_portals_set_open(s, 7, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n,
        "the count is 'how many CHANGED' -- a second close moves nothing, "
        "which is how a caller tells 'no such id' from 'already closed'");

    n = jce_scene_occlusion_portals_set_open(s, 42, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n, "an id no portal carries changes nothing");

    jce_scene_destroy(s);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_closed_portal_hides_what_is_behind_it);
    RUN_TEST(test_open_portal_hides_nothing);
    RUN_TEST(test_object_well_inside_the_shadow_is_hidden);
    RUN_TEST(test_object_beside_the_portal_stays_visible);
    RUN_TEST(test_object_in_front_of_the_portal_stays_visible);
    RUN_TEST(test_object_peeking_past_the_edge_stays_visible);
    RUN_TEST(test_camera_inside_the_portal_culls_nothing);
    RUN_TEST(test_no_portals_is_never_occluded);
    RUN_TEST(test_set_open_by_id_moves_the_whole_group);
    return UNITY_END();
}
