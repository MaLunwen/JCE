#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <jce/os/core/jce_filesystem.h>

#include "core/jce_editor_project_render_pipeline.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr const char *kRoot = "_ut_editor_project_render_pipeline";

bool descriptor_is_zero(const JceRenderPipelineDesc &desc)
{
    const auto *begin = reinterpret_cast<const unsigned char *>(&desc);
    return std::all_of(begin, begin + sizeof(desc),
                       [](unsigned char byte) { return byte == 0; });
}

void write_pipeline(const char *text)
{
    const char *path =
        "_ut_editor_project_render_pipeline/Settings/RenderPipeline.rp.json";
    REQUIRE(jce_fs_host_write_all(path, text, std::strlen(text)));
}

} // namespace

TEST_CASE("project render pipeline loading is isolated per project")
{
    (void)jce_fs_host_remove_recursive(kRoot);

    const char *valid = R"json({
        "version": 1,
        "enable_taa": false,
        "enable_bloom": true,
        "shadow_resolution": 512
    })json";
    write_pipeline(valid);

    JceRenderPipelineDesc loaded{};
    REQUIRE(jce_editor_project_render_pipeline_load(kRoot, &loaded));
    CHECK_FALSE(loaded.enable_taa);
    CHECK(loaded.enable_bloom);
    CHECK(loaded.shadow_resolution == 512);

    std::memset(&loaded, 0xa5, sizeof(loaded));
    CHECK_FALSE(jce_editor_project_render_pipeline_load(
        "_ut_editor_project_render_pipeline_missing", &loaded));
    CHECK(descriptor_is_zero(loaded));

    write_pipeline("{ malformed");
    std::memset(&loaded, 0xa5, sizeof(loaded));
    CHECK_FALSE(jce_editor_project_render_pipeline_load(kRoot, &loaded));
    CHECK(descriptor_is_zero(loaded));

    CHECK_FALSE(jce_editor_project_render_pipeline_load(kRoot, nullptr));
    (void)jce_fs_host_remove_recursive(kRoot);
}
