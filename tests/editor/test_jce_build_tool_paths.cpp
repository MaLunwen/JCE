#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#undef jce_toolchain_get
#undef jce_host_resolve_executable
#undef jce_fs_host_exists_file
#include "core/jce_build_tool_paths.h"
#include <jce/os/core/jce_toolchain.h>
#include <cstring>
#include <string>

namespace {
struct Ports {
    JceToolchain cmake{}, ninja{};
    std::string present_file, path_result;
    unsigned snapshots = 0, path_reads = 0, existence_reads = 0;
} ports;
struct Reset {
    Reset() { ports = {}; }
};
std::string executable(const char *directory, const char *tool)
{
    std::string path = std::string(directory) + "/" + tool;
#if JCE_PLATFORM_WINDOWS
    path += ".exe";
#endif
    return path;
}
}
extern "C" const JceToolchain *jce_test_build_toolchain_get(JceToolchainKind kind)
{
    ++ports.snapshots;
    return kind == JCE_TOOLCHAIN_CMAKE ? &ports.cmake :
        (kind == JCE_TOOLCHAIN_NINJA ? &ports.ninja : nullptr);
}
extern "C" bool jce_test_build_resolve_executable(const char *, char *dst, size_t cap)
{
    ++ports.path_reads;
    if (ports.path_result.empty() || ports.path_result.size() >= cap) return false;
    std::memcpy(dst, ports.path_result.c_str(), ports.path_result.size() + 1);
    return true;
}
extern "C" bool jce_test_build_exists_file(const char *path)
{
    ++ports.existence_reads;
    return ports.present_file == path;
}

TEST_CASE("public toolchain override wins over PATH")
{
    Reset reset;
    ports.cmake.present = ports.cmake.from_override = true;
    std::strcpy(ports.cmake.path, "/builder with spaces/bin");
    ports.present_file = executable(ports.cmake.path, "cmake");
    ports.path_result = "/another/cmake";
    CHECK(jce_build_tool_executable("cmake") == ports.present_file);
    CHECK(ports.path_reads == 0);
}
TEST_CASE("auto-discovered tool directory works without PATH")
{
    Reset reset;
    ports.ninja.present = true;
    std::strcpy(ports.ninja.path, "/visual studio/tools");
    ports.present_file = executable(ports.ninja.path, "ninja");
    CHECK(jce_build_tool_executable("ninja") == ports.present_file);
    CHECK(ports.path_reads == 0);
}
TEST_CASE("stale tool snapshot falls back to the host resolver")
{
    Reset reset;
    ports.cmake.present = true;
    std::strcpy(ports.cmake.path, "/removed-tool");
    ports.path_result = "/active/cmake";
    CHECK(jce_build_tool_executable("cmake") == ports.path_result);
    CHECK(ports.existence_reads == 1);
    CHECK(ports.path_reads == 1);
}
TEST_CASE("missing tools refuse an implicit bare executable")
{
    Reset reset;
    CHECK(jce_build_tool_executable("cmake").empty());
    CHECK(ports.path_reads == 1);
    CHECK(jce_build_tool_executable(nullptr).empty());
    CHECK(jce_build_tool_executable("").empty());
    CHECK(ports.path_reads == 1);
}
TEST_CASE("captured executable paths survive a later working-directory change")
{
    Reset reset;
    const std::string full = "/captured builder/cmake";
    CHECK(jce_build_tool_executable(full.c_str()) == full);
    CHECK(ports.snapshots == 0);
    CHECK(ports.path_reads == 0);
    CHECK(jce_build_tool_executable("C:\\builder\\cmake.exe") == "C:\\builder\\cmake.exe");
}
TEST_CASE("workspace-only tools still use the shared host resolver")
{
    Reset reset;
    ports.path_result = "/workspace/conan";
    CHECK(jce_build_tool_executable("conan") == ports.path_result);
    CHECK(ports.snapshots == 0);
    CHECK(ports.path_reads == 1);
}
