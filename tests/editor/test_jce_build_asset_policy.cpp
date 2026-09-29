#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_build_asset_policy.h"

TEST_CASE("runtime asset publication keeps consumable content")
{
    CHECK(jce_build_asset_path_is_publishable("engine_debug_hud.rml"));
    CHECK(jce_build_asset_path_is_publishable(
        "scripts/space_codex_director.lua"));
    CHECK(jce_build_asset_path_is_publishable(
        "licenses/THIRD_PARTY_LICENSES.md"));
    CHECK(jce_build_asset_path_is_publishable(
        "custom/runtime_payload.anything"));
}

TEST_CASE("runtime asset publication rejects authoring metadata")
{
    CHECK_FALSE(jce_build_asset_path_is_publishable("AGENTS.md"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("docs/README.md"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("CMakeLists.txt"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("scripts/build.ps1"));
    CHECK_FALSE(jce_build_asset_path_is_publishable(".git/config"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("cache/__pycache__/x.pyc"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("raw_assets/source.blend"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("textures/.DS_Store"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("textures/Thumbs.db"));
}

/* The regression this file exists to stop is invisible from the editor: a
 * stripped script keeps working from loose files and is missing only in the
 * PACKAGED build.  `.py` used to be denied here as build tooling, from before
 * Python was a scripting language the engine ships — an extension cannot tell
 * turret.py from build.py, so the rule that dropped one dropped the other.
 * The languages come from the engine's single script-extension authority
 * (jce_asset_script_language_from_ext), so a sixth language is covered here
 * the moment it is added there. */
TEST_CASE("every shippable script language survives publication")
{
    CHECK(jce_build_asset_path_is_publishable("scripts/director.lua"));
    CHECK(jce_build_asset_path_is_publishable("scripts/turret.py"));
    CHECK(jce_build_asset_path_is_publishable("scripts/Turret.java"));
    CHECK(jce_build_asset_path_is_publishable("scripts/Turret.class"));
    CHECK(jce_build_asset_path_is_publishable("Scripts\\TURRET.PY"));
}

/* Tooling that no scripting language claims stays out — the positive rule
 * above must not become "publish anything that looks like code". */
TEST_CASE("host tooling extensions are still rejected")
{
    CHECK_FALSE(jce_build_asset_path_is_publishable("tools/run.bat"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("tools/run.cmd"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("tools/run.ps1"));
    CHECK_FALSE(jce_build_asset_path_is_publishable("tools/run.sh"));
    /* A CPython cache artefact, not authored content, and claimed by no
     * language — the one .py-adjacent extension that must NOT publish. */
    CHECK_FALSE(jce_build_asset_path_is_publishable("scripts/turret.pyc"));
}

TEST_CASE("publication policy is separator and case insensitive")
{
    CHECK_FALSE(jce_build_asset_path_is_publishable(
        "Nested\\Agents.MD"));
    CHECK_FALSE(jce_build_asset_path_is_publishable(
        "Tools\\BUILD.CMD"));
    CHECK(jce_build_asset_path_is_publishable(
        "Scripts\\Runtime.LUA"));
}
