/*
 * test_jce_editor_assetdb.cpp — editor asset DB coverage.
 *
 * Pure(ish) C++ — no ImGui, no editor state. Exercises:
 *   - kind_label() string table
 *   - set_root(NULL/"") guard + get_root()
 *   - get_kind() extension-only fallback when no scan has happened
 *   - rescan() against a temp directory containing several file types,
 *     including a .scene referencing a texture (reverse-ref lookup)
 *
 * Temp dirs follow the engine convention:
 *   <cwd>/_ut_assetdb_<leaf>/
 * which is the same pattern used by test_jce_filesystem_host.c.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_assetdb.h"

extern "C" {
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_alloc.h>
}

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string ut_dir(const char *leaf)
{
    char cwd[1024] = {0};
    jce_fs_host_get_current_dir(cwd, sizeof(cwd));
    std::string p = cwd;
    p += "/_ut_assetdb_";
    p += leaf;
    return p;
}

void write_text(const std::string &path, const char *data)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    if (data && *data) std::fwrite(data, 1, std::strlen(data), f);
    std::fclose(f);
}

void cleanup_tree(const std::string &dir)
{
    /* Best-effort: walk and remove files, then rmdir. We don't care if
     * pieces are missing — only that the next test starts clean. */
    jce_fs_host_walk(dir.c_str(),
        [](const char *p, bool is_dir, void *) -> bool {
            if (!is_dir) std::remove(p);
            return true;
        },
        nullptr);
    std::remove(dir.c_str());
}

} /* namespace */

TEST_CASE("kind_label covers every defined JceAssetKind")
{
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_UNKNOWN), "unknown") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_TEXTURE), "texture") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_MODEL), "model") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_AUDIO), "audio") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_MATERIAL), "material") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_SCENE), "scene") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_SHADER), "shader") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_SCRIPT), "script") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_PARTICLE), "particle") == 0);
    CHECK(std::strcmp(jce_assetdb_kind_label(JCE_ASSET_KIND_DATA), "data") == 0);
    /* Out-of-range falls through the switch default. */
    CHECK(std::strcmp(jce_assetdb_kind_label((JceAssetKind)999), "unknown") == 0);
}

TEST_CASE("set_root with NULL or empty resets to empty")
{
    jce_assetdb_set_root(nullptr);
    CHECK(std::strcmp(jce_assetdb_get_root(), "") == 0);
    CHECK(jce_assetdb_count() == 0);

    jce_assetdb_set_root("");
    CHECK(std::strcmp(jce_assetdb_get_root(), "") == 0);
    CHECK(jce_assetdb_count() == 0);
}

TEST_CASE("get_kind falls back to extension-based classify when not scanned")
{
    jce_assetdb_set_root(nullptr); /* ensure DB is empty */

    CHECK(jce_assetdb_get_kind("foo.png") == JCE_ASSET_KIND_TEXTURE);
    CHECK(jce_assetdb_get_kind("/abs/path/bar.GLTF") == JCE_ASSET_KIND_MODEL);
    CHECK(jce_assetdb_get_kind("c:\\proj\\sfx.WAV") == JCE_ASSET_KIND_AUDIO);
    CHECK(jce_assetdb_get_kind("level.scn") == JCE_ASSET_KIND_SCENE);
    CHECK(jce_assetdb_get_kind("particles.particle") == JCE_ASSET_KIND_PARTICLE);
    CHECK(jce_assetdb_get_kind("config.json") == JCE_ASSET_KIND_DATA);
    CHECK(jce_assetdb_get_kind("readme.md") == JCE_ASSET_KIND_UNKNOWN);
    CHECK(jce_assetdb_get_kind(nullptr) == JCE_ASSET_KIND_UNKNOWN);
    CHECK(jce_assetdb_get_kind("") == JCE_ASSET_KIND_UNKNOWN);
}

/* The Script component's path picker filters on JCE_ASSET_KIND_SCRIPT, so a
 * language missing from this classification cannot be ATTACHED at all — the
 * file never appears in the picker.  The languages come from the engine's
 * single script-extension authority, not from a list spelled out here, which
 * is what keeps the picker and the cooker from disagreeing. */
