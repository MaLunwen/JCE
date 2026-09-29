/*
 * test_jce_editor_run_resolve.cpp — the editor "▶ Run" launch path.
 *
 * jce_run_manager_resolve_path() decides which binary Run launches.  It is
 * pure: every input arrives in JceRunResolveInputs and the only I/O is the
 * injected existence predicate, so the whole resolution order is testable
 * without build_manager, assetdb, the panels, the console, or a disk.
 *
 * The case that matters most is EMPTY_CONFIGURED_STILL_RESOLVES: the shipped
 * default executable path became empty on 2026-08-27 (it used to name one
 * game), and the function it lives in used to start with
 * `if (configured.empty()) return configured;` -- which would have made "Run"
 * silently do nothing for every user.  Nothing tested this path at all before
 * this file existed.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_run_manager_internal.h"

#include <set>
#include <string>

namespace {

/* A fake filesystem: a set of paths that "exist". */
bool fake_exists(const std::string &p, void *user)
{
    const auto *fs = static_cast<const std::set<std::string> *>(user);
    return fs->find(p) != fs->end();
}

JceRunResolveInputs base_inputs()
{
    JceRunResolveInputs in;
    in.exe_suffix  = ".exe";
    in.parents     = { ".", "..", "../.." };
    in.output_dirs = { "build/desktop/windows-x64/release",
                       "build/desktop/windows-x64/debug" };
    return in;
}

std::string run(const JceRunResolveInputs &in, std::set<std::string> fs)
{
    return jce_run_manager_resolve_path(in, fake_exists, &fs);
}

} /* namespace */

TEST_CASE("configured path that exists is returned as-is")
{
    auto in = base_inputs();
    in.configured = "build/x/game.exe";
    CHECK(run(in, { "build/x/game.exe" }) == "build/x/game.exe");
}

TEST_CASE("configured path gains the platform exe suffix")
{
    auto in = base_inputs();
    in.configured = "build/x/game";
    CHECK(run(in, { "build/x/game.exe" }) == "build/x/game.exe");
}

TEST_CASE("the exe suffix is never doubled")
{
    auto in = base_inputs();
    in.configured = "build/x/game.exe";
    /* Only the doubled form exists — it must NOT be found. */
    CHECK(run(in, { "build/x/game.exe.exe" }) == "build/x/game.exe");
}

TEST_CASE("a relative configured path is tried under each parent")
{
    auto in = base_inputs();
    in.configured = "out/game.exe";
    CHECK(run(in, { "../out/game.exe" }) == "../out/game.exe");
}

TEST_CASE("an absolute configured path is never re-rooted under a parent")
{
    auto in = base_inputs();
    in.configured = "/abs/out/game.exe";
    /* The parent-prefixed form exists; the absolute one does not.  Prefixing
     * an absolute path would be nonsense, so the answer is "not found". */
    CHECK(run(in, { ".//abs/out/game.exe" }) == "/abs/out/game.exe");

    in.configured = "C:/abs/out/game.exe";
    CHECK(run(in, { "./C:/abs/out/game.exe" }) == "C:/abs/out/game.exe");
}

TEST_CASE("EMPTY_CONFIGURED_STILL_RESOLVES via the project's target name")
{
    auto in = base_inputs();
    in.configured  = "";                 /* the shipped default */
    in.target_name = "MyGame";
    CHECK(run(in, { "../build/desktop/windows-x64/release/MyGame.exe" }) ==
          "../build/desktop/windows-x64/release/MyGame.exe");
}

TEST_CASE("empty configured AND empty target resolves to nothing")
{
    auto in = base_inputs();
    in.configured  = "";
    in.target_name = "";
    /* A file that WOULD match if the editor invented a name is present; with
     * no target the well-known dirs must not be probed at all. */
    CHECK(run(in, { "./build/desktop/windows-x64/release/SomeGame.exe" }) == "");
}

TEST_CASE("the configured build output dir wins over the well-known dirs")
{
    auto in = base_inputs();
    in.configured        = "";
    in.target_name       = "MyGame";
    in.build_output_path = "custom/out";
    std::set<std::string> fs = {
        "custom/out/MyGame.exe",
        "./build/desktop/windows-x64/release/MyGame.exe",
    };
    CHECK(run(in, fs) == "custom/out/MyGame.exe");
}

TEST_CASE("output dirs are tried in order, parents inside each")
{
    auto in = base_inputs();
    in.configured  = "";
    in.target_name = "MyGame";
    /* Only the SECOND output dir has it, under the third parent. */
    CHECK(run(in, { "../../build/desktop/windows-x64/debug/MyGame.exe" }) ==
          "../../build/desktop/windows-x64/debug/MyGame.exe");
}

TEST_CASE("nothing found returns the configured string for a diagnostic")
{
    auto in = base_inputs();
    in.configured  = "build/x/game.exe";
    in.target_name = "MyGame";
    CHECK(run(in, {}) == "build/x/game.exe");
}

TEST_CASE("a platform with no exe suffix does not append one")
{
    auto in = base_inputs();
    in.exe_suffix  = "";
    in.configured  = "";
    in.target_name = "MyGame";
    CHECK(run(in, { "./build/desktop/windows-x64/release/MyGame" }) ==
          "./build/desktop/windows-x64/release/MyGame");
}
