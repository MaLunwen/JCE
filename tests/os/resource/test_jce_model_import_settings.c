/*
 * test_jce_model_import_settings.c — the import options the editor writes must
 * change what the importer produces.
 *
 * WHAT WAS WRONG.  jce_panel_import_presets.cpp writes ten keys into
 * `<asset>.import.json` for every model an import preset touches.  The
 * importer's assimp post-process word was a CONSTANT --
 * Triangulate | GenSmoothNormals | FlipUVs | CalcTangentSpace |
 * PreTransformVertices -- so gen_normals, gen_tangents, flip_uv and
 * merge_meshes were forced ON whatever the preset said, and `scale` reached
 * nothing.  Setting "do not flip UVs" on a folder wrote the file exactly as
 * asked, and every model still imported flipped, with no error.
 *
 * THE OBSERVABLE IS THE VERTEX DATA, not the settings struct.  Reading the
 * struct back only proves a JSON parser works; the assertions below import the
 * same .obj twice with different sidecars and compare what came out.
 *
 * THE DEFAULTS ARE THE OLD CONSTANTS, and the no-sidecar case asserts it: an
 * importer change that silently re-imports every asset in every project
 * differently is not a fix, it is a migration nobody asked for.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/os/core/jce_filesystem.h>
#include <jce/resource/jce_model_importer.h>

/* Internal to the importer; a non-static symbol in the engine library. */
#include "jce_model_import_settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* One unit quad with EXPLICIT uvs and no normals, so gen_normals and flip_uv
 * both have something to do.  v=0.25 is deliberately not 0.5: a flip about
 * 0.5 maps 0.5 to itself, and a fixture that cannot tell flipped from
 * unflipped is a fixture that asserts nothing. */
static const char *k_obj =
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 1 1 0\n"
    "vt 0.0 0.25\n"
    "vt 1.0 0.25\n"
    "vt 1.0 0.75\n"
    "f 1/1 2/2 3/3\n";

static char s_dir[512];
static char s_obj[640];
static char s_side[700];

static void write_file(const char *path, const char *text)
{
    TEST_ASSERT_TRUE_MESSAGE(
        jce_fs_host_write_all(path, text, strlen(text)),
        "fixture write failed -- the assertions below would then be about a "
        "file that is not there");
}

static void set_sidecar(const char *json)
{
    if (json) {
        write_file(s_side, json);
    } else {
        (void)jce_fs_host_remove_file(s_side);
    }
}

/* Import the fixture and hand back its first vertex. */
static JceMeshVertex import_first(uint32_t *out_count)
{
    JceModelCpuMeshData d;
    memset(&d, 0, sizeof d);
    TEST_ASSERT_TRUE_MESSAGE(jce_model_importer_load_cpu_file(s_obj, &d),
        "the fixture .obj must import at all");
    TEST_ASSERT_TRUE(d.vertex_count > 0);
    JceMeshVertex v = d.vertices[0];
    if (out_count) *out_count = d.vertex_count;
    jce_model_importer_free_cpu(&d);
    return v;
}

/* ── the settings themselves ──────────────────────────────────────── */

static void test_no_sidecar_is_todays_behaviour(void)
{
    /* Every default here IS the constant the importer used to pass.  If one of
     * these flips, every model in every project without a sidecar imports
     * differently on the next build. */
    JceModelImportSettings s;
    memset(&s, 0xAB, sizeof s);
    set_sidecar(NULL);
    TEST_ASSERT_FALSE_MESSAGE(jce_model_import_settings_load(s_obj, &s),
        "no sidecar must report 'not present'");
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.scale);
    TEST_ASSERT_TRUE(s.gen_normals);
    TEST_ASSERT_TRUE(s.gen_tangents);
    TEST_ASSERT_TRUE(s.flip_uv);
    TEST_ASSERT_TRUE(s.merge_meshes);
    TEST_ASSERT_FALSE(s.present);
}

static void test_a_broken_sidecar_falls_back_to_defaults(void)
{
    /* Not to all-false.  An import that cannot read its options must behave
     * like one that has none -- reading "{" as "the user turned everything
     * off" would silently strip normals from a model. */
    JceModelImportSettings s;
    set_sidecar("{ this is not json");
    TEST_ASSERT_FALSE(jce_model_import_settings_load(s_obj, &s));
    TEST_ASSERT_TRUE(s.gen_normals);
    TEST_ASSERT_TRUE(s.flip_uv);
    TEST_ASSERT_FALSE(s.present);
}

static void test_a_texture_sidecar_is_refused(void)
{
    /* kind 0 is the panel's TEXTURE preset: different keys entirely, so all
     * the model booleans would read as their defaults anyway -- but `present`
     * must stay false so nothing claims the model was configured. */
    JceModelImportSettings s;
    set_sidecar("{\"kind\":0,\"gen_mips\":true}");
    TEST_ASSERT_FALSE(jce_model_import_settings_load(s_obj, &s));
    TEST_ASSERT_FALSE(s.present);
}

