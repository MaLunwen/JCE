/* Actual Archive/PAK/VFS and JSON owners; only the private project-quality
 * snapshot ports are counted. No renderer, window, Bundle loader or GPU starts.
 * Changing CWD is a test-only portable host operation, restored before cleanup. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "core/jce_editor_effective_render_settings.h"
#include "core/jce_project_settings.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

static const JceProjectSettings *s_quality = nullptr;
static unsigned s_quality_loads = 0;
const JceProjectSettings *jce_project_settings_current(void) { return s_quality; }
bool jce_project_settings_load(JceProjectSettings *) { ++s_quality_loads; return false; }

namespace {
const char *kMounted = R"({"grassEnabled":1,"lodBias":1.75,"msaa":8,"look":{"wrap":0.27}})";
const char *kPoisonA = R"({"grassEnabled":0,"lodBias":7,"look":{"wrap":0.91}})";
const char *kPoisonB = R"({"grassEnabled":0,"lodBias":9,"look":{"wrap":0.82}})";

void write_bytes(const std::string &path, const std::string &bytes)
{
    REQUIRE(jce_fs_host_write_all(path.c_str(), bytes.data(), bytes.size()));
}

struct ScopedCwd {
    std::filesystem::path previous;
    explicit ScopedCwd(const std::string &path)
    {
        char cwd[2048] = {};
        REQUIRE(jce_fs_host_get_current_dir(cwd, sizeof cwd));
        previous = cwd;
        std::error_code error;
        std::filesystem::current_path(path, error);
        REQUIRE(error.value() == 0);
        char actual[2048] = {};
        REQUIRE(jce_fs_host_get_current_dir(actual, sizeof actual));
        CHECK(std::filesystem::equivalent(path, actual));
    }
    ~ScopedCwd()
    {
        std::error_code error;
        std::filesystem::current_path(previous, error);
        CHECK(error.value() == 0);
    }
};

struct HostConflict {
    JceTestOwnedDirectory owned{"jce_render_settings_vfs"};
    std::string a = owned.path("CWD A"), b = owned.path("CWD B");
    HostConflict()
    {
        REQUIRE(jce_fs_host_create_directory(a.c_str()));
        REQUIRE(jce_fs_host_create_directory(b.c_str()));
        write_bytes(a + "/render_settings.json", kPoisonA);
        write_bytes(b + "/render_settings.json", kPoisonB);
    }
};

/* The core VFS uses its public resource-provider inversion port. These are
 * transparent adapters to the real PAK reader, not fake bytes or decode logic. */
const void *JCE_CALL pak_find(const JcePakArchive *pak, const char *path)
{ return jce_pak_find(pak, path); }
uint64_t JCE_CALL pak_size(const void *asset)
{ return static_cast<const JcePakAsset *>(asset)->original_size; }
size_t JCE_CALL pak_decode(const void *asset, void *bytes, size_t size)
{ return jce_pak_decompress(static_cast<const JcePakAsset *>(asset), bytes, size); }
const JceFsPakProvider kActualPakProvider{pak_find, pak_size, pak_decode};

struct ActiveScope {
    const JceFsPakProvider *previous_provider = jce_fs_get_pak_provider();
    ActiveScope()
    {
        REQUIRE(jce_fs_get_active() == nullptr);
        jce_fs_set_pak_provider(&kActualPakProvider);
        s_quality = nullptr; s_quality_loads = 0;
    }
    ~ActiveScope()
    {
        jce_fs_set_active(nullptr);
        jce_fs_set_pak_provider(previous_provider);
        s_quality = nullptr;
    }
};

