/* A project NAME is human text; a CMake target and a C function name are
 * IDENTIFIERS.  jce_project_create_from_template used one token for both.
 *
 * REACHED FROM THE EDITOR, measured: the New Project dialog's only validation
 * is `strlen(project_name) > 0` (jce_dialog_project.cpp:583), so "My Game" is
 * accepted and the template is written with the raw name substituted into
 * `project(... C)`, `add_executable(...)` and `static JceAppDesc ..._get_desc`.
 *
 * What that produced, measured with cmake 3.27.9 on the generated file:
 *
 *     CMake Error: Could not find cmake module file:
 *                  CMakeDetermineGameCompiler.cmake
 *     No CMAKE_Game_COMPILER could be found.
 *
 * -- an error naming a LANGUAGE that does not exist, for a bug about a space.
 * A hyphen is quieter and worse: `my-game_get_desc` is a valid C expression
 * (a subtraction), so the failure moves from configure time to a confusing
 * compile error about undeclared identifiers.
 *
 * The Automation layer's project.create had derived a sanitised target for
 * years and carried a comment saying it learned it from a failure.  Two
 * scaffolders, one of which knew.  These tests pin the rule in the engine, so
 * both paths get it.
 */

#include <jce/application/jce_project.h>
#include <jce/os/core/jce_defs.h>        /* JCE_PLATFORM_WINDOWS */
#include <jce/os/core/jce_filesystem.h>

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AN UNDEFINED MACRO IS 0 IN #if, SILENTLY -- and this file would then take
 * the POSIX branch on Windows and fail to build rather than fail quietly,
 * but the #error says which header went missing instead. */
#if !defined(JCE_PLATFORM_WINDOWS)
#error "JCE_PLATFORM_WINDOWS is not defined; include <jce/os/core/jce_defs.h>"
#endif

/* POINT THE CREATOR AT THE SOURCE TEMPLATES, FROM INSIDE THE PROGRAM.
 *
 * Not a ctest ENVIRONMENT property, and check_test_window_discipline.py says
 * why in its own docstring: "A ctest ENVIRONMENT property is a property of the
 * LAUNCH, not of the program: it does not reach the exe when someone runs it
 * directly to debug it."  That is exactly when this matters -- without it the
 * creator falls back to dist/sdk's INSTALLED copy, so a direct run would
 * measure whatever was last installed rather than what is in the tree.  That
 * is the stale-binary trap with a different subject.
 *
 * JCE_TEST_TEMPLATES_DIR comes from the CMakeLists beside this file. */
static void point_at_source_templates(void)
{
#ifdef JCE_TEST_TEMPLATES_DIR
#if JCE_PLATFORM_WINDOWS
    _putenv_s("JCE_TEMPLATES_DIR", JCE_TEST_TEMPLATES_DIR);
#else
    setenv("JCE_TEMPLATES_DIR", JCE_TEST_TEMPLATES_DIR, 1);
#endif
#endif
}

void setUp(void) { point_at_source_templates(); }
void tearDown(void) {}

/* A scratch directory that does not already exist.  Named per test so a
 * failure leaves its own tree behind to look at rather than the last one's. */
static void scratch_dir(char *out, size_t cap, const char *leaf)
{
    snprintf(out, cap, "build/automation/tmp-project-%s", leaf);
    jce_fs_host_remove_recursive(out);
}

static char *read_all_text(const char *path)
{
    uint64_t sz = 0;
    void *buf = jce_fs_host_read_all(path, &sz);
    if (!buf)
        return NULL;
    char *s = (char *)malloc((size_t)sz + 1);
    if (!s) {
        jce_fs_buffer_free(buf);
        return NULL;
    }
    memcpy(s, buf, (size_t)sz);
    s[sz] = '\0';
    jce_fs_buffer_free(buf);
    return s;
}

/* Create a project and hand back the generated CMakeLists / main.c /
 * manifest, or skip the test when the SDK templates are not on this machine.
 *
 * SKIPPED, NOT PASSED, when the templates are missing: a test that silently
 * succeeds because its subject could not be reached is the shape this tree
 * has paid for repeatedly.
 */
