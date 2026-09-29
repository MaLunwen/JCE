/* test_jce_asset_ext.c
 *
 * Characterization tests for REF-008 (dedup audit): one canonical
 * extension -> asset-kind table (jce_asset_type_from_ext).
 *
 * Before REF-008 five tables answered "is this a texture/model?" differently:
 * the cooker knew only png/jpg/jpeg/bmp/tga; the runtime whitelist added
 * dds/ktx/ktx2/hdr/webp/psd/gif; the editor asset DB added gif/webp/hdr but
 * dropped ktx2/psd and claimed dae/stl/ply/usd as models; the editor texture
 * cache omitted hdr/webp/psd/gif; the thumbnailer omitted dds/ktx/webp/gif.
 * The browser would label a file "Texture"/"Model" while the cooker packed it
 * as an opaque RAW blob, so it failed to load at runtime with no obvious cause.
 *
 * These tests pin the canonical table AND the deliberate separation between
 * "what kind of file is this?" (shared) and "can this build step encode it?"
 * (each consumer's own capability check).
 */

#include <jce/resource/jce_asset_format.h>

#include "unity.h"

/* Declared in the engine-internal engine/src/resource/jce_asset_cooker.h,
 * which is not on the test include path; forward-declared here and resolved
 * via the jce_resource link (same convention as test_jce_grass_field.c). */
int jce_cook_detect_type(const char *path);

void setUp(void)    {}
void tearDown(void) {}

/* Every extension that any of the five original tables called a texture must
 * classify as one — this is the union that ends the disagreement. */
static void test_texture_extensions(void)
{
    static const char *const tex[] = {
        "a.png", "a.jpg", "a.jpeg", "a.bmp", "a.tga",
        "a.dds", "a.ktx", "a.ktx2", "a.hdr", "a.webp", "a.psd", "a.gif",
    };
    for (size_t i = 0; i < sizeof(tex) / sizeof(tex[0]); ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCEASSET_TYPE_TEXTURE,
            jce_asset_type_from_ext(tex[i]), tex[i]);
        TEST_ASSERT_TRUE_MESSAGE(jce_asset_ext_is_texture(tex[i]), tex[i]);
    }
}

static void test_model_extensions(void)
{
    static const char *const mdl[] = {
        "m.obj", "m.fbx", "m.gltf", "m.glb",
        "m.dae", "m.stl", "m.ply", "m.usd", "m.usdc", "m.usdz",
    };
    for (size_t i = 0; i < sizeof(mdl) / sizeof(mdl[0]); ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCEASSET_TYPE_MODEL,
            jce_asset_type_from_ext(mdl[i]), mdl[i]);
}

static void test_audio_font_shader_extensions(void)
{
    static const char *const snd[] = {
        "s.wav", "s.ogg", "s.opus", "s.flac", "s.mp3", "s.m4a", "s.aac",
    };
    for (size_t i = 0; i < sizeof(snd) / sizeof(snd[0]); ++i)
        TEST_ASSERT_EQUAL_INT_MESSAGE(JCEASSET_TYPE_SOUND,
            jce_asset_type_from_ext(snd[i]), snd[i]);

    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_FONT,   jce_asset_type_from_ext("f.ttf"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_FONT,   jce_asset_type_from_ext("f.otf"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_SHADER, jce_asset_type_from_ext("v.sc"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_SHADER, jce_asset_type_from_ext("v.bin"));
}

/* Case-insensitive, separator-tolerant, and tolerant of a bare extension. */
static void test_input_shapes(void)
{
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_asset_type_from_ext("A.PNG"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_asset_type_from_ext("a.PnG"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE,
        jce_asset_type_from_ext("textures/props/wall.png"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE,
        jce_asset_type_from_ext("textures\\props\\wall.png"));
    /* Bare extension, with and without the dot (the editor passes ".png"). */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_asset_type_from_ext(".png"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_asset_type_from_ext("png"));
    /* Compound suffix resolves on the LAST component, so .mat.json is data. */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW,
        jce_asset_type_from_ext("mats/brick.mat.json"));
}

/* Unknown / degenerate inputs must fall back to RAW, never crash. */
static void test_unknown_and_degenerate(void)
{
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext(NULL));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext(""));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext("README"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext("a."));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext("a.qqq"));
    /* A dot in a DIRECTORY name must not be mistaken for an extension. */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW,
        jce_asset_type_from_ext("my.assets/README"));
    TEST_ASSERT_FALSE(jce_asset_ext_is_texture("a.obj"));
    TEST_ASSERT_FALSE(jce_asset_ext_is_texture(NULL));
}