TEST_CASE("every shipped script language classifies as script")
{
    jce_assetdb_set_root(nullptr);
    CHECK(jce_assetdb_get_kind("scripts/director.lua") == JCE_ASSET_KIND_SCRIPT);
    CHECK(jce_assetdb_get_kind("scripts/turret.py") == JCE_ASSET_KIND_SCRIPT);
    CHECK(jce_assetdb_get_kind("scripts/Turret.java") == JCE_ASSET_KIND_SCRIPT);
    CHECK(jce_assetdb_get_kind("scripts/Turret.CLASS") == JCE_ASSET_KIND_SCRIPT);
    /* Code the engine does not load by path still files under "script" for
     * the browser, and shader source still does not. */
    CHECK(jce_assetdb_get_kind("src/turret.cpp") == JCE_ASSET_KIND_SCRIPT);
    CHECK(jce_assetdb_get_kind("shaders/vs_pbr.sc") == JCE_ASSET_KIND_SHADER);
}

/* THE PICKER OFFERS EXACTLY WHAT A SCRIPT COMPONENT ACCEPTS.
 *
 * jce_dialog_asset_picker filters on jce_assetdb_script_language() whenever
 * the requested kind is JCE_ASSET_KIND_SCRIPT, and
 * jce_panel_inspector_gameplay asks the SAME function whether the attached
 * path resolves to a language.  One function, so the two sets cannot drift;
 * this test is what fails if someone reroutes either caller back to the kind.
 *
 * The case that motivated it: on the real Elemental Serenity project the
 * kind-only filter offered 20 entries of which 5 resolved.  Among the 15
 * that did not was the project's own C++ script SOURCE — a cpp scriptPath
 * names the class through the backend's ".jcecpp" extension, so attaching the
 * .cpp translation unit by path is silently dead and stays that way. */
TEST_CASE("script picker offers exactly what a Script component accepts")
{
    jce_assetdb_set_root(nullptr);

    /* Every shipped language: classified as script AND attachable, with the
     * language name the runtime will select on. */
    struct { const char *path; const char *lang; } ok[] = {
        { "scripts/director.lua",  "lua"    },
        { "scripts/turret.py",     "python" },
        { "scripts/Turret.java",   "java"   },
        { "scripts/Turret.class",  "java"   },
        { "scripts/Turret.CLASS",  "java"   },   /* extension match is ci */
    };
    for (const auto &c : ok) {
        CAPTURE(c.path);
        CHECK(jce_assetdb_get_kind(c.path) == JCE_ASSET_KIND_SCRIPT);
        REQUIRE(jce_assetdb_script_language(c.path) != nullptr);
        CHECK(std::strcmp(jce_assetdb_script_language(c.path), c.lang) == 0);
    }

    /* Project code: still kind=script for the browser, never attachable.
     * These are the entries the picker used to offer. */
    static const char *const code[] = {
        "src/turret.cpp", "src/turret.c", "src/turret.h", "src/turret.hpp",
        "web/tool.js",    "web/tool.ts",
    };
    for (const char *p : code) {
        CAPTURE(p);
        CHECK(jce_assetdb_get_kind(p) == JCE_ASSET_KIND_SCRIPT);
        CHECK(jce_assetdb_script_language(p) == nullptr);
    }

    /* A cpp script's stored path is a CLASS NAME, not a file, and the editor
     * cannot promise anything about it offline — the inspector says so
     * rather than guessing.  Both the extension-less form and a project's
     * own claimed extension are unknown to the shipped catalog. */
    CHECK(jce_assetdb_script_language("EsFlowerSway") == nullptr);
    CHECK(jce_assetdb_script_language("EsFlowerSway.escpp") == nullptr);

    /* Degenerate paths answer NULL rather than crashing: script_path is a
     * fixed char[128] a user types into freely. */
    CHECK(jce_assetdb_script_language(nullptr) == nullptr);
    CHECK(jce_assetdb_script_language("") == nullptr);
    CHECK(jce_assetdb_script_language("scripts/noext") == nullptr);
    CHECK(jce_assetdb_script_language("scripts/trailingdot.") == nullptr);
    /* Only the last component has an extension: a directory's dot must not
     * be read as one, or "scripts.py/readme" would resolve to python. */
    CHECK(jce_assetdb_script_language("scripts.py/readme") == nullptr);
}

