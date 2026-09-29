/*
 * test_jce_project_script_modules.c — jce_project.json's "script_modules"
 * survives a load/save round trip.
 *
 * WHAT THIS IS ABOUT.  A C++ script is a class in a shared object the PROJECT
 * builds, and the only place a tool can learn which shared object that is, is
 * this manifest key.  The editor reads it at project-open and loads the module
 * (editor/src/core/jce_editor_script_backends.cpp); with no module loaded,
 * every .jcecpp scriptPath in the scene is refused.
 *
 * THE FAILURE THIS EXISTS FOR IS SILENT AND DESTRUCTIVE.  jce_project_save()
 * REBUILDS the manifest from the in-memory struct rather than patching the
 * file, so a key that is parsed but not written back disappears the first time
 * anything else is saved — and the editor saves this manifest for ordinary
 * reasons (jce_editor_project_update_field, the project dialog's fields).  The
 * symptom would arrive later and somewhere else: a project whose C++ scripts
 * stopped loading, with no edit to any script and nothing in the log about it.
 *
 * So the round trip is asserted in both directions, and the second case is the
 * one that matters: saving after touching an UNRELATED field must not lose it.
 */

#include "unity.h"

#include "jce_test_file_util.h"

#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>

#include <stdio.h>
#include <string.h>

#define TEST_DIR "test_project_script_modules"

static const char *k_manifest =
    "{\n"
    "  \"schema\": 3,\n"
    "  \"name\": \"scripted\",\n"
    "  \"target\": \"Scripted\",\n"
    "  \"source_assets\": \"assets\",\n"
    "  \"startup_scene\": \"scenes/main.scene.json\",\n"
    "  \"script_modules\": [\n"
    "    \"build/release/game_scripts.dll\",\n"
    "    \"D:/prebuilt/vendor_scripts.dll\"\n"
    "  ]\n"
    "}\n";

void setUp(void)
{
    jce_fs_host_create_directory(TEST_DIR);
    jce_test_write_file(TEST_DIR "/jce_project.json", k_manifest);
}

void tearDown(void)
{
    jce_fs_host_remove_recursive(TEST_DIR);
}

static void test_script_modules_are_parsed_in_order(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "the manifest did not load at all");

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, p->script_modules_count,
        "\"script_modules\" was not parsed -- the editor would load no C++ "
        "script module and every .jcecpp scriptPath would be refused in Play");
    /* ORDER IS PART OF THE CONTRACT: modules are loaded in the order given, and
     * a module registers its classes when it loads, so a project that relies on
     * one module's classes existing before another's needs this to be stable. */
    TEST_ASSERT_EQUAL_STRING("build/release/game_scripts.dll",
                             p->script_modules[0]);
    /* Relative and absolute entries both survive verbatim; resolving them is
     * the consumer's job, not the parser's. */
    TEST_ASSERT_EQUAL_STRING("D:/prebuilt/vendor_scripts.dll",
                             p->script_modules[1]);
    jce_project_free(p);
}

/* THE DATA-LOSS CASE.  Nothing in the editor edits this key; it edits OTHER
 * fields and saves the whole manifest.  If save() did not write script_modules
 * back, renaming the startup scene would silently delete the project's C++
 * scripting. */
static void test_saving_after_an_unrelated_edit_keeps_script_modules(void)
{
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(2, p->script_modules_count);

    TEST_ASSERT_TRUE(jce_project_set_field(p, "startup_scene",
                                           "scenes/other.scene.json"));
    TEST_ASSERT_TRUE_MESSAGE(jce_project_save(p), "save failed");
    jce_project_free(p);

    JceProject *q = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_STRING("scenes/other.scene.json", q->startup_scene);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, q->script_modules_count,
        "saving the manifest after editing an unrelated field DROPPED "
        "\"script_modules\" -- the project's C++ scripts stop loading and "
        "nothing says why");
    TEST_ASSERT_EQUAL_STRING("build/release/game_scripts.dll",
                             q->script_modules[0]);
    TEST_ASSERT_EQUAL_STRING("D:/prebuilt/vendor_scripts.dll",
                             q->script_modules[1]);
    jce_project_free(q);
}

/* A manifest with no such key is the NORMAL state — most projects have no C++
 * scripts — and must load as "nothing to do", not as an error and not as a
 * default path that would then be reported missing. */
static void test_a_manifest_without_the_key_declares_no_modules(void)
{
    jce_test_write_file(TEST_DIR "/jce_project.json",
                        "{\n  \"schema\": 2,\n  \"name\": \"plain\"\n}\n");
    JceProject *p = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, "a manifest without script_modules failed "
                                    "to load -- the key became mandatory");
    TEST_ASSERT_EQUAL_INT(0, p->script_modules_count);
    TEST_ASSERT_NULL(p->script_modules);

    /* And saving one must not INVENT the key either: a project that never had
     * C++ scripts should round-trip without gaining a declaration. */
    TEST_ASSERT_TRUE(jce_project_save(p));
    jce_project_free(p);

    JceProject *q = jce_project_load(TEST_DIR);
    TEST_ASSERT_NOT_NULL(q);
    TEST_ASSERT_EQUAL_INT(0, q->script_modules_count);
    jce_project_free(q);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_script_modules_are_parsed_in_order);
    RUN_TEST(test_saving_after_an_unrelated_edit_keeps_script_modules);
    RUN_TEST(test_a_manifest_without_the_key_declares_no_modules);
    return UNITY_END();
}