struct MountedPak {
    void *blob = nullptr;
    JcePakArchive *pak = nullptr;
    JceFileSystem *fs = nullptr;
    explicit MountedPak(const char *json)
    {
        std::unique_ptr<JceArchiveWriter, decltype(&jce_archive_writer_destroy)>
            writer(jce_archive_writer_create(nullptr), jce_archive_writer_destroy);
        REQUIRE(writer != nullptr);
        const char *key = json ? "render_settings.json" : "unrelated.txt";
        const char *bytes = json ? json : "missing render settings";
        REQUIRE(jce_archive_writer_add(writer.get(), key, bytes, std::strlen(bytes)));
        size_t size = 0;
        REQUIRE(jce_archive_writer_finish(writer.get(), &blob, &size));
        pak = jce_pak_open(blob, size);
        REQUIRE(pak != nullptr);
        fs = jce_fs_create();
        REQUIRE(fs != nullptr);
        REQUIRE(jce_fs_mount_pak(fs, pak));
    }
    void activate(JceFsActivePolicy policy = JCE_FS_ACTIVE_ISOLATED) const
    { jce_fs_set_active_policy(fs, policy); }
    ~MountedPak()
    {
        if (jce_fs_get_active() == fs) jce_fs_set_active(nullptr);
        jce_fs_destroy(fs); jce_pak_close(pak); jce_free(blob);
    }
};

void check_mounted()
{
    JceRenderSettings out = jce_render_settings_default();
    std::string used = "stale project";
    REQUIRE(jce_editor_effective_render_settings(true, "ignored-host-project", &out, &used));
    CHECK(used == "render_settings.json");
    CHECK(out.grass_enabled == 1);
    CHECK(out.lod_bias == doctest::Approx(1.75));
    CHECK(out.wrap_factor == doctest::Approx(0.27));
    CHECK(s_quality_loads == 1);
}

void check_refused()
{
    JceRenderSettings out = jce_render_settings_default();
    out.grass_enabled = 7; out.wrap_factor = 0.37f;
    const JceRenderSettings before = out;
    std::string used = "stale project";
    CHECK_FALSE(jce_editor_effective_render_settings(true, "ignored-host-project", &out, &used));
    CHECK(used.empty());
    CHECK(std::memcmp(&out, &before, sizeof out) == 0);
    CHECK(s_quality_loads == 1);
}
} // namespace

TEST_CASE("isolated mounted settings are invariant under two conflicting actual CWDs")
{
    HostConflict host;
    ActiveScope active;
    MountedPak content(kMounted);
    content.activate();
    { ScopedCwd cwd(host.a); check_mounted(); }
    s_quality_loads = 0;
    { ScopedCwd cwd(host.b); check_mounted(); }
}

TEST_CASE("direct isolated VFS remains usable with the legacy reader callback absent")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    MountedPak content(kMounted);
    content.activate();
    jce_fs_set_active_reader(nullptr);
    check_mounted();
}

TEST_CASE("isolated admission refuses absent or non-isolated active content")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    MountedPak content(kMounted);
    SUBCASE("no active filesystem") { check_refused(); }
    SUBCASE("overlay is not isolated admission") {
        content.activate(JCE_FS_ACTIVE_OVERLAY); check_refused();
    }
}

TEST_CASE("a real isolated VFS miss never reads the conflicting host file")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    MountedPak missing(nullptr);
    missing.activate();
    SUBCASE("normal convenience reader already refuses this miss") { check_refused(); }
    SUBCASE("absent legacy callback also refuses the actual VFS miss") {
        jce_fs_set_active_reader(nullptr); check_refused();
    }
}

TEST_CASE("malformed empty and oversized mounted JSON leave the output unchanged")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    std::string json;
    SUBCASE("malformed") { json = "{"; }
    SUBCASE("empty") { json.clear(); }
    SUBCASE("above the existing one MiB JSON limit") {
        json = "{}"; json.append((1u << 20), ' ');
    }
    MountedPak content(json.c_str());
    content.activate();
    check_refused();
}

TEST_CASE("active filesystem epoch replacement cannot retain old settings bytes")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    MountedPak first(kMounted), second(kPoisonB), missing(nullptr);
    first.activate(); check_mounted();
    second.activate(); s_quality_loads = 0;
    JceRenderSettings out = jce_render_settings_default();
    std::string used;
    REQUIRE(jce_editor_effective_render_settings(true, nullptr, &out, &used));
    CHECK(out.lod_bias == doctest::Approx(9));
    CHECK(out.wrap_factor == doctest::Approx(0.82));
    CHECK(used == "render_settings.json");
    missing.activate(); s_quality_loads = 0; check_refused();
}

