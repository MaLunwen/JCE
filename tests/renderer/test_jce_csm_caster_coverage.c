/* test_jce_csm_caster_coverage.c
 *
 * The cascade fit had no test at all, and its header states the thing it is
 * trusted to do:
 *
 *   "Without this the near plane is anchored to the camera sphere and tall
 *    casters are clipped out of the shadow map, so their shadows truncate and
 *    the truncation moves with the camera."
 *
 * It does not hold.  The near-plane push (`near_extend`) is sized from a
 * world-XZ window of +/- 2*radius around the cascade centre, while the
 * horizontal reach of a caster that shadows a cascade is
 * `height / tan(sun elevation)` -- unbounded as the sun drops, and unrelated
 * to the cascade radius.  Measured over visible ground only, eye pinned and
 * only the yaw swept, this is how many ground points are owed a caster the
 * cascade's box does not reach:
 *
 *     scene 12 m tall     sun 45  35  25  20  15  10 deg
 *       shadow_far 300          .   .   .   .   .  112
 *       shadow_far 150          .   .   4  80 284  636
 *       shadow_far  80          .  16 284 376 1224 1844
 *       shadow_far  40        160 272 1416 1584 4388 6560
 *
 * THE FIX IS NOT IN THIS FILE, AND THAT IS THE POINT OF THIS FILE.  Pushing
 * the near plane out far enough was written, proved sufficient (all 48 cells
 * to zero) -- and measured: the rendered shadow area went DOWN 2-3.6% at every
 * yaw, because a deeper box deepens `depth_range`, the shader's depth bias is
 * expressed in NORMALIZED depth, and the same bias number then means a bigger
 * world offset.  Under a 12 degree sun the ground displacement went from
 * 1.29 m to 3.75 m.  So the near plane is left where it is and the caster is
 * CLAMPED onto it instead (engine/shaders/pbr/shadow_pancake.sh).
 *
 * WHICH LEAVES THIS FILE A CONTRACT TO GUARD.  The pancake can only rescue a
 * caster the near plane rejects; it cannot rescue one rejected by the SQUARE
 * or by the FAR plane, because those reject it in x/y/w, which the clamp does
 * not touch.  So: for every receiver a camera can see, the caster standing
 * over it must fail -- if it fails at all -- ONLY at the near plane.  If that
 * ever stops being true, the pancake silently stops being a complete fix, and
 * the shape of the report will be the one the header describes.
 *
 * The camera is held at ONE world position and only its yaw is swept: that is
 * the reported shape, and it isolates the fit, since a rotation moves the
 * frustum slice's centroid around the eye while leaving every caster, the sun
 * and the scene bounds exactly where they were.
 */

#include <jce/renderer/jce_csm.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── The fixed conditions ──────────────────────────────────────────────── */

#define EYE_Y        1.8f
#define PITCH_DEG    12.0f
#define FOV_DEG      60.0f
#define ASPECT       (16.0f / 9.0f)
#define CAM_NEAR     0.1f
#define CAM_FAR      1000.0f
#define SHADOW_NEAR  0.1f        /* JCE_CSM_SHADOW_NEAR */
#define SPLIT_LAMBDA 0.7f        /* jce_scene.c default */
#define MAP_SIZE     2048

#define SCENE_HALF   200.0f
#define SCENE_FLOOR  (-1.0f)
#define SCENE_TOP    12.0f

/* Rejection classes. */
#define IN_BOX       0
#define OFF_NEAR     1           /* the pancake's job */
#define OFF_FAR      2           /* nothing rescues this */
#define OFF_SQUARE   3           /* nothing rescues this */

/* out = M * v, column-major (raw[col][row]) -- the convention already used by
 * test_jce_local_shadow.c, rather than a second one in the same directory. */
static void mul_m4v4(const jce_mat4 *m, const float v[4], float out[4])
{
    for (int r = 0; r < 4; r++)
        out[r] = m->raw[0][r] * v[0] + m->raw[1][r] * v[1]
               + m->raw[2][r] * v[2] + m->raw[3][r] * v[3];
}

/* The camera view for a yaw, with the EYE PINNED.  jce_m4_look_at is the
 * engine's own; this file does not hand-roll a view matrix. */