/* The cooker deliberately encodes a NARROWER set than the shared table
 * recognises.  Recognising a format must not imply the cooker will cook it —
 * that separation is what lets the table widen without changing build output. */
static void test_cooker_capability_is_separate(void)
{
    /* Recognised engine-wide as a texture / model ... */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_asset_type_from_ext("a.webp"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_MODEL,   jce_asset_type_from_ext("m.dae"));
    /* ... but the cooker has no encoder for them, so it still emits RAW,
     * exactly as it did before REF-008. */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_cook_detect_type("a.webp"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_cook_detect_type("m.dae"));
    /* Formats the cooker DOES handle are unchanged. */
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_TEXTURE, jce_cook_detect_type("a.png"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_MODEL,   jce_cook_detect_type("m.glb"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_SOUND,   jce_cook_detect_type("s.ogg"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_FONT,    jce_cook_detect_type("f.ttf"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_SHADER,  jce_cook_detect_type("v.sc"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW,     jce_cook_detect_type("d.txt"));
    TEST_ASSERT_EQUAL_INT(-1,                    jce_cook_detect_type(NULL));
}

/* ================================================================== */
/* Script languages                                                    */
/* ================================================================== */

/* Every language the engine can SHIP a script for resolves here, in every
 * build, whether or not that language's VM backend was compiled in.  The
 * cooker runs with no backends linked and still has to pack turret.py. */
static void test_every_shipped_language_resolves(void)
{
    TEST_ASSERT_EQUAL_STRING("lua",
        jce_asset_script_language_from_ext("scripts/bob.lua"));
    TEST_ASSERT_EQUAL_STRING("python",
        jce_asset_script_language_from_ext("scripts/turret.py"));
    TEST_ASSERT_EQUAL_STRING("java",
        jce_asset_script_language_from_ext("scripts/Turret.java"));
    TEST_ASSERT_EQUAL_STRING("java",
        jce_asset_script_language_from_ext("scripts/Turret.class"));
    TEST_ASSERT_EQUAL_STRING("cpp",
        jce_asset_script_language_from_ext("scripts/Turret.jcecpp"));
    TEST_ASSERT_EQUAL_STRING("c",
        jce_asset_script_language_from_ext("scripts/Turret.jcec"));

    /* Same case-insensitivity and bare-extension shapes as the asset table:
     * the picker hands us ".PY", the archive cooker hands us "py". */
    TEST_ASSERT_EQUAL_STRING("python",
        jce_asset_script_language_from_ext("Turret.PY"));
    TEST_ASSERT_EQUAL_STRING("python", jce_asset_script_language_from_ext("py"));
    TEST_ASSERT_EQUAL_STRING("python", jce_asset_script_language_from_ext(".py"));
}

/* THE C++ ROW IS `.jcecpp`, AND THE POINT OF THIS TEST IS THE ROWS THAT ARE
 * NOT THERE.
 *
 * The cpp backend does claim an extension now, because routing happens before
 * instantiation and a Script component naming a class had to resolve to SOME
 * language.  The spelling is the whole decision: a `.cpp` row would classify
 * every translation unit in the project as an attachable script (the Script
 * picker offers exactly what this table says is a script) and would tell the
 * cooker and the publication policy to pack the project's C++ SOURCE into the
 * shipped game.  So the four build-input spellings must stay unknown, and
 * that is what breaks if someone "generalises" the row later. */
static void test_cpp_translation_units_are_not_scripts(void)
{
    TEST_ASSERT_NULL_MESSAGE(jce_asset_script_language_from_ext("src/turret.cpp"),
        "a .cpp became an attachable script -- the picker now offers every "
        "translation unit and the cooker packs project source into the game");
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("src/turret.cc"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("src/turret.h"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("src/turret.hpp"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("src/turret.c"));
    /* A bare class name has no extension at all: the catalog cannot classify
     * it, and the runtime's per-path routing is what resolves it instead. */
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("EsGpuTask"));

    /* The engine-namespaced spelling, which no toolchain produces. */
    TEST_ASSERT_EQUAL_STRING("cpp",
        jce_asset_script_language_from_ext("FlowerSway.jcecpp"));
    TEST_ASSERT_EQUAL_STRING("cpp",
        jce_asset_script_language_from_ext("scripts/FlowerSway.JCECPP"));
}

/* C IS ITS OWN LANGUAGE IN THIS TABLE, AND ".c" / ".h" MUST STAY OUT OF IT.
 *
 * The same rule as the C++ block above, and it bites harder: a ".c" row would
 * make every translation unit in the project an attachable script, and a ".h"
 * row would additionally sweep in every vendored third-party header — with
 * the cooker and the publication policy then packing all of it into the
 * shipped game.
 *
 * The LANGUAGE half is the other thing pinned here.  ".jcec" resolving to
 * "cpp" would compile, would ship, and would make the editor tell a C author
 * that their script is C++ — the name-the-wrong-thing defect that .jcecpp
 * itself was introduced to end.  Its runtime twin is the claim in
 * scripting/c/src/jce_script_vm_c.c, and
 * tools/audit/check_script_language_catalog.py fails when the two disagree. */
static void test_c_is_its_own_row_and_c_sources_are_not_scripts(void)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE("c",
        jce_asset_script_language_from_ext("scripts/Spinner.jcec"),
        "a .jcec is not catalogued as c -- the cooker labels it 'binary', the "
        "Script picker never offers it, and a packaged build may drop it");
    TEST_ASSERT_EQUAL_STRING("c",
        jce_asset_script_language_from_ext("Spinner.JCEC"));
    TEST_ASSERT_EQUAL_STRING("c", jce_asset_script_language_from_ext("jcec"));
    TEST_ASSERT_EQUAL_STRING("c", jce_asset_script_language_from_ext(".jcec"));

    /* Neither extension is a prefix of the other for this matcher: the text
     * after the LAST dot is compared whole. */
    TEST_ASSERT_EQUAL_STRING("cpp",
        jce_asset_script_language_from_ext("Spinner.jcecpp"));

    TEST_ASSERT_NULL_MESSAGE(jce_asset_script_language_from_ext("src/main.c"),
        "a .c became an attachable script -- the picker now offers every "
        "translation unit and the cooker packs project source into the game");
    TEST_ASSERT_NULL_MESSAGE(jce_asset_script_language_from_ext("include/x.h"),
        "a .h became an attachable script -- that is every vendored "
        "third-party header too");

    /* Same REFERENCE form as cpp, and for the same reason: nothing compiles
     * these bytes, so they must not join the shared text dictionary. */
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_REFERENCE,
                          jce_asset_script_form_from_ext("Spinner.jcec"));
    TEST_ASSERT_EQUAL_STRING("c.class-ref",
        jce_asset_script_representation_from_ext("Spinner.jcec"));
    /* A DISTINCT representation token from cpp's: the bundle manifest is what
     * a report reads to say which language a shipped script is. */
    TEST_ASSERT_EQUAL_STRING("cpp.class-ref",
        jce_asset_script_representation_from_ext("Spinner.jcecpp"));
}

/* REFERENCE is a third form and not a spelling of SOURCE, because the one
 * consumer that branches on `form` — the archive cooker's compression class,
 * engine/src/resource/jce_archive_cook.c — reads SOURCE as "these bytes are
 * text a VM will compile".  For a .jcecpp both halves are false: the code was
 * compiled before the process started and lives in a native module. */
static void test_a_reference_form_script_is_neither_source_nor_bytecode(void)
{
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_REFERENCE,
                          jce_asset_script_form_from_ext("FlowerSway.jcecpp"));
    TEST_ASSERT_EQUAL_STRING("cpp.class-ref",
        jce_asset_script_representation_from_ext("FlowerSway.jcecpp"));
    /* The three forms stay distinct values; a collapse would silently move
     * every .jcecpp into the shared text dictionary. */
    TEST_ASSERT_NOT_EQUAL(JCEASSET_SCRIPT_FORM_SOURCE,
                          JCEASSET_SCRIPT_FORM_REFERENCE);
    TEST_ASSERT_NOT_EQUAL(JCEASSET_SCRIPT_FORM_BYTECODE,
                          JCEASSET_SCRIPT_FORM_REFERENCE);
    TEST_ASSERT_NOT_EQUAL(JCEASSET_SCRIPT_FORM_NONE,
                          JCEASSET_SCRIPT_FORM_REFERENCE);
}

static void test_non_scripts_and_degenerate_inputs(void)
{
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("a.png"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("a.json"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("scripts/build.ps1"));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext(NULL));
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext(""));
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_NONE,
                          jce_asset_script_form_from_ext("a.png"));
    TEST_ASSERT_NULL(jce_asset_script_representation_from_ext("a.png"));

    /* A dot in a DIRECTORY is not an extension — the same rule
     * jce_script_vm_language_for_path() applies to the live registry. */
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("scripts.py/bob"));
}

