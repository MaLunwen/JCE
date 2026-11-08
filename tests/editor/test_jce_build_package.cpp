/* Exact production package owner, real disk operations. Audit callbacks are
 * controlled admission boundaries; these tests do not certify PE GUI/imports,
 * relocation of a real game, symlink aliases or a graphics/audio runtime. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_build_package.h"
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <string>

namespace {
std::string read(const std::string &path)
{
    uint64_t size = 0;
    auto *bytes = static_cast<char *>(jce_fs_host_read_all(path.c_str(), &size));
    REQUIRE(bytes != nullptr);
    std::string result(bytes, (size_t)size);
    jce_fs_buffer_free(bytes);
    return result;
}
void write(const std::string &path, const std::string &bytes)
{
    REQUIRE(jce_fs_host_write_all(path.c_str(), bytes.data(), bytes.size()));
}
bool count_entry(const char *, bool, void *user)
{
    ++*static_cast<unsigned *>(user);
    return true;
}
unsigned entries(const std::string &path)
{
    unsigned count = 0;
    REQUIRE(jce_fs_host_list_dir(path.c_str(), count_entry, &count));
    return count;
}
struct Files {
    std::string base, root, project, sdk, reports, built, out;
    Files()
    {
        char cwd[1024] = {}, normalized[1024] = {}, unique[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        REQUIRE(jce_path_normalize(normalized, sizeof(normalized), cwd));
        base = normalized;
        REQUIRE(jce_fs_host_make_unique_path((base + "/_ut_build_package").c_str(),
                                            unique, sizeof(unique)));
        root = unique;
        REQUIRE_FALSE(jce_fs_host_exists_dir(root.c_str()));
        REQUIRE_FALSE(jce_fs_host_exists_file(root.c_str()));
        REQUIRE(jce_fs_host_create_directory(root.c_str()));
        project = root + "/project";
        sdk = root + "/sdk";
        reports = project + "/build/platform/reports";
        built = project + "/build/platform/out/game.exe";
        out = project + "/dist/apps/game/platform-native-dist";
        REQUIRE(jce_fs_host_create_directory(sdk.c_str()));
        REQUIRE(jce_fs_host_create_directory((project + "/assets").c_str()));
        REQUIRE(jce_fs_host_create_directory(reports.c_str()));
        write(built, "NEW_BINARY");
    }
    ~Files()
    {
        // Resolve the fixture root before recursive cleanup; only the uniquely
        // created child of this test's absolute CWD is ever removed.
        const bool owned = !base.empty() && root.rfind(base + "/_ut_build_package", 0) == 0 &&
                           root.find('/', base.size() + 1) == std::string::npos;
        CHECK(owned);
        if (owned) CHECK(jce_fs_host_remove_recursive(root.c_str()));
    }
    std::string old() const { return out + "/game.exe"; }
    void keep_old() { write(old(), "OLD_COMPLETE"); }
};
struct Audit {
    Files *files;
    unsigned calls = 0;
    unsigned reject_call = 0;
    bool remove_candidate = false;
    bool introduce_unknown = false;
    std::string expected = "NEW_BINARY";
    std::string candidate;
};
bool audit(const std::string &exe, const char *directory, void *user)
{
    auto &a = *static_cast<Audit *>(user);
    ++a.calls;
    CHECK(read(exe) == a.expected);
    if (directory) {
        a.candidate = directory;
        CHECK(entries(directory) == 1);
        if (a.introduce_unknown) write(a.files->out + "/user-notes.txt", "KEEP_ME");
        if (a.remove_candidate) REQUIRE(jce_fs_host_remove_file(exe.c_str()));
    } else CHECK(exe == a.files->built);
    return a.calls != a.reject_call;
}
bool publish(Files &f, Audit &a, std::string *error = nullptr)
{
    return jce_build_package_publish_dist(f.built, f.reports, f.out,
                                         "game.exe", audit, &a, error);
}
} // namespace

TEST_CASE("default application layout shares portable Unicode filename admission")
{
    Files f;
    std::string path, error;
    REQUIRE(jce_build_package_default_path(f.project, "塔防 Game", "future-platform",
                "future-arch", "dist", &path, &error));
    CHECK(path == f.project + "/dist/apps/塔防 Game/future-platform-future-arch-dist");
    for (const std::string name : {"../escape", "x/y", "x\\y", "CON.exe", "LPT1",
                                   "trailing.", "trailing ", "x..y", "a:b", ""}) {
        CHECK_FALSE(jce_build_package_default_path(f.project, name, "platform",
                                                   "native", "dist", &path, &error));
        CHECK(path.empty());
        CHECK_FALSE(error.empty());
    }
    CHECK_FALSE(jce_build_package_component_valid(std::string("bad\nname")));
    CHECK_FALSE(jce_build_package_component_valid(std::string("bad\0name", 8)));
    CHECK(jce_build_package_component_valid("游戏.exe"));
}
TEST_CASE("package admission rejects project source build SDK roots before writing")
{
    Files f;
    std::string path, error;
    for (const auto &unsafe : {f.project, f.root, f.sdk, f.sdk + "/apps",
                              f.project + "/build/out", f.project + "/assets/app",
                              f.project + "/resources/app", f.project + "/src/app",
                              f.project + "/.jce/app"}) {
        CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                           unsafe, "game.exe", true, &path, &error));
        CHECK(path.empty());
        CHECK_FALSE(error.empty());
    }
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                      "relative/out", "game.exe", true, &path, &error));
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                      f.out, "../game.exe", true, &path, &error));
    REQUIRE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                   f.out, "game.exe", true, &path, &error));
    CHECK(path == f.out);
    CHECK_FALSE(jce_fs_host_exists_dir(f.out.c_str()));
}
TEST_CASE("normalization and component boundaries cannot alias lexical build roots")
{
    Files f;
    std::string path, error;
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                f.project + "/dist/../build/apps", "game.exe", true, &path, &error));
    REQUIRE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                f.project + "/build-sibling/app/./dist", "game.exe", true, &path, &error));
    CHECK(path == f.project + "/build-sibling/app/dist");
#if JCE_PLATFORM_WINDOWS
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       "C:relative", "game.exe", true, &path, &error));
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       "C:/", "game.exe", true, &path, &error));
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       "//server/share", "game.exe", true, &path, &error));
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       "//?/C:/app", "game.exe", true, &path, &error));
#else
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       "/", "game.exe", true, &path, &error));
#endif
}
TEST_CASE("unknown files and authoring old packages fail closed without removal")
{
    Files f;
    std::string path, error;
    f.keep_old();
    REQUIRE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                    f.out, "game.exe", true, &path, &error));
    CHECK_FALSE(jce_build_package_admit(f.project, f.sdk, "resources/_cooked",
                                       f.out, "game.exe", false, &path, &error));
    write(f.out + "/user-notes.txt", "KEEP_ME");
    Audit a{&f};
    CHECK_FALSE(publish(f, a, &error));
    CHECK(a.calls == 0);
    CHECK(read(f.old()) == "OLD_COMPLETE");
    CHECK(read(f.out + "/user-notes.txt") == "KEEP_ME");
    CHECK(error.find("unowned") != std::string::npos);
}
TEST_CASE("output subdirectories and basename directories never pass single-file admission")
{
    Files f;
    REQUIRE(jce_fs_host_create_directory((f.out + "/game.exe").c_str()));
    std::string error;
    CHECK_FALSE(jce_build_package_check_directory(f.out, "game.exe", true, &error));
    CHECK(jce_fs_host_exists_dir((f.out + "/game.exe").c_str()));
    write(f.root + "/file-output", "KEEP_FILE");
    CHECK_FALSE(jce_build_package_check_directory(f.root + "/file-output",
                                                 "game.exe", true, &error));
    CHECK(read(f.root + "/file-output") == "KEEP_FILE");
}
TEST_CASE("built artifact rejection leaves old complete application untouched")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    a.reject_call = 1;
    CHECK_FALSE(publish(f, a));
    CHECK(a.calls == 1);
    CHECK(a.candidate.empty());
    CHECK(read(f.old()) == "OLD_COMPLETE");
}
TEST_CASE("candidate rejection removes only the owned candidate and retains old application")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    a.reject_call = 2;
    CHECK_FALSE(publish(f, a));
    CHECK(a.calls == 2);
    CHECK(read(f.old()) == "OLD_COMPLETE");
    CHECK_FALSE(jce_fs_host_exists_file((a.candidate + "/game.exe").c_str()));
    CHECK(entries(a.candidate) == 0);
}
TEST_CASE("actual rename failure preserves the previous complete application")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    // Fault injection at the audit seam removes the new candidate on disk;
    // the real host rename fails, rather than a mocked replacement result.
    a.remove_candidate = true;
    std::string error;
    CHECK_FALSE(publish(f, a, &error));
    CHECK(a.calls == 2);
    CHECK(read(f.old()) == "OLD_COMPLETE");
    CHECK(error.find("old output kept") != std::string::npos);
    CHECK(entries(f.out) == 1);
}
TEST_CASE("new unknown final files during audit prevent replacement and are kept")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    a.introduce_unknown = true;
    CHECK_FALSE(publish(f, a));
    CHECK(a.calls == 2);
    CHECK(read(f.old()) == "OLD_COMPLETE");
    CHECK(read(f.out + "/user-notes.txt") == "KEEP_ME");
}
TEST_CASE("successful publication replaces exact bytes and reuses bounded reports staging")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    REQUIRE(publish(f, a));
    CHECK(a.calls == 2);
    CHECK(read(f.old()) == "NEW_BINARY");
    CHECK(entries(f.out) == 1);
    CHECK(entries(a.candidate) == 0);
    const auto candidate = a.candidate;
    write(f.built, "NEXT_BINARY");
    Audit again{&f};
    again.expected = "NEXT_BINARY";
    REQUIRE(publish(f, again));
    CHECK(read(f.old()) == "NEXT_BINARY");
    CHECK(again.candidate == candidate);
    CHECK(entries(f.reports) == 1);
    CHECK(entries(f.out) == 1);
}
TEST_CASE("reports overlap and missing built artifact fail before touching final contents")
{
    Files f;
    f.keep_old();
    Audit a{&f};
    CHECK_FALSE(jce_build_package_publish_dist(f.built, f.out, f.out, "game.exe",
                                               audit, &a));
    REQUIRE(jce_fs_host_remove_file(f.built.c_str()));
    CHECK_FALSE(publish(f, a));
    CHECK(a.calls == 0);
    CHECK(read(f.old()) == "OLD_COMPLETE");
}
TEST_CASE("candidate unknown entries and failed reports creation never delete old output")
{
    Files f;
    f.keep_old();
    const auto candidate = f.reports + "/package-candidate-game.exe";
    write(candidate + "/user-diagnostic.txt", "KEEP_DIAGNOSTIC");
    Audit a{&f};
    CHECK_FALSE(publish(f, a));
    CHECK(a.calls == 1);
    CHECK(read(candidate + "/user-diagnostic.txt") == "KEEP_DIAGNOSTIC");
    CHECK(read(f.old()) == "OLD_COMPLETE");
    write(f.root + "/blocked-reports", "KEEP_REPORTS_FILE");
    CHECK_FALSE(jce_build_package_publish_dist(f.built, f.root + "/blocked-reports",
                                               f.out, "game.exe", audit, &a));
    CHECK(read(f.root + "/blocked-reports") == "KEEP_REPORTS_FILE");
    CHECK(read(f.old()) == "OLD_COMPLETE");
}
