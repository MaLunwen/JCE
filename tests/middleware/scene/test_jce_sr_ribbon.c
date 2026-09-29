/*
 * test_jce_sr_ribbon.c — the two deciding halves of the LineRenderer /
 * TrailRenderer material wire, asserted with NO editor callback installed.
 *
 * WHY THAT QUALIFIER IS THE POINT.  sr_resolve_texture() has two halves: in
 * the editor it forwards to the asset cache, which parses .mat.json; in a
 * shipping exe it hands the path to jce_texture_decode_cpu(), whose extension
 * whitelist has no .json in it.  So the naive wire -- material_path straight
 * into the resolver -- is textured in the editor and untextured in the exe for
 * every authorable value.  These tests run in a bare process: jce_fs_get_active()
 * is NULL, there is no JceSceneRenderer and no callback, which is the closest
 * a unit test gets to the shipping configuration.  If sr_material_albedo_path
 * can turn a material into an IMAGE path here, the resolver's runtime half can
 * load it there.
 *
 * The arc-length assertion is the one that distinguishes the new
 * parameterisation from the old: three points at x = 0, 1, 4 are 0, 0.25, 1.0
 * by length and 0, 0.5, 1.0 by index.  A test that only checked the endpoints
 * would pass on both and prove nothing.
 *
 * Linked against jce_scene so it executes the SHIPPED functions -- a test that
 * compiled its own copy would stop noticing when the shipped one changed.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include "jce_sr_ribbon.h"

#include <jce/os/core/jce_filesystem.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static char s_root[512];

/* Fixture setup runs BEFORE UNITY_BEGIN, where the TEST_ASSERT_* macros have
 * no test frame to abort into.  A failed fixture is reported as a hard exit
 * instead: a suite that asserts about files it never wrote would report the
 * resolver as broken when the disk was. */
static bool write_file(const char *rel, const char *body)
{
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", s_root, rel);
    if (jce_fs_host_write_all(path, body, strlen(body))) return true;
    fprintf(stderr, "fixture write failed: %s\n", path);
    return false;
}

static void mkdir_rel(const char *rel)
{
    char path[768];
    snprintf(path, sizeof(path), "%s/%s", s_root, rel);
    (void)jce_fs_host_create_directory(path);
}

/* ── material -> image ────────────────────────────────────────────── */

static void test_material_resolves_to_its_sibling_texture(void)
{
    char out[256];
    char mat[768];
    snprintf(mat, sizeof(mat), "%s/Materials/ribbon.mat.json", s_root);

    TEST_ASSERT_TRUE_MESSAGE(sr_material_albedo_path(mat, out, sizeof(out)),
        "a material naming albedoMap must resolve");

    char want[768];
    snprintf(want, sizeof(want), "%s/Materials/spark.png", s_root);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out,
        "the reference is relative to the material file, so it must be "
        "anchored against the material's own directory");
}

static void test_material_resolves_a_sibling_folder_reference(void)
{
    /* Unity exports name textures from the project root, not from the
     * material.  Both anchors have to be tried or half the authorable
     * materials silently resolve to nothing. */
    char out[256];
    char mat[768];
    snprintf(mat, sizeof(mat), "%s/Materials/up.mat.json", s_root);

    TEST_ASSERT_TRUE(sr_material_albedo_path(mat, out, sizeof(out)));

    char want[768];
    snprintf(want, sizeof(want), "%s/Textures/glow.png", s_root);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, out,
        "the parent walk must find Textures/ beside Materials/");
}

static void test_the_answer_is_never_a_json_path(void)
{
    /* The whole reason this function exists.  jce_texture_decode_cpu()
     * rejects any extension outside its image whitelist, so a resolver that
     * hands back the .mat.json it was given produces a ribbon that is
     * textured in the editor and blank in the exe. */
    char out[256];
    char mat[768];
    snprintf(mat, sizeof(mat), "%s/Materials/ribbon.mat.json", s_root);
    TEST_ASSERT_TRUE(sr_material_albedo_path(mat, out, sizeof(out)));

    const size_t n = strlen(out);
    TEST_ASSERT_TRUE_MESSAGE(n < 5 || strcmp(out + n - 5, ".json") != 0,
        "resolved to a .json path -- the shipping texture loader cannot open "
        "that, and the editor's asset cache can, which is exactly the split "
        "this wire had to avoid");
}

