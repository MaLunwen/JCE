#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#undef jce_fs_host_read_capped
#undef jce_fs_buffer_free
#include "core/jce_project_build_target.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>


namespace {
unsigned capped_reads = 0, capped_frees = 0;
uint64_t capped_limit = 0;
bool capped_grown = false, capped_failure = false;
struct CappedControl {
    CappedControl() { capped_reads = capped_frees = 0; capped_limit = 0; }
    ~CappedControl() { capped_grown = capped_failure = false; }
};
}
extern "C" void *JCE_CALL jce_test_sdk_read_capped(const char *path, uint64_t maximum,
                                                 uint64_t *read, uint64_t *total)
{
    ++capped_reads;
    capped_limit = maximum;
    CHECK(maximum == 16385);
    if (capped_failure) return nullptr;
    void *result = jce_fs_host_read_capped(path, maximum, read, total);
    if (result && capped_grown) *total = UINT64_C(1) << 32;
    return result;
}
extern "C" void JCE_CALL jce_test_sdk_buffer_free(void *buffer)
{
    if (buffer) ++capped_frees;
    jce_fs_buffer_free(buffer);
}

TEST_CASE("project default architecture follows the actual host")
{
    CHECK(std::strcmp(jce_project_build_arch(nullptr, "aarch64"), "aarch64") == 0);
    CHECK(std::strcmp(jce_project_build_arch("", "i686"), "i686") == 0);
    CHECK(jce_project_build_arch(nullptr, nullptr) == nullptr);
}

TEST_CASE("explicit native architecture aliases are canonical")
{
    CHECK(std::strcmp(jce_project_build_arch("x64", "aarch64"), "x86_64") == 0);
    CHECK(std::strcmp(jce_project_build_arch("x86", "x86_64"), "i686") == 0);
    CHECK(std::strcmp(jce_project_build_arch("arm64", "x86_64"), "aarch64") == 0);
    CHECK(jce_project_build_arch("wasm32", "x86_64") == nullptr);
    CHECK(jce_project_build_arch("invalid", "x86_64") == nullptr);
}

TEST_CASE("Windows ARM32 cannot silently use the x64 compiler")
{
    CHECK(jce_project_build_target_error("armv7", "x86_64", JceProjectBuildPlatform::Windows) != nullptr);
    CHECK(jce_project_build_target_error("aarch64", "x86_64", JceProjectBuildPlatform::Windows) == nullptr);
    CHECK(jce_project_build_target_error("i686", "x86_64", JceProjectBuildPlatform::Windows) == nullptr);
}

TEST_CASE("Linux cross architecture needs an explicit toolchain")
{
    CHECK(jce_project_build_target_error("aarch64", "x86_64", JceProjectBuildPlatform::Linux) != nullptr);
    CHECK(jce_project_build_target_error("aarch64", "aarch64", JceProjectBuildPlatform::Linux) == nullptr);
    CHECK(jce_project_build_target_error("x86_64", nullptr, JceProjectBuildPlatform::Linux) != nullptr);
}

TEST_CASE("macOS native targets match supported compiler architectures")
{
    CHECK(jce_project_build_target_error("i686", "x86_64", JceProjectBuildPlatform::MacOS) != nullptr);
    CHECK(jce_project_build_target_error("aarch64", "x86_64", JceProjectBuildPlatform::MacOS) == nullptr);
    CHECK(jce_project_build_target_error(nullptr, "x86_64", JceProjectBuildPlatform::MacOS) != nullptr);
}