static jce_mat4 view_for_yaw(float yaw_deg)
{
    const float y = yaw_deg * JCE_DEG2RAD, p = PITCH_DEG * JCE_DEG2RAD;
    const jce_vec3 eye = jce_v3(0.0f, EYE_Y, 0.0f);
    const jce_vec3 fwd = jce_v3(-sinf(y) * cosf(p), -sinf(p), cosf(y) * cosf(p));
    return jce_m4_look_at(eye, jce_v3(eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z),
                          jce_v3(0.0f, 1.0f, 0.0f));
}

static int classify(const jce_mat4 *vp, jce_vec3 p, float *out_z)
{
    const float v[4] = { p.x, p.y, p.z, 1.0f };
    float c[4];
    mul_m4v4(vp, v, c);
    const float w = (fabsf(c[3]) < 1e-6f) ? 1.0f : c[3];
    const float x = c[0] / w, y = c[1] / w, z = c[2] / w;
    if (out_z) *out_z = z;
    /* A hair of tolerance so a point sitting exactly ON a plane -- which these
     * samples do, by construction -- is not read as a defect. */
    const float tol = 1e-4f;
    /* Order matters: the square is reported even when the depth also fails,
     * because an off-square caster is the unrescuable case and must not be
     * hidden behind a near-plane verdict the pancake would forgive. */
    if (fabsf(x) > 1.0f + tol || fabsf(y) > 1.0f + tol) return OFF_SQUARE;
    if (z > 1.0f + tol) return OFF_FAR;
    if (z < -tol)       return OFF_NEAR;
    return IN_BOX;
}

static int camera_sees(const jce_mat4 *cam_vp, jce_vec3 p, float *out_depth)
{
    const float v[4] = { p.x, p.y, p.z, 1.0f };
    float c[4];
    mul_m4v4(cam_vp, v, c);
    if (c[3] <= 1e-6f) return 0;
    const float x = c[0] / c[3], y = c[1] / c[3], z = c[2] / c[3];
    if (out_depth) *out_depth = c[3];
    return (fabsf(x) <= 1.0f && fabsf(y) <= 1.0f && z >= 0.0f && z <= 1.0f);
}

/* A unit vector at elevation `el`, azimuth `az`, pointing TOWARD the sun --
 * the sense jce_csm.c gives light_dir (it places the light camera at
 * centre + ld * (radius + near_extend)).  Asserted, not assumed: see
 * test_light_dir_points_toward_the_sun. */
static jce_vec3 to_sun_dir(float el_deg, float az_deg)
{
    const float e = el_deg * JCE_DEG2RAD, a = az_deg * JCE_DEG2RAD;
    return jce_v3(cosf(e) * sinf(a), sinf(e), cosf(e) * cosf(a));
}

/* Which cascade the SHADER picks for a fragment: by view depth (csm_shadow.sh). */
static uint32_t cascade_for_depth(const JceCsmData *d, float depth)
{
    if (depth < d->splits[1]) return 0;
    if (depth < d->splits[2]) return 1;
    if (depth < d->splits[3]) return 2;
    return 3;
}

static JceCsmData compute_at(float yaw_deg, jce_vec3 to_sun, float shadow_far,
                             bool with_caster_bounds)
{
    const jce_mat4 vw = view_for_yaw(yaw_deg);
    const jce_vec3 mn = jce_v3(-SCENE_HALF, SCENE_FLOOR, -SCENE_HALF);
    const jce_vec3 mx = jce_v3( SCENE_HALF, SCENE_TOP,   SCENE_HALF);
    JceCsmData out;
    jce_csm_compute(&out, 4, SHADOW_NEAR, shadow_far, FOV_DEG, ASPECT,
                    &vw, &to_sun, false, MAP_SIZE, SPLIT_LAMBDA,
                    with_caster_bounds ? &mn : NULL,
                    with_caster_bounds ? &mx : NULL);
    return out;
}

/* ── 1. The convention everything here depends on, asserted not assumed ─── */

