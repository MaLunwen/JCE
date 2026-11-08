/* Exact Play mixer descriptor segment, with typed public presence/profile
 * ports. This does not certify actual native file access, Runtime playback,
 * content-Bundle closure or physical Editor UI. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/application/jce_runtime.h>
#include <jce/os/core/jce_filesystem.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static char s_current_project_root[512];
static const char *primary_path = "D:/fixture-project/Settings/audio_mixer.json";
static const char *fallback_path = "D:/fixture-profile/audio_mixer.json";
static JceFsPresence primary_presence, fallback_presence;
static bool profile_success;
static unsigned profile_queries;
static std::vector<std::string> probes;

static JceFsPresence JCE_CALL play_mixer_probe_port(const char *path)
{
    REQUIRE(path != nullptr);
    probes.emplace_back(path);
    if (std::strcmp(path, primary_path) == 0) return primary_presence;
    CHECK(std::strcmp(path, fallback_path) == 0);
    return fallback_presence;
}
static bool play_mixer_profile_port(const char *name, char *out, size_t cap)
{
    ++profile_queries;
    CHECK(std::strcmp(name, "audio_mixer.json") == 0);
    /* Failure can leave nonempty storage; callers must consume the result. */
    std::snprintf(out, cap, "%s", fallback_path);
    return profile_success;
}
static const char *actual_play_mixer_descriptor()
{
    JceRuntimeDesc rd{}; /* Actual Play memset initializes this field to NULL. */
#define jce_fs_probe_read play_mixer_probe_port
#define jce_editor_dotjce_path play_mixer_profile_port
#include JCE_EDITOR_PLAY_MIXER_WIRE
#undef jce_editor_dotjce_path
#undef jce_fs_probe_read
    return rd.mixer_config_path;
}
static void reset_ports(bool project = true)
{
    std::snprintf(s_current_project_root, sizeof(s_current_project_root), "%s",
                  project ? "D:/fixture-project" : "");
    primary_presence = fallback_presence = JCE_FS_PRESENCE_ABSENT;
    profile_success = true;
    profile_queries = 0;
    probes.clear();
}
static void expect_path(const char *expected)
{
    const char *selected = actual_play_mixer_descriptor();
    CHECK(selected != nullptr);
    if (selected) CHECK(std::string(selected) == expected);
}

TEST_CASE("authored project mixer wins without legacy lookup")
{
    reset_ports(); primary_presence = JCE_FS_PRESENCE_PRESENT;
    expect_path(primary_path);
    CHECK(probes.size() == 1); CHECK(profile_queries == 0);
}
TEST_CASE("uncertain authored mixer retains its real read and cannot select legacy")
{
    reset_ports(); primary_presence = JCE_FS_PRESENCE_UNKNOWN;
    fallback_presence = JCE_FS_PRESENCE_PRESENT;
    expect_path(primary_path);
    CHECK(probes.size() == 1); CHECK(profile_queries == 0);
}
TEST_CASE("confirmed missing project mixer selects existing legacy config")
{
    reset_ports(); fallback_presence = JCE_FS_PRESENCE_PRESENT;
    expect_path(fallback_path);
    CHECK(probes.size() == 2); CHECK(profile_queries == 1);
}
TEST_CASE("two confirmed absent optional paths leave Runtime default policy intact")
{
    reset_ports(); CHECK(actual_play_mixer_descriptor() == nullptr);
    CHECK(probes.size() == 2); CHECK(profile_queries == 1);
}
TEST_CASE("uncertain legacy config retains its real read rather than becoming absence")
{
    reset_ports(); fallback_presence = JCE_FS_PRESENCE_UNKNOWN;
    expect_path(fallback_path);
    CHECK(probes.size() == 2); CHECK(profile_queries == 1);
}
TEST_CASE("without a project only the selected profile store is probed")
{
    SUBCASE("existing profile mixer") {
        reset_ports(false); fallback_presence = JCE_FS_PRESENCE_PRESENT;
        expect_path(fallback_path);
        CHECK(probes.size() == 1); CHECK(profile_queries == 1);
    }
    SUBCASE("confirmed absent profile mixer") {
        reset_ports(false); CHECK(actual_play_mixer_descriptor() == nullptr);
        CHECK(probes.size() == 1); CHECK(profile_queries == 1);
    }
}
TEST_CASE("failed profile resolution cannot publish leftover generated storage")
{
    reset_ports(); profile_success = false;
    fallback_presence = JCE_FS_PRESENCE_PRESENT;
    CHECK(actual_play_mixer_descriptor() == nullptr);
    CHECK(probes.size() == 1); CHECK(profile_queries == 1);
}
TEST_CASE("a later absent project cannot reuse a prior Play descriptor")
{
    reset_ports(); primary_presence = JCE_FS_PRESENCE_PRESENT;
    expect_path(primary_path);
    reset_ports(); CHECK(actual_play_mixer_descriptor() == nullptr);
    CHECK(probes.size() == 2); CHECK(profile_queries == 1);
}