// Real host-file admission controls. Archive bytes are a presence fixture;
// these tests do not certify compiler ABI, cross-platform linkage or GUI use.
namespace {
struct SdkFiles {
    std::string base, root, sdk;
    SdkFiles()
    {
        char cwd[1024] = {}, normalized[1024] = {}, unique[1024] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof(cwd)));
        REQUIRE(jce_path_normalize(normalized, sizeof(normalized), cwd));
        base = normalized;
        REQUIRE(jce_fs_host_make_unique_path((base + "/_ut_sdk_target").c_str(), unique, sizeof(unique)));
        root = unique;
        REQUIRE(jce_fs_host_create_directory(root.c_str()));
        sdk = root + "/arbitrary-renamed-sdk";
        REQUIRE(jce_fs_host_create_directory((sdk + "/lib/Release").c_str()));
        REQUIRE(jce_fs_host_create_directory((sdk + "/lib/Debug").c_str()));
    }
    ~SdkFiles()
    {
        const bool owned = root.rfind(base + "/_ut_sdk_target", 0) == 0 &&
                           root.find('/', base.size() + 1) == std::string::npos;
        CHECK(owned);
        if (owned) CHECK(jce_fs_host_remove_recursive(root.c_str()));
    }
    void write(const std::string &file, const std::string &data)
    {
        REQUIRE(jce_fs_host_write_all(file.c_str(), data.data(), data.size()));
    }
    void metadata(const std::string &text, const char *config = "Release")
    {
        write(sdk + "/lib/" + config + "/jce-sdk-target.json", text);
    }
    void archive(const char *config = "Release", const char *name = "jce_engine_core.lib")
    {
        write(sdk + "/lib/" + config + "/" + name, "PRESENCE_ONLY");
    }
    std::string error(const char *arch = "x86_64", const char *variant = "dist",
                      JceProjectBuildPlatform platform = JceProjectBuildPlatform::Windows)
    {
        return jce_project_build_sdk_target_error(sdk, arch, platform, variant);
    }
};
std::string sdk_metadata(const char *config = "Release", const char *variant = "dist",
                         const char *delivery = "embedded", const char *crt = "MultiThreaded")
{
    return std::string("{\"schema\":1,\"system\":\"Windows\",\"arch\":\"x86_64\",\"pointer_bits\":64,") +
        "\"config\":\"" + config + "\",\"variant\":\"" + variant + "\",\"delivery\":\"" + delivery +
        "\",\"crt\":\"" + crt + "\",\"archive\":\"jce_engine_core.lib\"}";
}
std::string replace(std::string text, const std::string &old, const std::string &value)
{
    const auto pos = text.find(old);
    REQUIRE(pos != std::string::npos);
    text.replace(pos, old.size(), value);
    return text;
}
}

TEST_CASE("SDK admission follows metadata after root relocation")
{
    SdkFiles files;
    files.metadata(sdk_metadata()); files.archive();
    files.write(files.sdk + "/VERSION.txt", "variant: release\nhost: anything\n");
    CHECK(files.error().empty()); // Old explanatory stamp cannot override the archive.
    const auto moved = files.root + "/sdk-with-another-name";
    REQUIRE(jce_fs_host_rename(files.sdk.c_str(), moved.c_str()));
    files.sdk = moved;
    CHECK(files.error().empty());
}

TEST_CASE("SDK missing metadata and archive fail closed with refresh guidance")
{
    SdkFiles files;
    CHECK(files.error().find("refresh") != std::string::npos);
    files.metadata(sdk_metadata());
    CHECK(files.error().find("archive missing") != std::string::npos);
    files.archive(); CHECK(files.error().empty());
}

TEST_CASE("SDK x64 archive cannot admit ARM64 or another target system")
{
    SdkFiles files; files.metadata(sdk_metadata()); files.archive();
    CHECK(files.error("aarch64").find("arch mismatch") != std::string::npos);
    CHECK(files.error("x86_64", "dist", JceProjectBuildPlatform::Linux).find("system mismatch") != std::string::npos);
    CHECK(files.error("x64").find("Unsupported") != std::string::npos);
}

TEST_CASE("SDK configurations do not borrow sibling metadata")
{
    SdkFiles files; files.metadata(sdk_metadata()); files.archive();
    files.archive("Debug");
    CHECK(files.error("x86_64", "debug").find("Missing") != std::string::npos);
    files.metadata(sdk_metadata("Debug", "debug", "authoring", "MultiThreadedDebugDLL"), "Debug");
    CHECK(files.error("x86_64", "debug").empty());
    CHECK(files.error("x86_64", "release").find("variant mismatch") != std::string::npos);
}

TEST_CASE("SDK malformed duplicate unknown and oversized metadata is rejected")
{
    SdkFiles files; files.archive();
    for (const auto &text : {std::string("{}"), std::string("null"), sdk_metadata() + " garbage",
            replace(sdk_metadata(), "{", "{\"schema\":1,"),
            replace(sdk_metadata(), "{", "{\"unknown\":1,"),
            replace(sdk_metadata(), "\"schema\":1", "\"schema\":true"),
            replace(sdk_metadata(), "\"arch\":\"x86_64\"", "\"arch\":\"aarch64\",\"ar\\u0063h\":\"x86_64\""),
            replace(sdk_metadata(), "MultiThreaded", "MultiThreaded\\u0000DLL"),
            std::string(16385, ' ')}) {
        files.metadata(text);
        CHECK_FALSE(files.error().empty());
    }
}

