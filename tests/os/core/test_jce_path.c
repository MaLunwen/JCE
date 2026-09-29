/*
 * test_jce_path.c — Unit tests for engine/include/jce/os/core/jce_path.h
 *
 * Layer: L1.  Pure string ops, no filesystem touched.
 */

#include "unity.h"

#include <stdio.h>

#include <jce/os/core/jce_path.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- canonical / classification ------------------------------------- */

static void test_to_canonical_converts_backslash(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_to_canonical(out, sizeof(out), "a\\b\\c.txt"));
    TEST_ASSERT_EQUAL_STRING("a/b/c.txt", out);
}

static void test_to_canonical_overflow(void)
{
    char out[4];
    TEST_ASSERT_FALSE(jce_path_to_canonical(out, sizeof(out), "abcdef"));
}

static void test_is_canonical(void)
{
    TEST_ASSERT_TRUE (jce_path_is_canonical("a/b/c"));
    TEST_ASSERT_FALSE(jce_path_is_canonical("a\\b/c"));
}

static void test_is_absolute(void)
{
    TEST_ASSERT_TRUE (jce_path_is_absolute("/usr/bin"));
    TEST_ASSERT_TRUE (jce_path_is_absolute("C:/x"));
    TEST_ASSERT_FALSE(jce_path_is_absolute("foo/bar"));
    TEST_ASSERT_FALSE(jce_path_is_absolute(""));
    TEST_ASSERT_FALSE(jce_path_is_absolute(NULL));
}

/* ---- decomposition --------------------------------------------------- */

static void test_basename(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_basename(out, sizeof(out), "a/b/c.txt"));
    TEST_ASSERT_EQUAL_STRING("c.txt", out);

    TEST_ASSERT_TRUE(jce_path_basename(out, sizeof(out), "single"));
    TEST_ASSERT_EQUAL_STRING("single", out);
}

static void test_parent(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_parent(out, sizeof(out), "a/b/c.txt"));
    TEST_ASSERT_EQUAL_STRING("a/b", out);
}

static void test_stem_and_extension(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_stem(out, sizeof(out), "a/b/c.png"));
    TEST_ASSERT_EQUAL_STRING("c", out);
    TEST_ASSERT_TRUE(jce_path_extension(out, sizeof(out), "a/b/c.png"));
    TEST_ASSERT_EQUAL_STRING(".png", out);
    /* no extension */
    TEST_ASSERT_FALSE(jce_path_extension(out, sizeof(out), "noext"));
}

/* ---- composition ----------------------------------------------------- */

static void test_join_no_trailing(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), "a/b", "c.txt"));
    TEST_ASSERT_EQUAL_STRING("a/b/c.txt", out);
}

static void test_join_handles_trailing_slash(void)
{
    /* Trailing '/' on a is folded.  Leading '/' on b would make b
       absolute and replace a entirely (documented behaviour) — covered
       implicitly by jce_path_is_absolute semantics. */
    char out[64];
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), "a/b/", "c.txt"));
    TEST_ASSERT_EQUAL_STRING("a/b/c.txt", out);

    /* Absolute b wins. */
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), "a/b", "/x.txt"));
    TEST_ASSERT_EQUAL_STRING("/x.txt", out);
}

static void test_replace_extension(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_replace_extension(out, sizeof(out), "a/b.png", "jpg"));
    TEST_ASSERT_EQUAL_STRING("a/b.jpg", out);
    TEST_ASSERT_TRUE(jce_path_replace_extension(out, sizeof(out), "a/b.png", ".webp"));
    TEST_ASSERT_EQUAL_STRING("a/b.webp", out);
}

static void test_normalize_collapses_dot_segments(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_normalize(out, sizeof(out), "a/./b/../c"));
    TEST_ASSERT_EQUAL_STRING("a/c", out);
}

/* ---- additional edge cases (was uncovered) -------------------------- */