/* The picker's actual filter decision, not a helper it happens to call.
 * jce_dialog_asset_picker's loop is this function plus a search-text match,
 * so dropping or inverting the script rule fails here. */
TEST_CASE("asset picker filter decision")
{
    jce_assetdb_set_root(nullptr);

    /* "Any" offers everything, including code the Script picker will not. */
    CHECK(jce_assetdb_picker_accepts(0, JCE_ASSET_KIND_SCRIPT, "src/a.cpp"));
    CHECK(jce_assetdb_picker_accepts(0, JCE_ASSET_KIND_TEXTURE, "t.png"));

    /* Wrong kind is refused regardless of path. */
    CHECK_FALSE(jce_assetdb_picker_accepts(JCE_ASSET_KIND_TEXTURE,
                                           JCE_ASSET_KIND_SCRIPT, "a.lua"));
    CHECK_FALSE(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                           JCE_ASSET_KIND_TEXTURE, "t.png"));

    /* THE RULE.  Right kind AND resolvable -> offered. */
    CHECK(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                     JCE_ASSET_KIND_SCRIPT,
                                     "scripts/es_fireflies.py"));
    CHECK(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                     JCE_ASSET_KIND_SCRIPT,
                                     "scripts/EsCampfire.java"));
    CHECK(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                     JCE_ASSET_KIND_SCRIPT,
                                     "scripts/es_director.lua"));
    /* Right kind, NOT resolvable -> withheld.  This is the assertion that
     * fails if the filter goes back to comparing kinds alone. */
    CHECK_FALSE(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                           JCE_ASSET_KIND_SCRIPT,
                                           "src/es_flower_sway.cpp"));
    CHECK_FALSE(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                           JCE_ASSET_KIND_SCRIPT,
                                           "src/es_script_probe.h"));

    /* The extra rule applies to SCRIPT only: a kind whose files have no
     * language must not be filtered by one.  Requesting DATA still offers a
     * .json, which resolves to no script language at all. */
    CHECK(jce_assetdb_picker_accepts(JCE_ASSET_KIND_DATA,
                                     JCE_ASSET_KIND_DATA, "config.json"));
    CHECK(jce_assetdb_picker_accepts(JCE_ASSET_KIND_SHADER,
                                     JCE_ASSET_KIND_SHADER, "vs_pbr.sc"));

    /* Every entry the ES project puts under kind=script, counted the way the
     * picker counts them.  5 offered / 15 withheld was the measurement that
     * motivated the rule; the shapes are what matter, not the tree. */
    static const char *const es_script_kind[] = {
        "resources/assets/scripts/es_director.lua",
        "resources/assets/scripts/es_fireflies.py",
        "resources/assets/scripts/EsCampfire.java",
        "src/es_flower_sway.cpp", "src/es_script_langs.c",
        "src/es_script_langs.h", "src/es_script_probe.c",
        "src/es_script_probe.h", "src/main.c", "src/es_camera.h",
    };
    int offered = 0;
    for (const char *p : es_script_kind)
        if (jce_assetdb_picker_accepts(JCE_ASSET_KIND_SCRIPT,
                                       JCE_ASSET_KIND_SCRIPT, p))
            ++offered;
    CHECK(offered == 3);
}

TEST_CASE("path_at / kind_at bounds-check")
{
    jce_assetdb_set_root(nullptr);
    CHECK(jce_assetdb_path_at(-1) == nullptr);
    CHECK(jce_assetdb_path_at(0) == nullptr);
    CHECK(jce_assetdb_kind_at(-1) == JCE_ASSET_KIND_UNKNOWN);
    CHECK(jce_assetdb_kind_at(100) == JCE_ASSET_KIND_UNKNOWN);
}