static bool make_project(const char *leaf, const char *name, char *dir,
                         size_t dir_cap, char **cmake, char **mainc,
                         char **manifest)
{
    char err[512] = {0};
    scratch_dir(dir, dir_cap, leaf);
    if (!jce_project_create_from_template(dir, name,
                                          JCE_PROJECT_TEMPLATE_EMPTY,
                                          err, sizeof err)) {
        if (strstr(err, "templates not found") || strstr(err, "template missing")) {
            TEST_IGNORE_MESSAGE("SDK templates are not installed on this "
                                "machine; run `python scripts/jce.py sdk`");
            return false;
        }
        TEST_FAIL_MESSAGE(err);
        return false;
    }
    char p[1024];
    snprintf(p, sizeof p, "%s/CMakeLists.txt", dir);
    *cmake = read_all_text(p);
    snprintf(p, sizeof p, "%s/src/main.c", dir);
    *mainc = read_all_text(p);
    snprintf(p, sizeof p, "%s/jce_project.json", dir);
    *manifest = read_all_text(p);
    TEST_ASSERT_NOT_NULL_MESSAGE(*cmake, "template wrote no CMakeLists.txt");
    TEST_ASSERT_NOT_NULL_MESSAGE(*mainc, "template wrote no src/main.c");
    TEST_ASSERT_NOT_NULL_MESSAGE(*manifest, "template wrote no jce_project.json");
    return true;
}

static void free_all(char *a, char *b, char *c) { free(a); free(b); free(c); }

/* The value of a top-level string field, without assuming the separator.
 *
 * WRITTEN AFTER GUESSING WRONG.  The first version asserted the substring
 * `"target": "MyGame"` -- how JSON usually looks -- and failed against a
 * manifest that is real and correct: jce_project_save() writes
 * `"target":<TAB>"MyGame"`.  The fix was working; the expectation came from
 * habit rather than from the product.  Skipping whitespace means this test
 * pins the VALUE, which is what it is about, and stops caring how the writer
 * spaces things, which it is not. */
static bool json_string_field(const char *text, const char *key,
                              char *out, size_t cap)
{
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t w = 0;
    while (*p && *p != '"' && w + 1 < cap)
        out[w++] = *p++;
    out[w] = '\0';
    return *p == '"';
}

/* THE POSITIVE CONTROL FOR THIS WHOLE FILE.  An ordinary name must still
 * arrive intact -- otherwise "no space in the target" could be satisfied by a
 * sanitiser that mangles everything, and every other assertion here would
 * pass while the feature was broken. */