/* `form` is the only field anything branches on: SOURCE joins the archive
 * cooker's shared TEXT dictionary, BYTECODE does not.  Getting .class wrong
 * would feed a binary blob to a text dictionary trainer. */
static void test_form_separates_source_from_bytecode(void)
{
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_SOURCE,
                          jce_asset_script_form_from_ext("bob.lua"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_SOURCE,
                          jce_asset_script_form_from_ext("turret.py"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_SOURCE,
                          jce_asset_script_form_from_ext("Turret.java"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_SCRIPT_FORM_BYTECODE,
                          jce_asset_script_form_from_ext("Turret.class"));
}

/* The bundle manifest's `representation` is per-language so a build report
 * says which VM a shipped script needs.  Lua's spelling is pinned because
 * every already-committed .jbundle.json in the repo carries it. */
static void test_representation_is_per_language(void)
{
    TEST_ASSERT_EQUAL_STRING("lua.source",
        jce_asset_script_representation_from_ext("bob.lua"));
    TEST_ASSERT_EQUAL_STRING("python.source",
        jce_asset_script_representation_from_ext("turret.py"));
    TEST_ASSERT_EQUAL_STRING("java.source",
        jce_asset_script_representation_from_ext("Turret.java"));
    TEST_ASSERT_EQUAL_STRING("java.class",
        jce_asset_script_representation_from_ext("Turret.class"));
}

/* A script is not a JCEASSET_TYPE_*: it ships as its own bytes, there is no
 * cooked container for it, and the two tables must not start answering each
 * other's question. */
static void test_scripts_stay_out_of_the_asset_type_table(void)
{
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext("bob.lua"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW, jce_asset_type_from_ext("turret.py"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_RAW,
                          jce_asset_type_from_ext("Turret.java"));
    /* ... and a shader is not a script, even though both are "code". */
    TEST_ASSERT_NULL(jce_asset_script_language_from_ext("vs_pbr.sc"));
    TEST_ASSERT_EQUAL_INT(JCEASSET_TYPE_SHADER, jce_asset_type_from_ext("vs_pbr.sc"));
}

/* THE RUNTIME'S REFUSAL MESSAGE IS BUILT ON THIS, so it is pinned here.
 *
 * rt_script_language_for() (engine/src/application/jce_rt_script.c) has to
 * tell two failures apart that used to print the same sentence:
 *
 *   (a) no backend in this ENGINE implements the language  -> write a VM
 *   (b) the backend exists but was not linked into this EXE -> link + register
 *
 * It separates them by asking this catalog after the live registry has already
 * said "nobody claims that extension".  That only works because the catalog is
 * independent of which backends were BUILT — and this test binary is the proof
 * of that independence rather than an assertion about it: it links
 * `jce_core jce_resource` and NO script VM whatsoever, so there is no Python,
 * Java or Lua backend in this process to answer.  The catalog answers anyway.
 *
 * If someone later "optimises" this table by deriving it from the registered
 * VMs, this test fails here and the runtime silently goes back to telling a
 * user with an un-linked Python to go and write a Python VM. */
static void test_the_catalog_answers_without_any_backend_linked(void)
{
    TEST_ASSERT_EQUAL_STRING_MESSAGE("python",
        jce_asset_script_language_from_ext("scripts/es_fireflies.py"),
        "the offline catalog stopped naming python in a process with no "
        "Python backend -- the runtime can no longer distinguish 'no such "
        "language' from 'backend not linked into this exe'");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("java",
        jce_asset_script_language_from_ext("scripts/EsCampfire.java"),
        "the offline catalog stopped naming java without the Java backend");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("java",
        jce_asset_script_language_from_ext("scripts/EsCampfire.class"),
        "a shipped Java game carries .class, not sources; the catalog must "
        "name java for bytecode with no backend linked");

    /* And the other side of the branch: a genuinely unknown extension must
     * stay NULL, or the runtime would report 'the X backend is missing' for a
     * language that does not exist and send the reader after a library that
     * was never written. */
    TEST_ASSERT_NULL_MESSAGE(jce_asset_script_language_from_ext("enemy.rb"),
        "an unimplemented language resolved to something");
    /* The cpp backend's own extension answers here too, and for the same
     * reason as python's: the editor's Script inspector reads this table, and
     * a working C++ script used to be flagged amber because the offline half
     * of the answer did not exist. */
    TEST_ASSERT_EQUAL_STRING_MESSAGE("cpp",
        jce_asset_script_language_from_ext("FlowerSway.jcecpp"),
        "the offline catalog stopped naming cpp -- the Script inspector goes "
        "back to flagging a working C++ script 'no language claims this "
        "extension'");

    /* .escpp is elemental_serenity's own runtime-claimed extension for the
     * cpp backend, invented before .jcecpp existed and still working.  A
     * project may claim an extension the catalog has never heard of, so this
     * must stay NULL and the runtime must word that case as two
     * possibilities rather than as a verdict. */
    TEST_ASSERT_NULL_MESSAGE(
        jce_asset_script_language_from_ext("EsFlowerSway.escpp"),
        "a project-private extension leaked into the offline catalog");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_texture_extensions);
    RUN_TEST(test_model_extensions);
    RUN_TEST(test_audio_font_shader_extensions);
    RUN_TEST(test_input_shapes);
    RUN_TEST(test_unknown_and_degenerate);
    RUN_TEST(test_cooker_capability_is_separate);
    RUN_TEST(test_every_shipped_language_resolves);
    RUN_TEST(test_cpp_translation_units_are_not_scripts);
    RUN_TEST(test_c_is_its_own_row_and_c_sources_are_not_scripts);
    RUN_TEST(test_a_reference_form_script_is_neither_source_nor_bytecode);
    RUN_TEST(test_non_scripts_and_degenerate_inputs);
    RUN_TEST(test_form_separates_source_from_bytecode);
    RUN_TEST(test_representation_is_per_language);
    RUN_TEST(test_scripts_stay_out_of_the_asset_type_table);
    RUN_TEST(test_the_catalog_answers_without_any_backend_linked);
    return UNITY_END();
}