TEST_CASE("rescan classifies files and builds reverse references")
{
    std::string root = ut_dir("scan");
    cleanup_tree(root);
    REQUIRE(jce_fs_host_create_directory(root.c_str()));

    write_text(root + "/hero.png", "fake-png");
    write_text(root + "/world.scn", "scene contains hero.png reference");
    write_text(root + "/sfx.wav", "fake-wav");

    jce_assetdb_set_root(root.c_str()); /* triggers rescan internally */

    CHECK(std::string(jce_assetdb_get_root()).find("_ut_assetdb_scan")
          != std::string::npos);
    CHECK(jce_assetdb_count() == 3);

    /* path_at / kind_at iterate the in-memory list. */
    int seen = 0;
    for (int i = 0; i < jce_assetdb_count(); i++) {
        const char *p = jce_assetdb_path_at(i);
        REQUIRE(p != nullptr);
        JceAssetKind k = jce_assetdb_kind_at(i);
        if (std::string(p).find("hero.png") != std::string::npos) {
            CHECK(k == JCE_ASSET_KIND_TEXTURE);
            ++seen;
        } else if (std::string(p).find("world.scn") != std::string::npos) {
            CHECK(k == JCE_ASSET_KIND_SCENE);
            ++seen;
        } else if (std::string(p).find("sfx.wav") != std::string::npos) {
            CHECK(k == JCE_ASSET_KIND_AUDIO);
            ++seen;
        }
    }
    CHECK(seen == 3);

    /* Reverse references: world.scn mentions hero.png → its DB entry
     * (full canonical path) should map back to world.scn. */
    std::string hero = root + "/hero.png";
    char out[4][512];
    int total = jce_assetdb_find_references(hero.c_str(), out, 4);
    CHECK(total >= 1);
    if (total >= 1) {
        CHECK(std::string(out[0]).find("world.scn") != std::string::npos);
    }

    /* Unknown asset → 0 references. */
    CHECK(jce_assetdb_find_references("/no/such/asset.png", out, 4) == 0);
    CHECK(jce_assetdb_find_references(nullptr, out, 4) == 0);

    /* Reset + cleanup. */
    jce_assetdb_set_root(nullptr);
    cleanup_tree(root);
}

TEST_CASE("rescan on non-existent root leaves DB empty without crashing")
{
    std::string root = ut_dir("missing");
    cleanup_tree(root); /* make sure it really doesn't exist */

    jce_assetdb_set_root(root.c_str());
    CHECK(jce_assetdb_count() == 0);
    CHECK(jce_assetdb_path_at(0) == nullptr);

    jce_assetdb_set_root(nullptr);
}

TEST_CASE("renaming an asset repoints the files that referenced it")
{
    /* THE WHOLE POINT.  Rename used to be jce_fs_host_rename plus a success
     * log: the file moved and every scene, prefab and material naming it went
     * silently to a missing asset.  There is no GUID and no .meta sidecar in
     * this engine -- references are raw path strings -- and the fuzzy basename
     * index cannot cover a rename, because a rename is where the basename
     * changed.  jce_assetdb_find_references knew who pointed at what and had
     * zero callers.
     *
     * The observable is the REFERRING FILE'S BYTES after the rename, not the
     * return value: true only means the file moved. */
    std::string root = ut_dir("rename");
    cleanup_tree(root);
    REQUIRE(jce_fs_host_create_directory(root.c_str()));

    write_text(root + "/hero.png", "fake-png");
    write_text(root + "/other.png", "fake-png");
    write_text(root + "/world.scn",
               "{\"tex\":\"hero.png\",\"keep\":\"other.png\"}");

    jce_assetdb_set_root(root.c_str());

    const std::string oldp = root + "/hero.png";
    const std::string newp = root + "/champion.png";
    int upd = -1, unrep = -1;
    CHECK(jce_assetdb_rename_asset(oldp.c_str(), newp.c_str(), &upd, &unrep));

    CHECK(jce_fs_host_exists_file(newp.c_str()));
    CHECK_FALSE(jce_fs_host_exists_file(oldp.c_str()));
    CHECK(upd == 1);
    CHECK(unrep == 0);

    size_t sz = 0;
    char *raw = (char *)jce_fs_host_read_all((root + "/world.scn").c_str(), &sz);
    REQUIRE(raw != nullptr);
    const std::string body(raw, sz);
    jce_free(raw);
    CHECK(body.find("champion.png") != std::string::npos);
    CHECK(body.find("hero.png") == std::string::npos);
    /* The other texture was never stale and must not have been touched. */
    CHECK(body.find("other.png") != std::string::npos);

    jce_assetdb_set_root(nullptr);
    cleanup_tree(root);
}