static void test_an_image_path_passes_straight_through(void)
{
    /* The Inspector picker accepts an image as well as a material; running
     * one through a material parse would only fail. */
    char out[256];
    TEST_ASSERT_TRUE(sr_material_albedo_path("Textures/direct.png",
                                             out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Textures/direct.png", out);
}

static void test_a_material_with_no_base_colour_resolves_to_nothing(void)
{
    char out[256];
    char mat[768];
    snprintf(mat, sizeof(mat), "%s/Materials/bare.mat.json", s_root);
    TEST_ASSERT_FALSE_MESSAGE(sr_material_albedo_path(mat, out, sizeof(out)),
        "no albedoMap means no texture -- the ribbon must fall back to the "
        "colour program, not to an empty sampler that reads black");
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_a_missing_material_resolves_to_nothing(void)
{
    char out[256];
    char mat[768];
    snprintf(mat, sizeof(mat), "%s/Materials/absent.mat.json", s_root);
    TEST_ASSERT_FALSE(sr_material_albedo_path(mat, out, sizeof(out)));
}

static void test_empty_and_null_are_refused(void)
{
    char out[256] = "poison";
    TEST_ASSERT_FALSE(sr_material_albedo_path(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(sr_material_albedo_path("", out, sizeof(out)));
    TEST_ASSERT_FALSE(sr_material_albedo_path("x.mat.json", NULL, 16));
    TEST_ASSERT_FALSE(sr_material_albedo_path("x.mat.json", out, 0));
}

/* ── arc-length parameter ─────────────────────────────────────────── */

static void test_u_is_arc_length_not_index(void)
{
    /* THE discriminating case: unequal segments.  0 -> 1 -> 4, so the two
     * segments are 1 and 3 long out of a total of 4.
     *   by length: 0, 0.25, 1.0
     *   by index:  0, 0.50, 1.0
     * Both agree at the endpoints, so only the middle sample separates them.
     * (The first version of this fixture used 0 -> 1 -> 3 and asserted 0.25,
     * which is 1/3 by length -- the assertion was wrong before the code was,
     * and it failed on a correct implementation.) */
    const float pts[3][3] = { {0,0,0}, {1,0,0}, {4,0,0} };
    float u[3] = { -1, -1, -1 };
    TEST_ASSERT_EQUAL_INT(3, sr_ribbon_arc_u(pts, 3, false, u, 3));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.00f, u[0]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.25f, u[1],
        "0.5 here means the index parameterisation is back, and with it a "
        "texture that compresses on short segments and stretches on long ones");
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.00f, u[2]);
}

static void test_a_loop_includes_its_closing_segment(void)
{
    /* Unit square.  Total length 4 with the closing edge, 3 without -- so a
     * loop that forgot to close would put u[3] at 1.0 instead of 0.75 and the
     * texture would wrap early, one seam short of the joint. */
    const float sq[4][3] = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} };
    float u[4];
    TEST_ASSERT_EQUAL_INT(4, sr_ribbon_arc_u(sq, 4, true, u, 4));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.00f, u[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.25f, u[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.50f, u[2]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, 0.75f, u[3],
        "1.0 would mean the closing edge was left out of the total");
}

static void test_the_open_case_ends_at_one(void)
{
    const float sq[4][3] = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0} };
    float u[4];
    TEST_ASSERT_EQUAL_INT(4, sr_ribbon_arc_u(sq, 4, false, u, 4));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, u[3]);
}

static void test_a_degenerate_polyline_is_zero_not_nan(void)
{
    /* A trail whose points have not moved yet.  Dividing by a zero total
     * would put NaN in a vertex buffer, and a NaN position removes the whole
     * primitive silently -- the ribbon would vanish rather than look wrong. */
    const float same[3][3] = { {2,2,2}, {2,2,2}, {2,2,2} };
    float u[3] = { 9, 9, 9 };
    TEST_ASSERT_EQUAL_INT(3, sr_ribbon_arc_u(same, 3, false, u, 3));
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(0.0f, 0.0f, u[i]);
        TEST_ASSERT_TRUE_MESSAGE(u[i] == u[i], "NaN in the u parameter");
    }
}

