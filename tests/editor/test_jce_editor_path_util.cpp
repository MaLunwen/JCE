// test_jce_editor_path_util.cpp — tests for jce_editor_path_to_relative_to.
//
// We compile the editor .cpp directly into this exe (see CMakeLists) so we
// avoid the JCE_Editor umbrella target.  The translation unit references
// jce_editor_assets_get_project() through jce_editor_path_to_relative() —
// we stub that in test_stub_assets_get_project.cpp so the link resolves.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <jce/os/core/jce_defs.h>   /* JCE_PLATFORM_WINDOWS */
#include <jce/os/core/jce_path.h>   /* jce_path_is_absolute  */

#include <cstring>
#include <cstddef>
#include <string>

extern "C" void jce_editor_path_to_relative_to(char *out, std::size_t out_size,
                                               const char *abs_or_rel_path,
                                               const char *base_dir);

TEST_CASE("relative input is forward-slash normalized and copied through")
{
    char out[256] = {0};
    jce_editor_path_to_relative_to(out, sizeof(out),
                                   "sub\\dir\\file.txt", "C:/proj");
    CHECK(std::string(out) == "sub/dir/file.txt");
}

TEST_CASE("empty input clears output buffer")
{
    char out[16] = "garbage";
    jce_editor_path_to_relative_to(out, sizeof(out), "", "C:/proj");
    CHECK(out[0] == '\0');
}

TEST_CASE("null output is a no-op (no crash)")
{
    jce_editor_path_to_relative_to(nullptr, 0, "anything", "anything");
    CHECK(true);
}

TEST_CASE("absolute path under base becomes relative")
{
    char out[256] = {0};
    jce_editor_path_to_relative_to(out, sizeof(out),
                                   "C:/proj/assets/foo.bin", "C:/proj");
    /* Result must be relative (no drive, forward slashes). */
    CHECK(std::strstr(out, "assets") != nullptr);
    CHECK(std::strstr(out, "foo.bin") != nullptr);
    CHECK(std::strchr(out, '\\') == nullptr);
    CHECK(out[0] != 'C');
}

TEST_CASE("absolute path escaping base falls back to absolute (forward-slash)")
{
    char out[256] = {0};
    jce_editor_path_to_relative_to(out, sizeof(out),
                                   "D:/other/file.bin", "C:/proj");
    /* Should be untouched aside from slash normalization (no `..` escape). */
    CHECK(std::strchr(out, '\\') == nullptr);
    CHECK(std::strstr(out, "other") != nullptr);
    CHECK(std::strstr(out, "..") == nullptr);
}

TEST_CASE("absolute path with empty base falls back to absolute (normalized)")
{
    char out[256] = {0};
    jce_editor_path_to_relative_to(out, sizeof(out),
                                   "C:\\proj\\file.bin", "");
    CHECK(std::strchr(out, '\\') == nullptr);
    CHECK(std::strstr(out, "file.bin") != nullptr);
}

/* Audit C2-SCENE-ABS-PATH, end to end at the editor level.
 *
 * Windows APIs disagree about the case of the same path, so a project root
 * recorded as "d:/proj/game" and an asset picked as "D:/Proj/Game/..." are
 * one directory.  The chain here is rel_in_place -> this function ->
 * jce_path_relative, and jce_path_relative compared bytes, so it produced
 * "../../../D:/Proj/Game/..." — which this function correctly rejects for
 * starting with "..", then falls back to ABSOLUTE.
 *
 * The user-visible result was a machine-specific path written into a scene
 * file, plus a warning telling them to move an asset that was already inside
 * the project root.  Both come from the same byte-exact comparison.
 */
TEST_CASE("case-differing project root still relativises on Windows")
{
    char out[512] = {0};
    jce_editor_path_to_relative_to(
        out, sizeof out,
        "D:/Proj/Game/resources/assets/models/x.glb",
        "d:/proj/game");

#if JCE_PLATFORM_WINDOWS
    INFO("got: " << out);
    CHECK(std::string(out) == "resources/assets/models/x.glb");
    /* The specific regression: an absolute path must not survive into the
       scene for an asset that IS under the project root. */
    CHECK(!jce_path_is_absolute(out));
#else
    /* Case-sensitive filesystem: these are different directories, so falling
       back to absolute is CORRECT here and must not be "fixed". */
    CHECK(out[0] != '\0');
#endif
}