TEST_CASE("authored quality wins while file and quality-only provenance remain distinct")
{
    HostConflict host;
    ScopedCwd cwd(host.a);
    ActiveScope active;
    MountedPak content(kMounted), missing(nullptr);
    JceProjectSettings quality{};
    quality.quality.levels[0].lod_bias = 2.5f;
    quality.quality.levels[0].shadow_resolution = 2;
    quality.quality.levels[0].shadow_cascades = 2;
    quality.quality.levels[0].anti_aliasing = 4;
    s_quality = &quality;
    JceRenderSettings out = jce_render_settings_default();
    std::string used;
    SUBCASE("file look and grass survive quality-owned overrides") {
        content.activate();
        REQUIRE(jce_editor_effective_render_settings(true, nullptr, &out, &used));
        CHECK(used == "render_settings.json");
        CHECK(out.grass_enabled == 1);
        CHECK(out.wrap_factor == doctest::Approx(0.27));
    }
    SUBCASE("missing file resolves only the authored quality snapshot") {
        missing.activate(); jce_fs_set_active_reader(nullptr);
        out.grass_enabled = 7; out.wrap_factor = 0.37f;
        REQUIRE(jce_editor_effective_render_settings(true, nullptr, &out, &used));
        CHECK(used.empty());
        CHECK(out.grass_enabled == 7);
        CHECK(out.wrap_factor == doctest::Approx(0.37));
    }
    CHECK(out.lod_bias == doctest::Approx(2.5));
    CHECK(out.msaa == 4);
    CHECK(out.shadow_map_size == 2048);
    CHECK(s_quality_loads == 0);
}

TEST_CASE("host projects retain explicit custom source and cooked fallback policy")
{
    HostConflict host;
    ActiveScope active;
    const std::string source = host.owned.path("authored"), cooked = host.owned.path("cooked");
    REQUIRE(jce_fs_host_create_directory(source.c_str()));
    REQUIRE(jce_fs_host_create_directory(cooked.c_str()));
    write_bytes(cooked + "/render_settings.json", kPoisonB);
    std::string wanted = cooked + "/render_settings.json";
    SUBCASE("missing source reads the explicit cooked root") {}
    SUBCASE("source wins over cooked") {
        write_bytes(source + "/render_settings.json", kMounted);
        wanted = source + "/render_settings.json";
    }
    JceRenderSettings out = jce_render_settings_default();
    std::string used;
    REQUIRE(jce_editor_effective_render_settings(false, host.owned.root.c_str(),
        &out, &used, "authored", "cooked"));
    CHECK(used == wanted);
    CHECK(out.lod_bias == doctest::Approx(wanted == source + "/render_settings.json" ? 1.75 : 9));
}

TEST_CASE("loose authored settings never inherit cached project quality or CWD defaults")
{
    HostConflict host;
    ActiveScope active;
    const auto loose = host.owned.path("loose-content");
    REQUIRE(jce_fs_host_create_directory(loose.c_str()));
    write_bytes(loose + "/render_settings.json",
        R"({"msaa":2,"shadowMapSize":4096,"shadowCascades":4,"grassEnabled":1})");
    JceProjectSettings quality{};
    quality.quality.levels[0].anti_aliasing = 8;
    quality.quality.levels[0].shadow_resolution = 1;
    quality.quality.levels[0].shadow_cascades = 2;
    SUBCASE("a stale outgoing project snapshot exists") { s_quality = &quality; }
    SUBCASE("there is no cached quality snapshot") { s_quality = nullptr; }
    JceRenderSettings out = jce_render_settings_default();
    std::string used;
    REQUIRE(jce_editor_effective_render_settings(false, nullptr, &out, &used,
                                                nullptr, nullptr, loose.c_str()));
    CHECK(used == loose + "/render_settings.json");
    CHECK(out.msaa == 2);
    CHECK(out.shadow_map_size == 4096);
    CHECK(out.shadow_cascades == 4);
    CHECK(out.grass_enabled == 1);
    CHECK(s_quality_loads == 0);
}