static void test_light_dir_points_toward_the_sun(void)
{
    /* If light_dir were the direction light TRAVELS, every test below would be
     * probing the ground UNDER the cascade instead of the air above it, and
     * would pass for the wrong reason. */
    const jce_vec3 sun = to_sun_dir(35.0f, 40.0f);
    const JceCsmData d = compute_at(0.0f, sun, 150.0f, true);
    float z_center = 0.0f, z_up = 0.0f;
    TEST_ASSERT_EQUAL_INT(IN_BOX, classify(&d.vp[0], d.center[0], &z_center));
    (void)classify(&d.vp[0], jce_v3(d.center[0].x + sun.x,
                                    d.center[0].y + sun.y,
                                    d.center[0].z + sun.z), &z_up);
    TEST_ASSERT_TRUE_MESSAGE(z_up < z_center,
        "light_dir is not the to-sun direction: moving along it did not move "
        "the point toward the light's near plane");
}

/* ── 2. Pure rotation must not resize a cascade ─────────────────────────── */

static void test_rotation_leaves_cascade_radius_bit_identical(void)
{
    /* The fit's own stability claim: radius depends on fov/aspect/splits only,
     * and frustum-corner generation is a rigid transform, so a rotation about
     * the eye cannot change it.  If this fails, the texel snap below it is
     * snapping to a grid whose spacing moves and no bias will settle it. */
    const jce_vec3 sun = to_sun_dir(35.0f, 40.0f);
    const JceCsmData a = compute_at(0.0f, sun, 150.0f, true);
    for (int i = 1; i <= 8; i++) {
        const JceCsmData b = compute_at((float)i * 40.0f, sun, 150.0f, true);
        for (uint32_t c = 0; c < a.cascade_count; c++)
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&a.radius[c], &b.radius[c],
                sizeof(float),
                "cascade radius changed under a pure camera rotation");
    }
}

/* ── 3. The contract the pancake depends on ─────────────────────────────── */

/* Walk from a visible ground point toward the sun and classify the caster that
 * would stand there.  Counts each rejection class separately, because they are
 * not interchangeable: OFF_NEAR is clamped onto the plane by the shadow vertex
 * shader, OFF_FAR and OFF_SQUARE are simply gone. */
typedef struct { long near_c, far_c, square_c, checked; float min_near_h; } Tally;

static void sweep(float el_deg, float shadow_far, Tally *t)
{
    const float az[4] = { 0.0f, 90.0f, 200.0f, 305.0f };
    const jce_mat4 pj = jce_m4_perspective(FOV_DEG * JCE_DEG2RAD, ASPECT,
                                           CAM_NEAR, CAM_FAR, false);
    for (int ai = 0; ai < 4; ai++) {
        const jce_vec3 sun = to_sun_dir(el_deg, az[ai]);
        for (int yi = 0; yi < 24; yi++) {
            const float yaw = (float)yi * 15.0f;
            const jce_mat4 vw = view_for_yaw(yaw);
            const jce_mat4 cam_vp = jce_m4_multiply(&pj, &vw);
            const JceCsmData d = compute_at(yaw, sun, shadow_far, true);
            const float step = shadow_far / 40.0f;
            for (float gx = -shadow_far; gx <= shadow_far; gx += step) {
                for (float gz = -shadow_far; gz <= shadow_far; gz += step) {
                    const jce_vec3 r = jce_v3(gx, 0.0f, gz);
                    float depth = 0.0f;
                    if (!camera_sees(&cam_vp, r, &depth)) continue;
                    if (depth > shadow_far) continue;
                    const uint32_t c = cascade_for_depth(&d, depth);
                    /* A fragment its own cascade does not cover is handed to a
                     * wider one by the shader's fallthrough ladder -- not this
                     * test's subject. */
                    if (classify(&d.vp[c], r, NULL) != IN_BOX) continue;
                    for (int h = 1; h <= 12; h++) {
                        const float y_top = (SCENE_TOP / 12.0f) * (float)h;
                        const float tt = (y_top - r.y) / sun.y;
                        const jce_vec3 p = jce_v3(r.x + sun.x * tt,
                                                  r.y + sun.y * tt,
                                                  r.z + sun.z * tt);
                        t->checked++;
                        switch (classify(&d.vp[c], p, NULL)) {
                        case OFF_NEAR:
                            t->near_c++;
                            if (y_top < t->min_near_h) t->min_near_h = y_top;
                            break;
                        case OFF_FAR:    t->far_c++;    break;
                        case OFF_SQUARE: t->square_c++; break;
                        default: break;
                        }
                    }
                }
            }
        }
    }
}