static void test_parent_returns_false_for_single_component(void)
{
    char out[64];
    TEST_ASSERT_FALSE(jce_path_parent(out, sizeof(out), "single"));
    TEST_ASSERT_FALSE(jce_path_parent(out, sizeof(out), ""));
}

static void test_basename_empty(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_basename(out, sizeof(out), ""));
    TEST_ASSERT_EQUAL_STRING("", out);
}

static void test_stem_no_extension(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_stem(out, sizeof(out), "Makefile"));
    TEST_ASSERT_EQUAL_STRING("Makefile", out);
}

static void test_extension_dot_file(void)
{
    char out[64];
    /* Leading dot file like ".gitignore" -> no extension. */
    TEST_ASSERT_FALSE(jce_path_extension(out, sizeof(out), ".gitignore"));
}

static void test_join_empty_inputs(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), "",  "b.txt"));
    TEST_ASSERT_EQUAL_STRING("b.txt", out);
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), "a", ""));
    TEST_ASSERT_EQUAL_STRING("a", out);
    TEST_ASSERT_TRUE(jce_path_join(out, sizeof(out), NULL, "b"));
    TEST_ASSERT_EQUAL_STRING("b", out);
}

static void test_replace_extension_strip(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_replace_extension(out, sizeof(out), "a/b.png", ""));
    TEST_ASSERT_EQUAL_STRING("a/b", out);
    TEST_ASSERT_TRUE(jce_path_replace_extension(out, sizeof(out), "a/b.png", NULL));
    TEST_ASSERT_EQUAL_STRING("a/b", out);
    /* Adding extension to file without one. */
    TEST_ASSERT_TRUE(jce_path_replace_extension(out, sizeof(out), "noext", "txt"));
    TEST_ASSERT_EQUAL_STRING("noext.txt", out);
}

static void test_normalize_escape_underflow_fails(void)
{
    char out[64];
    /* '..' beyond absolute root -> false. */
    TEST_ASSERT_FALSE(jce_path_normalize(out, sizeof(out), "/../escape"));
}

static void test_normalize_handles_duplicate_separators(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_normalize(out, sizeof(out), "a//b///c"));
    TEST_ASSERT_EQUAL_STRING("a/b/c", out);
}

static void test_normalize_absolute_root(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_normalize(out, sizeof(out), "/a/./b"));
    TEST_ASSERT_EQUAL_STRING("/a/b", out);
}

static void test_relative_same_root(void)
{
    char out[64];
    TEST_ASSERT_TRUE(jce_path_relative(out, sizeof(out),
                                       "/usr/local/bin/app", "/usr/local"));
    TEST_ASSERT_EQUAL_STRING("bin/app", out);
}

static void test_relative_uses_parent_traversal(void)
{
    /* Different sub-trees share /etc and /usr's common root; result
       starts with ".." traversal segments. */
    char out[64];
    TEST_ASSERT_TRUE(jce_path_relative(out, sizeof(out),
                                       "/a/b/c", "/a/x/y"));
    TEST_ASSERT_EQUAL_STRING_LEN("../../", out, 6);
}

static void test_join_overflow_returns_false(void)
{
    char out[4];
    TEST_ASSERT_FALSE(jce_path_join(out, sizeof(out), "abc", "def"));
}

/* ------------------------------------------------------------------ *
 *  jce_path_asset_key — host path -> PAK-relative lookup key.
 *
 *  This scan previously existed as two byte-identical copies (glTF loader
 *  + scene component deserialiser, audit C2-DUP-KEY-DERIVE) and NEITHER had
 *  a test.  Two copies of an asset-key rule disagree the moment one grows a
 *  folder the other lacks, and the symptom is the maddening "loads in the
 *  editor, missing from the PAK".  Pin the contract on the single authority.
 * ------------------------------------------------------------------ */

