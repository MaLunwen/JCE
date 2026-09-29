/*
 * test_jce_asset_ref_rewrite.cpp — renaming an asset must repoint what uses it.
 *
 * Every asset reference in this engine is a raw path string; there is no GUID
 * and no .meta sidecar anywhere in the tree.  Renaming a file in the Asset
 * Browser called jce_fs_host_rename, logged success, and touched nothing else,
 * so every scene, prefab and material pointing at the old name silently went
 * to a missing asset.  The engine's mitigation, the fuzzy basename index in
 * editor/src/scene/jce_asset_path_index.*, is powerless here by construction:
 * a rename is exactly the case where the basename changed.
 *
 * WHAT THESE CASES ARE REALLY ABOUT is the rewrite being CONSERVATIVE.  A
 * textual rewrite that is too eager is worse than the broken reference it set
 * out to fix: it corrupts neighbouring filenames and prose, in files the user
 * did not open, silently.  So most of what follows asserts the rewrite does
 * NOT fire.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_asset_ref_rewrite.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
}

#include <cstring>
#include <string>

namespace {

struct Result {
    int         hits = 0;
    std::string text;
    bool        allocated = false;
};

Result rw(const char *src,
          const char *old_rel,  const char *new_rel,
          const char *old_base, const char *new_base)
{
    Result r;
    char  *out = nullptr;
    size_t out_len = 0;
    r.hits = jce_asset_ref_rewrite(src, std::strlen(src), old_rel, new_rel,
                                   old_base, new_base, &out, &out_len);
    r.allocated = (out != nullptr);
    if (out) {
        r.text.assign(out, out_len);
        jce_free(out);
    }
    return r;
}

} /* namespace */

TEST_CASE("a relative path is repointed, and its neighbours are not")
{
    const Result r = rw(
        "{\"albedo\":\"Textures/wood.png\",\"normal\":\"Textures/wood_n.png\"}",
        "Textures/wood.png", "Textures/oak.png", "wood.png", "oak.png");
    CHECK(r.hits == 1);
    /* wood_n.png is a DIFFERENT asset; rewriting it would break a reference
     * that was never stale. */
    CHECK(r.text ==
          "{\"albedo\":\"Textures/oak.png\",\"normal\":\"Textures/wood_n.png\"}");
}

TEST_CASE("the whole path wins over its own tail")
{
    /* "Art/Old/wood.png" ends with "wood.png".  Matching the basename first
     * would rewrite the tail and leave the directory stale -- a half-renamed
     * reference, silently, and only in the files that spelled it the more
     * careful way. */
    const Result r = rw("\"Art/Old/wood.png\"",
                        "Art/Old/wood.png", "Art/New/oak.png",
                        "wood.png", "oak.png");
    CHECK(r.hits == 1);
    CHECK(r.text == "\"Art/New/oak.png\"");
}

TEST_CASE("a bare basename in quotes is repointed")
{
    /* jce_assetdb counts a file as a referrer when it contains the bare
     * basename too, so a rewrite that only handled rel paths would report
     * "updated 3 files" while leaving these as broken as before. */
    const Result r = rw("{\"clip\":\"footstep.wav\"}",
                        "Audio/footstep.wav", "Audio/step.wav",
                        "footstep.wav", "step.wav");
    CHECK(r.hits == 1);
    CHECK(r.text == "{\"clip\":\"step.wav\"}");
}

TEST_CASE("a basename inside a longer name is left alone")
{
    /* THE DANGEROUS CASE.  "wood.png" occurs inside "darkwood.png".  An
     * undelimited substring rewrite turns an unrelated, working reference into
     * a broken one, in a file the user never opened. */
    const Result r = rw("{\"albedo\":\"darkwood.png\"}",
                        nullptr, nullptr, "wood.png", "oak.png");
    CHECK(r.hits == 0);
}

TEST_CASE("a basename in prose is left alone")
{
    /* Scene and material files carry comments and display names.  A rewrite
     * that edits prose corrupts data it does not understand. */
    const Result r = rw("{\"note\":\"replaces wood.png later\"}",
                        nullptr, nullptr, "wood.png", "oak.png");
    CHECK(r.hits == 0);
}

TEST_CASE("a raw Windows separator still matches the rel path")
{
    /* A scene saved on Windows can carry backslashes for the same asset, and
     * jce_assetdb finds it either way.  Rewriting only the forward-slash form
     * would repair half the projects and report success for all of them.  The
     * replacement normalises to '/', which is what the engine writes. */
    const char raw[] = { '"', 'M','o','d','e','l','s', '\\',
                         'r','o','c','k','.','f','b','x', '"', '\0' };
    const Result r = rw(raw, "Models/rock.fbx", "Models/boulder.fbx",
                        "rock.fbx", "boulder.fbx");
    CHECK(r.hits == 1);
    CHECK(r.text == "\"Models/boulder.fbx\"");
}

TEST_CASE("a JSON-escaped separator falls through to the basename")
{
    /* A scene file that escaped its separator carries TWO bytes -- \\ -- so
     * the rel path cannot match it, and the basename rule does, leaving the
     * directory bytes exactly as the file had them.
     *
     * This is the right outcome and it is worth pinning: the alternative,
     * teaching the matcher to unescape, would have it rewriting JSON string
     * escapes it does not otherwise parse, in files whose grammar it does not
     * know.  Renaming within a directory is the common case and this handles
     * it; a MOVE across directories in an escaped file is reported unrepaired
     * rather than half-repaired. */
    const Result r = rw("{\"mesh\":\"Models\\\\rock.fbx\"}",
                        "Models/rock.fbx", "Models/boulder.fbx",
                        "rock.fbx", "boulder.fbx");
    CHECK(r.hits == 1);
    CHECK(r.text == "{\"mesh\":\"Models\\\\boulder.fbx\"}");
}

TEST_CASE("every occurrence is rewritten")
{
    const Result r = rw("[\"Tex/a.png\",\"Tex/a.png\",\"Tex/b.png\",\"Tex/a.png\"]",
                        "Tex/a.png", "Tex/z.png", "a.png", "z.png");
    CHECK(r.hits == 3);   /* a material can name one texture in several slots */
    CHECK(r.text == "[\"Tex/z.png\",\"Tex/z.png\",\"Tex/b.png\",\"Tex/z.png\"]");
}

TEST_CASE("no match reports zero and allocates nothing")
{
    /* The caller must be able to tell "I repaired this file" from "the index
     * flagged it and I could not", and it tells them apart by the count.  A
     * buffer handed back on a zero would be written back unchanged at best. */
    const Result r = rw("{\"albedo\":\"Textures/stone.png\"}",
                        "Textures/wood.png", "Textures/oak.png",
                        "wood.png", "oak.png");
    CHECK(r.hits == 0);
    CHECK_FALSE(r.allocated);
}

TEST_CASE("degenerate inputs do not crash")
{
    char *out = nullptr;
    CHECK(jce_asset_ref_rewrite(nullptr, 0, "a", "b", "a", "b", &out, nullptr) == 0);
    CHECK(jce_asset_ref_rewrite("x", 1, "a", "b", "a", "b", nullptr, nullptr) == 0);
    CHECK(jce_asset_ref_rewrite("x", 1, nullptr, nullptr, nullptr, nullptr,
                                &out, nullptr) == 0);
    CHECK(jce_asset_ref_rewrite("", 0, "a/b.png", "a/c.png", "b.png", "c.png",
                                &out, nullptr) == 0);
    CHECK(out == nullptr);
}
