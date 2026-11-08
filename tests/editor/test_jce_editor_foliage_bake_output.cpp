#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "panels/jce_editor_foliage_bake_output.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <cstring>
#include <string>

TEST_CASE("new foliage output does not require an existing readable asset")
{
    char key[256], host[1024];
    REQUIRE(jce_editor_foliage_bake_paths("D:/projects/game", "content/source",
        false, 9007199254741001ull, 1337u, key, sizeof key, host, sizeof host));
    CHECK(std::string(key) ==
        "generated/foliage/vegetation_9007199254741001_1337.foliage.bin");
    CHECK(std::string(host) == "D:/projects/game/content/source/" + std::string(key));
}

TEST_CASE("foliage output roots remain within the declared project")
{
    const char *invalid[] = {"../outside", "assets/../../outside", "/outside",
        "D:/outside", "C:outside", "..\\outside"};
    for (const char *source : invalid) {
        char key[256] = "old", host[1024] = "old";
        CHECK_FALSE(jce_editor_foliage_bake_paths("D:/projects/game", source,
            false, 1u, 2u, key, sizeof key, host, sizeof host));
        CHECK(key[0] == '\0');
        CHECK(host[0] == '\0');
    }
}

TEST_CASE("source root normalization preserves safe interior paths")
{
    char key[256], host[1024];
    REQUIRE(jce_editor_foliage_bake_paths("/projects/game", "content/../assets",
        false, 5u, 0u, key, sizeof key, host, sizeof host));
    CHECK(std::string(host) == "/projects/game/assets/" + std::string(key));
}

TEST_CASE("isolated bundle never writes a stale source project")
{
    char key[256] = "old", host[1024] = "old";
    CHECK_FALSE(jce_editor_foliage_bake_paths("D:/previous/game", "assets",
        true, 1u, 2u, key, sizeof key, host, sizeof host));
    CHECK(key[0] == '\0');
    CHECK(host[0] == '\0');
}

TEST_CASE("missing and relative project context cannot redirect to CWD")
{
    const char *invalid[] = {nullptr, "", "project", "D:project"};
    for (const char *project : invalid) {
        char key[256], host[1024];
        CHECK_FALSE(jce_editor_foliage_bake_paths(project, "assets", false,
            1u, 2u, key, sizeof key, host, sizeof host));
    }
}

TEST_CASE("short output buffers fail without partial published keys")
{
    char key[4] = "old", host[1024] = "old";
    CHECK_FALSE(jce_editor_foliage_bake_paths("D:/projects/game", "assets",
        false, 1u, 2u, key, sizeof key, host, sizeof host));
    CHECK(key[0] == '\0');
    CHECK(host[0] == '\0');
}

struct FoliageOutputFixture {
    char root[1024] = {};
    FoliageOutputFixture()
    {
        char cwd[1024];
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
        REQUIRE(jce_path_join(root, sizeof root, cwd, "_ut_foliage_bake_output"));
        REQUIRE(jce_fs_host_create_directory(root));
    }
    ~FoliageOutputFixture() { jce_fs_host_remove_recursive(root); }
};

TEST_CASE_FIXTURE(FoliageOutputFixture, "first bake creates its missing parents and can be read back")
{
    const unsigned char payload[] = {0x13u, 0x00u, 0x7fu, 0xffu};
    char key[256], host[1024];
    REQUIRE(jce_editor_foliage_bake_paths(root, "new/source", false,
        1u, 2u, key, sizeof key, host, sizeof host));
    CHECK_FALSE(jce_fs_host_exists_file(host));
    const char *reason = nullptr;
    REQUIRE(jce_editor_foliage_bake_write(root, "new/source", false, 1u, 2u,
        payload, sizeof payload, key, sizeof key, &reason));
    CHECK(std::string(reason) == "written_pending_render_validation");
    uint64_t size = 0u;
    void *read = jce_fs_host_read_all(host, &size);
    REQUIRE(read != nullptr);
    CHECK(size == sizeof payload);
    CHECK(std::memcmp(read, payload, sizeof payload) == 0);
    jce_fs_buffer_free(read);
}

TEST_CASE_FIXTURE(FoliageOutputFixture, "failed parent creation publishes no asset key")
{
    const char blocked[] = "ordinary file";
    char blocker[1024];
    REQUIRE(jce_path_join(blocker, sizeof blocker, root, "blocked"));
    REQUIRE(jce_fs_host_write_all(blocker, blocked, sizeof blocked));
    char key[256] = "old";
    const char *reason = nullptr;
    CHECK_FALSE(jce_editor_foliage_bake_write(root, "blocked/assets", false,
        1u, 2u, blocked, sizeof blocked, key, sizeof key, &reason));
    CHECK(key[0] == '\0');
    CHECK(std::string(reason) == "create_parent_failed");
    uint64_t size = 0u;
    void *read = jce_fs_host_read_all(blocker, &size);
    REQUIRE(read != nullptr);
    CHECK(size == sizeof blocked);
    CHECK(std::memcmp(read, blocked, sizeof blocked) == 0);
    jce_fs_buffer_free(read);
}