TEST_CASE("SDK CRT and delivery policy cannot silently change shipping requirements")
{
    SdkFiles files; files.archive();
    files.metadata(sdk_metadata("Release", "dist", "embedded", "MultiThreadedDLL"));
    CHECK(files.error().find("CRT incompatible") != std::string::npos);
    files.metadata(sdk_metadata("Release", "dist", "authoring", "MultiThreadedDLL"));
    CHECK(files.error().find("delivery mismatch") != std::string::npos);
    files.metadata(sdk_metadata("Release", "dist", "embedded", "MultiThreadedDebug"));
    CHECK(files.error().find("CRT incompatible") != std::string::npos);
}

TEST_CASE("SDK pointer size and archive names must match exact selected owner")
{
    SdkFiles files; files.archive();
    files.metadata(replace(sdk_metadata(), "pointer_bits\":64", "pointer_bits\":32"));
    CHECK(files.error().find("pointer size") != std::string::npos);
    files.metadata(replace(sdk_metadata(), "jce_engine_core.lib", "../jce_engine_core.lib"));
    CHECK(files.error().find("archive name invalid") != std::string::npos);
}

TEST_CASE("SDK exact reader caps growth and releases each host buffer once")
{
    SdkFiles files; files.metadata(sdk_metadata()); files.archive();
    CappedControl control;
    CHECK(files.error().empty());
    CHECK(capped_reads == 1); CHECK(capped_frees == 1); CHECK(capped_limit == 16385);
    // Model replacement/growth observed by the capped host stream. Allocation
    // still uses its declared cap; the production admission must reject total.
    capped_grown = true;
    CHECK(files.error().find("16 KiB") != std::string::npos);
    CHECK(capped_reads == 2); CHECK(capped_frees == 2);
    capped_grown = false; capped_failure = true;
    CHECK_FALSE(files.error().empty());
    CHECK(capped_reads == 3); CHECK(capped_frees == 2);
}


TEST_CASE("user manifest target cannot inherit another workspace target")
{
    CHECK(std::string(jce_project_build_initial_target(false, "TowerDefense", "TD", "OtherGame")) == "TowerDefense");
    CHECK(std::string(jce_project_build_initial_target(false, "", "CurrentProject", "OtherGame")) == "CurrentProject");
    CHECK(std::string(jce_project_build_initial_target(true, "EngineTarget", "Engine", "WorkspaceOverride")) == "WorkspaceOverride");
    CHECK(std::string(jce_project_build_initial_target(true, "EngineTarget", "Engine", "")) == "EngineTarget");
}
TEST_CASE("SDK-native tools do not require the workspace dependency manager")
{
    CHECK(jce_project_build_tools_ready(true, true, true, false));
    CHECK_FALSE(jce_project_build_tools_ready(false, true, true, false));
    CHECK_FALSE(jce_project_build_tools_ready(true, false, true, true));
    CHECK_FALSE(jce_project_build_tools_ready(true, true, false, true));
    CHECK(jce_project_build_tools_ready(false, true, true, true));
}
TEST_CASE("project layout and edited artifact name follow the selected variant")
{
    const auto root = jce_project_build_root("/project relocated", JceProjectBuildPlatform::Windows, "aarch64");
    CHECK(root == "/project relocated/build/win32-aarch64");
    CHECK(jce_project_build_output_dir(root, jce_project_build_variant(true)) == root + "/dist");
    CHECK(jce_project_build_output_dir(root, jce_project_build_variant(false)) == root + "/release");
    CHECK(jce_project_build_root("", JceProjectBuildPlatform::Linux, "x86_64") == "build/linux-x86_64");
    CHECK(jce_project_build_root("/project/", JceProjectBuildPlatform::MacOS, "aarch64") == "/project/build/darwin-aarch64");
    CHECK(std::string(jce_project_build_package_platform(JceProjectBuildPlatform::Windows)) == "windows");
    CHECK(std::string(jce_project_build_package_platform(JceProjectBuildPlatform::MacOS)) == "macos");
    CHECK(jce_project_build_exe_name("EditedTarget", "EditedOutput.EXE") == "EditedOutput.EXE");
#if JCE_PLATFORM_WINDOWS
    CHECK(jce_project_build_exe_name("EditedTarget", nullptr) == "EditedTarget.exe");
#else
    CHECK(jce_project_build_exe_name("EditedTarget", nullptr) == "EditedTarget");
#endif
}