static void test_a_zero_scale_is_treated_as_one(void)
{
    /* A half-filled sidecar means "unset", never "collapse this mesh to a
     * point". */
    JceModelImportSettings s;
    set_sidecar("{\"kind\":1,\"scale\":0}");
    TEST_ASSERT_TRUE(jce_model_import_settings_load(s_obj, &s));
    TEST_ASSERT_FLOAT_WITHIN(0.0f, 1.0f, s.scale);
}

/* ── what actually comes out of the importer ──────────────────────── */

static void test_flip_uv_changes_the_imported_uv(void)
{
    set_sidecar("{\"kind\":1,\"flip_uv\":true}");
    const JceMeshVertex flipped = import_first(NULL);
    set_sidecar("{\"kind\":1,\"flip_uv\":false}");
    const JceMeshVertex plain = import_first(NULL);

    /* v = 0.25 in the file.  Flipped it is 0.75. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.75f, flipped.uv[1],
        "flip_uv true must mirror v about 0.5 -- this is what the importer "
        "always did, unconditionally");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.25f, plain.uv[1],
        "flip_uv FALSE must leave v alone.  Before this wire the key was "
        "written to disk and the importer flipped anyway");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, flipped.uv[0], plain.uv[0],
        "u must not move either way");
}

static void test_scale_changes_the_imported_positions(void)
{
    set_sidecar("{\"kind\":1,\"scale\":1}");
    const JceMeshVertex one = import_first(NULL);
    set_sidecar("{\"kind\":1,\"scale\":3.5}");
    const JceMeshVertex big = import_first(NULL);

    /* Vertex 0 of the fixture is the origin, so compare a nonzero component:
     * scaling (0,0,0) by anything is (0,0,0), and a fixture whose assertion
     * passes for every scale asserts nothing.  Take the whole first triangle
     * instead. */
    JceModelCpuMeshData d;
    memset(&d, 0, sizeof d);
    TEST_ASSERT_TRUE(jce_model_importer_load_cpu_file(s_obj, &d));
    float maxx = 0.0f;
    for (uint32_t i = 0; i < d.vertex_count; ++i)
        if (fabsf(d.vertices[i].pos[0]) > maxx) maxx = fabsf(d.vertices[i].pos[0]);
    jce_model_importer_free_cpu(&d);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 3.5f, maxx,
        "the quad is 1 unit wide; scale 3.5 must make it 3.5.  Before this "
        "wire `scale` reached no code at all");
    (void)one; (void)big;
}

static void test_gen_normals_off_falls_back_instead_of_generating(void)
{
    /* The fixture triangle lies in the XY plane, so a GENERATED normal points
     * along +-Z.  When assimp reports none, build_cpu_mesh_data writes its
     * fallback of (0, 1, 0) -- also a unit vector, which is why the first
     * version of this test (asserting the length drops) could never fail no
     * matter what the flag did.  Direction is the observable; length is not.
     *
     * gen_tangents is turned off with it because CalcTangentSpace needs
     * normals, and leaving it on would make assimp generate them anyway --
     * a coupling worth knowing before someone concludes the flag is dead. */
    set_sidecar("{\"kind\":1,\"gen_normals\":true}");
    const JceMeshVertex on = import_first(NULL);
    set_sidecar("{\"kind\":1,\"gen_normals\":false,\"gen_tangents\":false}");
    const JceMeshVertex off = import_first(NULL);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 1.0f, fabsf(on.normal[2]),
        "generation on must give the triangle's own normal, along Z");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-3f, 1.0f, off.normal[1],
        "generation off must leave assimp with none, so the importer's "
        "(0,1,0) fallback shows -- if this reads as a Z normal the flag "
        "reached nothing, which is the defect");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    snprintf(s_dir, sizeof s_dir, "%s", "jce_model_import_fixture");
    if (!jce_fs_host_create_directory(s_dir) && !jce_fs_host_exists_dir(s_dir)) {
        fprintf(stderr, "cannot create the fixture directory\n");
        return 2;
    }
    snprintf(s_obj, sizeof s_obj, "%s/quad.obj", s_dir);
    snprintf(s_side, sizeof s_side, "%s.import.json", s_obj);
    if (!jce_fs_host_write_all(s_obj, k_obj, strlen(k_obj))) {
        fprintf(stderr, "cannot write the fixture .obj\n");
        return 2;
    }

    UNITY_BEGIN();
    RUN_TEST(test_no_sidecar_is_todays_behaviour);
    RUN_TEST(test_a_broken_sidecar_falls_back_to_defaults);
    RUN_TEST(test_a_texture_sidecar_is_refused);
    RUN_TEST(test_a_zero_scale_is_treated_as_one);
    RUN_TEST(test_flip_uv_changes_the_imported_uv);
    RUN_TEST(test_scale_changes_the_imported_positions);
    RUN_TEST(test_gen_normals_off_falls_back_instead_of_generating);
    const int rc = UNITY_END();
    (void)jce_fs_host_remove_file(s_side);
    return rc;
}