static void run_case(float el, float far_, const char *tag)
{
    Tally t;
    memset(&t, 0, sizeof t);
    t.min_near_h = 1e9f;
    sweep(el, far_, &t);
    printf("  %-16s checked %7ld | near %6ld | far %ld | square %ld",
           tag, t.checked, t.near_c, t.far_c, t.square_c);
    if (t.near_c) printf(" | shortest pancaked caster %.1f m", (double)t.min_near_h);
    printf("\n");

    TEST_ASSERT_TRUE_MESSAGE(t.checked > 0,
        "the sweep examined nothing -- the camera sees no ground, so this "
        "case proves nothing about the cascade fit");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)t.square_c,
        "a caster over visible ground fell outside its cascade's light-space "
        "SQUARE: the shadow-map clamp in shadow_pancake.sh touches only z, so "
        "nothing puts this one back");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)t.far_c,
        "a caster over visible ground fell beyond its cascade's FAR plane: "
        "the shadow-map clamp in shadow_pancake.sh touches only the near side, "
        "so nothing puts this one back");
}

static void test_only_the_near_plane_ever_rejects_a_caster(void)
{
    /* Four configurations, chosen from the measured table at the top of this
     * file: one where the old fit had no hole at all, and three where it had
     * the biggest.  The `near` column is the load the pancake carries; it is
     * printed, not asserted, because its value is a property of the scene and
     * the sun, not a contract.  The two asserted columns are the contract. */
    run_case(35.0f, 150.0f, "sun 35 far 150");
    run_case(15.0f, 150.0f, "sun 15 far 150");
    run_case(25.0f,  40.0f, "sun 25 far 40");
    run_case(12.0f,  40.0f, "sun 12 far 40");
}

/* ── 4. Controls ────────────────────────────────────────────────────────── */

static void test_the_near_column_is_not_always_zero(void)
{
    /* Without this, the test above could pass on a fit that never pushes the
     * near plane at all AND on one that pushes it infinitely far -- the two
     * asserted columns would read zero either way.  The `near` column is what
     * distinguishes them, so at least one configuration must populate it, or
     * the file is measuring a constant. */
    Tally t;
    memset(&t, 0, sizeof t);
    t.min_near_h = 1e9f;
    sweep(12.0f, 40.0f, &t);
    TEST_ASSERT_TRUE_MESSAGE(t.near_c > 0,
        "no caster anywhere was near-rejected, so shadow_pancake.sh has "
        "nothing to do and this file cannot tell a working fit from a broken "
        "one");
}

static void test_without_caster_bounds_the_near_plane_sits_on_the_sphere(void)
{
    /* With caster bounds withheld, jce_csm.c documents the near plane as
     * anchored to the cascade sphere, so a caster above it must be rejected --
     * and rejected at the NEAR plane specifically, which is the class this
     * file's contract says is the only recoverable one. */
    const jce_vec3 sun = to_sun_dir(35.0f, 40.0f);
    const JceCsmData d = compute_at(0.0f, sun, 150.0f, false);
    const float t = (SCENE_TOP - d.center[0].y) / sun.y;
    const jce_vec3 p = jce_v3(d.center[0].x + sun.x * t,
                              d.center[0].y + sun.y * t,
                              d.center[0].z + sun.z * t);
    TEST_ASSERT_EQUAL_INT_MESSAGE(OFF_NEAR, classify(&d.vp[0], p, NULL),
        "with no caster bounds the near plane should sit on the cascade "
        "sphere, so a caster above it must be near-rejected -- it was not, so "
        "classify() cannot report that class and the sweep above is blind");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_light_dir_points_toward_the_sun);
    RUN_TEST(test_rotation_leaves_cascade_radius_bit_identical);
    RUN_TEST(test_only_the_near_plane_ever_rejects_a_caster);
    RUN_TEST(test_the_near_column_is_not_always_zero);
    RUN_TEST(test_without_caster_bounds_the_near_plane_sits_on_the_sphere);
    return UNITY_END();
}