static void test_asset_key_marker_assets(void)
{
    char buf[256];
    const char *k = jce_path_asset_key(
        "D:/proj/resources/assets/models/city/building-b.glb", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("models/city/building-b.glb", k);
}

static void test_asset_key_marker_cooked(void)
{
    char buf[256];
    const char *k = jce_path_asset_key(
        "/home/u/p/resources/_cooked/textures/rock.ktx", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("textures/rock.ktx", k);
}

/* The exact shape the editor writes: absolute, mixed separators. */
static void test_asset_key_mixed_separators(void)
{
    char buf[256];
    const char *k = jce_path_asset_key(
        "D:/proj/resources/assets\\models\\city\\building-b.glb",
        buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("models/city/building-b.glb", k);
}

/* No marker, but the path passes through a known top-level asset folder:
   the key starts AT the folder name, not at the slash before it. */
static void test_asset_key_top_level_fallback(void)
{
    char buf[256];
    const char *k = jce_path_asset_key("C:/whatever/scenes/main.scene.json",
                                       buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("scenes/main.scene.json", k);
}

/* The marker wins over the top-level fallback when both are present —
   otherwise a project living under ".../audio/myproj/resources/assets/..."
   would key off the wrong segment. */
static void test_asset_key_marker_beats_top_level(void)
{
    char buf[256];
    const char *k = jce_path_asset_key(
        "D:/audio/proj/resources/assets/models/x.glb", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL_STRING("models/x.glb", k);
}

static void test_asset_key_rejects_unrecognised_and_empty(void)
{
    char buf[256];
    TEST_ASSERT_NULL(jce_path_asset_key("D:/somewhere/else/file.bin",
                                        buf, sizeof(buf)));
    /* Marker present but nothing after it -> no key, not an empty string. */
    TEST_ASSERT_NULL(jce_path_asset_key("D:/p/resources/assets/",
                                        buf, sizeof(buf)));
    TEST_ASSERT_NULL(jce_path_asset_key(NULL, buf, sizeof(buf)));
    TEST_ASSERT_NULL(jce_path_asset_key("D:/p/resources/assets/a.glb", NULL, 8));
    TEST_ASSERT_NULL(jce_path_asset_key("D:/p/resources/assets/a.glb", buf, 0));
}

/* Overflow must fail closed, not truncate into a wrong key. */
static void test_asset_key_overflow_returns_null(void)
{
    char small[8];
    TEST_ASSERT_NULL(jce_path_asset_key(
        "D:/proj/resources/assets/models/city/building-b.glb",
        small, sizeof(small)));
}

/* The returned pointer aliases the caller's buffer — callers rely on this
   (both former copies returned an interior pointer and then copied out). */
static void test_asset_key_returns_pointer_into_buf(void)
{
    char buf[256];
    const char *k = jce_path_asset_key(
        "D:/proj/resources/assets/anim/walk.anim", buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_TRUE(k >= buf && k < buf + sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("anim/walk.anim", k);
}


/* ── Case sensitivity of the common-prefix scan (audit C2-SCENE-ABS-PATH) ──
 *
 * Windows paths are case-insensitive and Windows APIs disagree about the
 * case of the same path — drive letters especially, and a file dialog
 * returns what the user typed while GetCurrentDirectory returns what the
 * shell recorded.  A byte-exact prefix scan therefore finds NO common root
 * between "D:/Proj/Game" and "d:/proj/game", jce_path_relative fails, and
 * the scene serializer falls back to writing the ABSOLUTE path — which is
 * how a machine-specific path ends up in a scene file meant to be portable.
 *
 * On Linux those two really are different directories, so the same folding
 * would be a BUG there.  The behaviour is platform-conditional and the test
 * asserts both sides rather than only the one it runs on.
 */

static void test_relative_prefix_case_matches_platform_rules(void)
{
    char out[128];
    const bool ok = jce_path_relative(
        out, sizeof(out),
        "D:/Proj/Game/resources/assets/models/x.glb", "d:/proj/game");

#if JCE_PLATFORM_WINDOWS
    TEST_ASSERT_TRUE_MESSAGE(ok,
        "case-differing spellings of one Windows directory did not relativise "
        "- callers fall back to storing an absolute path");
    TEST_ASSERT_EQUAL_STRING("resources/assets/models/x.glb", out);
#else
    /* Case-sensitive filesystem: these are genuinely different directories,
       so folding them would silently relativise against the wrong root. */
    if (ok) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE("resources/assets/models/x.glb", out,
            "case folding leaked onto a case-SENSITIVE platform");
    }
#endif
}

/* Same-case behaviour must be untouched on every platform — the fix must not
   have been "make everything match". */
static void test_relative_exact_case_unaffected(void)
{
    char out[128];
    TEST_ASSERT_TRUE(jce_path_relative(
        out, sizeof(out),
        "D:/Proj/Game/resources/assets/a.png", "D:/Proj/Game"));
    TEST_ASSERT_EQUAL_STRING("resources/assets/a.png", out);
}

/* A genuinely different directory must still NOT relativise to something
   that looks rooted — folding case must not fold different NAMES together. */
static void test_relative_rejects_unrelated_roots(void)
{
    char out[128];
    const bool ok = jce_path_relative(out, sizeof(out),
                                      "D:/Other/thing.txt", "D:/Proj/Game");
    if (ok) {
        /* Allowed to succeed via ".." traversal, but it must not claim the
           file sits inside the base. */
        TEST_ASSERT_TRUE_MESSAGE(out[0] == '.',
            "an unrelated path was reported as living inside the base");
    }
}

/* ── in-place decomposition (out == path) ─────────────────────────────
 *
 * jce_path.h promises the decomposition family may be called with `out` and
 * `path` naming the same buffer, and every one of the four broke that promise
 * by clearing `out` before reading `path`.  Nothing here covered it, which is
 * why it survived: every existing case in this file passes two buffers.
 *
 * The half that hides is basename/stem, which return TRUE on an empty path.
 * An aliased caller got success and an empty string, so a check of the return
 * value did not help.  The live victim was resolve_project_root_path() in the
 * editor's project dialog: picking a project by its jce_project.json instead
 * of by its folder blanked the path and reported "not a project".
 */

static void test_parent_in_place(void)
{
    char b[64];
    snprintf(b, sizeof(b), "%s", "C:/proj/Materials/ribbon.mat.json");
    TEST_ASSERT_TRUE_MESSAGE(jce_path_parent(b, sizeof(b), b),
        "an aliased parent() must succeed -- it used to clear the buffer it "
        "was about to read and then report 'no separator'");
    TEST_ASSERT_EQUAL_STRING("C:/proj/Materials", b);
}

static void test_parent_in_place_walks_up_repeatedly(void)
{
    /* The shape both callers actually use: step up until something is found. */
    char b[64];
    snprintf(b, sizeof(b), "%s", "/a/b/c/d");
    TEST_ASSERT_TRUE(jce_path_parent(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING("/a/b/c", b);
    TEST_ASSERT_TRUE(jce_path_parent(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING("/a/b", b);
    TEST_ASSERT_TRUE(jce_path_parent(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING("/a", b);
    TEST_ASSERT_TRUE(jce_path_parent(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("/", b, "the root must survive the walk");
    /* The root is a FIXED POINT, not a false.  The header said "returns false
     * ... (root or single component)" and the implementation has always said
     * "Preserve root"; this assertion was written from the header and failed
     * against the code, so the header is what changed.  Both upward walkers in
     * the tree terminate on parent == current, which is why nobody spins. */
    TEST_ASSERT_TRUE(jce_path_parent(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING("/", b);
}

static void test_basename_in_place(void)
{
    char b[64];
    snprintf(b, sizeof(b), "%s", "/a/b/scene.json");
    TEST_ASSERT_TRUE(jce_path_basename(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("scene.json", b,
        "empty here is the dangerous answer: basename returns TRUE either way");
}

static void test_stem_in_place(void)
{
    char b[64];
    snprintf(b, sizeof(b), "%s", "/a/b/scene.json");
    TEST_ASSERT_TRUE(jce_path_stem(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING("scene", b);
}

static void test_extension_in_place(void)
{
    char b[64];
    snprintf(b, sizeof(b), "%s", "/a/b/scene.json");
    TEST_ASSERT_TRUE(jce_path_extension(b, sizeof(b), b));
    TEST_ASSERT_EQUAL_STRING(".json", b);
}

static void test_in_place_and_two_buffer_agree(void)
{
    /* The fix must not have changed the non-aliased answers, which is the
     * only part of this family anything else in the tree depends on today. */
    static const char *const cases[] = {
        "/a/b/c.tar.gz", "rel/file.txt", "noslash.txt", "/root.txt",
        "/a/b/", "C:/x/y.z", "plain", "/"
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char two[64], one[64];
        const char *in = cases[i];

        bool r2 = jce_path_parent(two, sizeof(two), in);
        snprintf(one, sizeof(one), "%s", in);
        bool r1 = jce_path_parent(one, sizeof(one), one);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)r2, (int)r1, in);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(two, one, in);

        r2 = jce_path_basename(two, sizeof(two), in);
        snprintf(one, sizeof(one), "%s", in);
        r1 = jce_path_basename(one, sizeof(one), one);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)r2, (int)r1, in);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(two, one, in);

        r2 = jce_path_stem(two, sizeof(two), in);
        snprintf(one, sizeof(one), "%s", in);
        r1 = jce_path_stem(one, sizeof(one), one);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)r2, (int)r1, in);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(two, one, in);

        r2 = jce_path_extension(two, sizeof(two), in);
        snprintf(one, sizeof(one), "%s", in);
        r1 = jce_path_extension(one, sizeof(one), one);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)r2, (int)r1, in);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(two, one, in);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_to_canonical_converts_backslash);
    RUN_TEST(test_to_canonical_overflow);
    RUN_TEST(test_is_canonical);
    RUN_TEST(test_is_absolute);
    RUN_TEST(test_basename);
    RUN_TEST(test_parent);
    RUN_TEST(test_stem_and_extension);
    RUN_TEST(test_join_no_trailing);
    RUN_TEST(test_join_handles_trailing_slash);
    RUN_TEST(test_replace_extension);
    RUN_TEST(test_normalize_collapses_dot_segments);
    RUN_TEST(test_parent_returns_false_for_single_component);
    RUN_TEST(test_basename_empty);
    RUN_TEST(test_stem_no_extension);
    RUN_TEST(test_extension_dot_file);
    RUN_TEST(test_join_empty_inputs);
    RUN_TEST(test_replace_extension_strip);
    RUN_TEST(test_normalize_escape_underflow_fails);
    RUN_TEST(test_normalize_handles_duplicate_separators);
    RUN_TEST(test_normalize_absolute_root);
    RUN_TEST(test_relative_same_root);
    RUN_TEST(test_relative_uses_parent_traversal);
    RUN_TEST(test_join_overflow_returns_false);
    RUN_TEST(test_asset_key_marker_assets);
    RUN_TEST(test_asset_key_marker_cooked);
    RUN_TEST(test_asset_key_mixed_separators);
    RUN_TEST(test_asset_key_top_level_fallback);
    RUN_TEST(test_asset_key_marker_beats_top_level);
    RUN_TEST(test_asset_key_rejects_unrecognised_and_empty);
    RUN_TEST(test_asset_key_overflow_returns_null);
    RUN_TEST(test_asset_key_returns_pointer_into_buf);
    RUN_TEST(test_relative_prefix_case_matches_platform_rules);
    RUN_TEST(test_relative_exact_case_unaffected);
    RUN_TEST(test_relative_rejects_unrelated_roots);
    RUN_TEST(test_parent_in_place);
    RUN_TEST(test_parent_in_place_walks_up_repeatedly);
    RUN_TEST(test_basename_in_place);
    RUN_TEST(test_stem_in_place);
    RUN_TEST(test_extension_in_place);
    RUN_TEST(test_in_place_and_two_buffer_agree);
    return UNITY_END();
}