static void test_arc_u_refuses_bad_input(void)
{
    const float pts[2][3] = { {0,0,0}, {1,0,0} };
    float u[2];
    TEST_ASSERT_EQUAL_INT(0, sr_ribbon_arc_u(NULL, 2, false, u, 2));
    TEST_ASSERT_EQUAL_INT(0, sr_ribbon_arc_u(pts, 2, false, NULL, 2));
    TEST_ASSERT_EQUAL_INT(0, sr_ribbon_arc_u(pts, 0, false, u, 2));
    TEST_ASSERT_EQUAL_INT(0, sr_ribbon_arc_u(pts, 2, false, u, 0));
    /* cap below pts writes cap entries and says so, rather than overrunning. */
    TEST_ASSERT_EQUAL_INT(1, sr_ribbon_arc_u(pts, 2, false, u, 1));
}

/* ── the SHIPPING branch: a project-relative path through a mounted VFS ── */

static void test_a_relative_path_resolves_through_a_mounted_vfs(void)
{
    /* The configuration a shipped exe is in.  Paths inside a bundle are
     * project-relative vpaths, the runtime mounts a JceFileSystem over them,
     * and jce_fs_get_active() returns it -- so "Materials/ribbon.mat.json"
     * with no directory prefix must resolve.  The host branch asserted above
     * cannot answer this: a relative host path is relative to the process CWD,
     * which is not where the project is.
     *
     * This is the assertion that says the wire ships.  Everything else in this
     * file could hold while a packaged game still drew untextured ribbons. */
    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_TRUE_MESSAGE(jce_fs_mount_dir(fs, "", s_root),
        "mount failed -- the test would then assert about an empty VFS");
    jce_fs_set_active(fs);

    char out[256];
    const bool ok = sr_material_albedo_path("Materials/ribbon.mat.json",
                                            out, sizeof(out));

    jce_fs_set_active(NULL);
    jce_fs_destroy(fs);

    TEST_ASSERT_TRUE_MESSAGE(ok,
        "a project-relative material must resolve through the mounted VFS -- "
        "if it does not, a packaged game draws every ribbon untextured while "
        "the editor shows it textured");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Materials/spark.png", out,
        "and the answer must stay a VPATH: an absolute host path would not "
        "exist inside the bundle");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    snprintf(s_root, sizeof(s_root), "%s", "jce_ribbon_fixture");
    (void)jce_fs_host_create_directory(s_root);
    mkdir_rel("Materials");
    mkdir_rel("Textures");
    if (!write_file("Materials/spark.png", "not a real png") ||
        !write_file("Textures/glow.png",   "not a real png") ||
        !write_file("Materials/ribbon.mat.json", "{\"albedoMap\":\"spark.png\"}") ||
        !write_file("Materials/up.mat.json", "{\"albedoMap\":\"Textures/glow.png\"}") ||
        !write_file("Materials/bare.mat.json", "{\"baseColor\":[1,1,1,1]}"))
        return 2;

    if (jce_fs_get_active() != NULL) {
        fprintf(stderr, "a VFS is mounted; this suite asserts the SHIPPING "
                        "resolution path and would assert about something "
                        "else\n");
        return 2;
    }

    UNITY_BEGIN();
    RUN_TEST(test_material_resolves_to_its_sibling_texture);
    RUN_TEST(test_material_resolves_a_sibling_folder_reference);
    RUN_TEST(test_the_answer_is_never_a_json_path);
    RUN_TEST(test_an_image_path_passes_straight_through);
    RUN_TEST(test_a_material_with_no_base_colour_resolves_to_nothing);
    RUN_TEST(test_a_missing_material_resolves_to_nothing);
    RUN_TEST(test_empty_and_null_are_refused);
    RUN_TEST(test_a_relative_path_resolves_through_a_mounted_vfs);
    RUN_TEST(test_u_is_arc_length_not_index);
    RUN_TEST(test_a_loop_includes_its_closing_segment);
    RUN_TEST(test_the_open_case_ends_at_one);
    RUN_TEST(test_a_degenerate_polyline_is_zero_not_nan);
    RUN_TEST(test_arc_u_refuses_bad_input);
    return UNITY_END();
}