TEST_CASE("a rename with no referrers reports zero, not failure")
{
    /* Renaming an unused asset is the common case and must not look like an
     * error, or the warning that DOES matter stops being read. */
    std::string root = ut_dir("rename_none");
    cleanup_tree(root);
    REQUIRE(jce_fs_host_create_directory(root.c_str()));
    write_text(root + "/lonely.png", "fake-png");
    jce_assetdb_set_root(root.c_str());

    int upd = -1, unrep = -1;
    CHECK(jce_assetdb_rename_asset((root + "/lonely.png").c_str(),
                                   (root + "/solo.png").c_str(),
                                   &upd, &unrep));
    CHECK(upd == 0);
    CHECK(unrep == 0);
    CHECK(jce_fs_host_exists_file((root + "/solo.png").c_str()));

    jce_assetdb_set_root(nullptr);
    cleanup_tree(root);
}

TEST_CASE("a rename that cannot move the file changes nothing else")
{
    /* If the move fails, rewriting referrers would point every scene at a file
     * that does not exist -- strictly worse than the stale reference. */
    std::string root = ut_dir("rename_fail");
    cleanup_tree(root);
    REQUIRE(jce_fs_host_create_directory(root.c_str()));
    write_text(root + "/a.png", "fake-png");
    write_text(root + "/w.scn", "{\"tex\":\"a.png\"}");
    jce_assetdb_set_root(root.c_str());

    int upd = -1, unrep = -1;
    CHECK_FALSE(jce_assetdb_rename_asset(
        (root + "/missing.png").c_str(),
        (root + "/nowhere/deep/b.png").c_str(), &upd, &unrep));
    CHECK(upd == 0);
    CHECK(unrep == 0);

    size_t sz = 0;
    char *raw = (char *)jce_fs_host_read_all((root + "/w.scn").c_str(), &sz);
    REQUIRE(raw != nullptr);
    const std::string body(raw, sz);
    jce_free(raw);
    CHECK(body == "{\"tex\":\"a.png\"}");

    jce_assetdb_set_root(nullptr);
    cleanup_tree(root);
}

TEST_CASE("degenerate rename arguments are refused")
{
    int upd = -1, unrep = -1;
    CHECK_FALSE(jce_assetdb_rename_asset(nullptr, "x", &upd, &unrep));
    CHECK_FALSE(jce_assetdb_rename_asset("x", nullptr, &upd, &unrep));
    CHECK_FALSE(jce_assetdb_rename_asset("", "x", &upd, &unrep));
    CHECK_FALSE(jce_assetdb_rename_asset("x", "", nullptr, nullptr));
}

TEST_CASE("find_references counts without writing when given no buffer")
{
    /* The Asset Browser's Delete confirm asks "how many files reference this"
     * and has nowhere to put the names, so it calls with (NULL, 0).  That
     * shape was never exercised before the dialog needed it, and a
     * count-without-buffer that returned 0 -- or wrote through the NULL --
     * would make the warning say "referenced by nothing" about an asset the
     * whole level uses. */
    std::string root = ut_dir("refcount");
    cleanup_tree(root);
    REQUIRE(jce_fs_host_create_directory(root.c_str()));
    write_text(root + "/hero.png", "fake-png");
    write_text(root + "/a.scn", "{\"tex\":\"hero.png\"}");
    write_text(root + "/b.scn", "{\"tex\":\"hero.png\"}");
    jce_assetdb_set_root(root.c_str());

    const std::string hero = root + "/hero.png";
    const int total = jce_assetdb_find_references(hero.c_str(), nullptr, 0);
    CHECK(total == 2);

    /* And the same number when a buffer IS supplied, so the count the dialog
     * shows and the list the repair walks cannot disagree. */
    char out[4][512];
    CHECK(jce_assetdb_find_references(hero.c_str(), out, 4) == total);

    /* An unreferenced asset is 0, not "unknown". */
    write_text(root + "/lonely.png", "fake-png");
    jce_assetdb_rescan();
    CHECK(jce_assetdb_find_references((root + "/lonely.png").c_str(),
                                      nullptr, 0) == 0);

    jce_assetdb_set_root(nullptr);
    cleanup_tree(root);
}