static void test_an_ordinary_name_is_unchanged(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("plain", "Asteroids", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    TEST_ASSERT_NOT_NULL(strstr(cmake, "project(Asteroids C)"));
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(Asteroids src/main.c)"));
    TEST_ASSERT_NOT_NULL(strstr(mainc, "JCE_MAIN(Asteroids_get_desc)"));
    TEST_ASSERT_NOT_NULL(strstr(manifest, "\"target\""));
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

static void test_a_space_never_reaches_an_identifier(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("space", "My Game", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;

    /* The exact text cmake choked on. */
    TEST_ASSERT_NULL_MESSAGE(strstr(cmake, "project(My Game"),
        "the raw name reached project(), which makes cmake look for a "
        "compiler for a language called Game");
    TEST_ASSERT_NULL_MESSAGE(strstr(cmake, "add_executable(My Game"),
        "the raw name reached add_executable()");
    TEST_ASSERT_NOT_NULL(strstr(cmake, "project(MyGame C)"));
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(MyGame src/main.c)"));

    /* main.c: the C identifier must be one token. */
    TEST_ASSERT_NULL_MESSAGE(strstr(mainc, "My Game_get_desc"),
        "the raw name reached a C function name");
    TEST_ASSERT_NOT_NULL(strstr(mainc, "JCE_MAIN(MyGame_get_desc)"));

    /* And the HUMAN name must survive where it belongs -- the manifest and
     * the window title.  A sanitiser that also flattened the display name
     * would pass every assertion above and still be wrong. */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mainc, "d.name          = \"My Game\";"),
        "the display name should keep its space");
    char field[128];
    TEST_ASSERT_TRUE_MESSAGE(json_string_field(manifest, "name", field,
                                               sizeof field),
        "the manifest has no name field");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("My Game", field,
        "the manifest should record the human name, space and all");

    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

/* AN UNDERSCORE IS KEPT, and this case exists because the two scaffolders
 * disagreed about it.
 *
 * `_` is legal in a CMake target AND in a C identifier, so stripping it
 * would silently mangle what the author typed for no gain.  When the engine
 * rule landed, the Automation layer's project.create still stripped it --
 * the same name gave `My_Game` here and `MyGame` there.  Neither produced a
 * broken project, which is exactly why it would have survived: drift that
 * breaks nothing is the kind nobody finds.
 *
 * This name and its expected target also appear in the Python half,
 * test_project_target_matches_the_engine_rule, which asserts that every name
 * in its corpus appears HERE -- so the two cannot silently stop pinning the
 * same rule. */
static void test_an_underscore_is_kept(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("underscore", "My_Game", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(My_Game src/main.c)"));
    TEST_ASSERT_NOT_NULL(strstr(mainc, "JCE_MAIN(My_Game_get_desc)"));
    TEST_ASSERT_NULL_MESSAGE(strstr(mainc, "JCE_MAIN(MyGame_get_desc)"),
        "the underscore should be kept, not stripped");
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

/* A hyphen is the QUIET case: `my-game_get_desc` parses as a subtraction, so
 * the old behaviour failed at compile time with a message about undeclared
 * identifiers rather than about the project name. */
static void test_a_hyphen_never_reaches_an_identifier(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("hyphen", "my-game", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    TEST_ASSERT_NULL(strstr(mainc, "my-game_get_desc"));
    TEST_ASSERT_NOT_NULL(strstr(mainc, "JCE_MAIN(mygame_get_desc)"));
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(mygame src/main.c)"));
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

/* Neither a CMake target nor a C identifier may begin with a digit. */
static void test_a_leading_digit_is_made_legal(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("digit", "2048", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    TEST_ASSERT_NULL(strstr(mainc, "JCE_MAIN(2048_get_desc)"));
    TEST_ASSERT_NOT_NULL(strstr(mainc, "JCE_MAIN(_2048_get_desc)"));
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

/* A name with nothing identifier-legal in it must still produce a buildable
 * project rather than an empty target name. */
static void test_a_name_of_pure_punctuation_still_builds_a_target(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("punct", "!!!", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    TEST_ASSERT_NULL_MESSAGE(strstr(cmake, "add_executable( src/main.c)"),
        "an empty target name would make add_executable take the source as "
        "the target");
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(JceGame src/main.c)"));
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

/* THE MANIFEST MUST AGREE WITH THE FILE NEXT TO IT.  The build pipeline reads
 * `target` from jce_project.json and asks CMake to build it; if CMakeLists
 * names a different one, the build hunts for an executable that was never
 * produced -- which is a linker or a "no such target" error, not a message
 * about the project name. */
static void test_the_manifest_target_matches_the_cmake_target(void)
{
    char dir[512], *cmake, *mainc, *manifest;
    if (!make_project("agree", "My Game", dir, sizeof dir,
                      &cmake, &mainc, &manifest))
        return;
    char target[128], exe[128];
    TEST_ASSERT_TRUE_MESSAGE(json_string_field(manifest, "target", target,
                                               sizeof target),
        "the manifest has no target field");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("MyGame", target,
        "the manifest's target must be the one CMakeLists declares");
    TEST_ASSERT_NOT_NULL(strstr(cmake, "add_executable(MyGame src/main.c)"));
    /* `exe` too: the build looks for the produced executable by this name,
     * and jce_project_new leaves it empty -- so a fix that set only `target`
     * would still send the pipeline after a file that is not there. */
    TEST_ASSERT_TRUE_MESSAGE(json_string_field(manifest, "exe", exe,
                                               sizeof exe),
        "the manifest has no exe field");
    TEST_ASSERT_EQUAL_STRING("MyGame", exe);
    free_all(cmake, mainc, manifest);
    jce_fs_host_remove_recursive(dir);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_an_ordinary_name_is_unchanged);
    RUN_TEST(test_a_space_never_reaches_an_identifier);
    RUN_TEST(test_an_underscore_is_kept);
    RUN_TEST(test_a_hyphen_never_reaches_an_identifier);
    RUN_TEST(test_a_leading_digit_is_made_legal);
    RUN_TEST(test_a_name_of_pure_punctuation_still_builds_a_target);
    RUN_TEST(test_the_manifest_target_matches_the_cmake_target);
    return UNITY_END();
}
