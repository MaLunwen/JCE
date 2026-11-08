/* Complete actual panel state/content/tick with counted UI observation ports.
 * Real RP and Bundle owners resolve requested/effective intent. No GUI or GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_project_state.h"
#include "io/jce_editor_bundle_render_pipeline.h"
#include "jce_test_owned_directory.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_console.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_texture.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct UiPort {
    struct Readout { std::string label, authored, applied; bool disabled = false; };
    const char *checkbox = nullptr;
    const char *button = nullptr;
    unsigned checkbox_edits = 0;
    bool checkbox_before = false;
    unsigned publications = 0;
    std::vector<Readout> readouts;
} ui;
JceGpuTier tier = JCE_GPU_TIER_HIGH;
}

/* Typed widget ports run the complete production panel dispatch. They change
 * only the selected widget value, never a pipeline or panel decision. */
struct ImVec4 { float x, y, z, w; };
enum { ImGuiCol_Header, ImGuiTreeNodeFlags_DefaultOpen,
       ImGuiHoveredFlags_AllowWhenDisabled };
namespace jce_theme {
static ImVec4 inspector_header_color() { return {}; }
}
namespace ImGui {
static void PushStyleColor(int, ImVec4) {}
static void PopStyleColor() {}
static bool CollapsingHeader(const char *, int = 0) { return true; }
static void SetNextItemWidth(float) {}
static bool InputText(const char *, char *, size_t) { return false; }
static void SameLine() {}
static bool Button(const char *label)
{
    if (!ui.button || std::strcmp(ui.button, label)) return false;
    ui.button = nullptr;
    return true;
}
static bool Checkbox(const char *label, bool *value)
{
    if (!ui.checkbox || std::strcmp(ui.checkbox, label)) return false;
    ui.checkbox = nullptr;
    ++ui.checkbox_edits;
    ui.checkbox_before = *value;
    *value = !*value;
    return true;
}
static void BeginDisabled() {}
static void EndDisabled() {}
static bool IsItemHovered(int = 0) { return false; }
static void SetTooltip(const char *, ...) {}
static void observe_readout(const char *format, va_list args, bool disabled)
{
    if (std::strcmp(format, "panel.render_pipeline.effective.values")) return;
    UiPort::Readout row;
    row.label = va_arg(args, const char *);
    row.authored = va_arg(args, const char *);
    row.applied = va_arg(args, const char *);
    row.disabled = disabled;
    ui.readouts.push_back(row);
}
static void TextDisabled(const char *format, ...)
{
    va_list args; va_start(args, format); observe_readout(format, args, true); va_end(args);
}
static void Text(const char *format, ...)
{
    va_list args; va_start(args, format); observe_readout(format, args, false); va_end(args);
}
static void TextWrapped(const char *, ...) {}
static void TextColored(ImVec4, const char *, ...) {}
static void TextUnformatted(const char *) {}
static void Indent() {}
static void Unindent() {}
static bool SliderFloat(const char *, float *, float, float, const char *) { return false; }
static bool SliderInt(const char *, int *, int, int) { return false; }
static bool Combo(const char *, int *, const char *const *, int) { return false; }
}

extern "C" {
const char *jce_editor_i18n(const char *key) { return key; }
JceRendererBackend jce_renderer_get_active_backend(void) { return JCE_BACKEND_D3D11; }
JceGpuTier jce_renderer_get_tier(void) { return tier; }
uint32_t jce_renderer_get_caps(void)
{ return JCE_CAP_TEXTURE_FLOAT | JCE_CAP_FRAMEBUFFER_FLOAT | JCE_CAP_COMPUTE; }
JceRenderRecommendation jce_renderer_get_recommendation(void)
{ JceRenderRecommendation r{}; r.has_discrete_gpu = true; return r; }
const char *jce_gpu_tier_name(JceGpuTier) { return "controlled"; }
JcePostFXPipeline *jce_scene_renderer_get_postfx(JceSceneRenderer *) { return nullptr; }
bool jce_postfx_get_motion_blur(const JcePostFXPipeline *) { return false; }
bool jce_postfx_get_motion_blur_active(const JcePostFXPipeline *) { return false; }
int8_t jce_texture_get_global_mip_bias(void) { return 0; }
void jce_texture_set_global_mip_bias(int8_t) {}
}
bool jce_editor_pstate_active(void) { return false; }
bool jce_editor_pstate_get_str(const char *, char *, size_t) { return false; }
void jce_editor_pstate_set_str(const char *, const char *) {}
JceSceneRenderer *jce_editor_get_scene_renderer(void) { return nullptr; }

#include JCE_TEST_EDITOR_PIPELINE_PANEL_OWNER

namespace {
static JceRenderPipelineDesc requested()
{ JceRenderPipelineDesc d; jce_render_pipeline_get_requested(&d); return d; }
static void published(const JceRenderPipelineDesc *, void *) { ++ui.publications; }
struct Fixture {
    Fixture()
    {
        tier = JCE_GPU_TIER_HIGH;
        ui = {};
        s_rp = {};
        jce_render_pipeline_set_observer(nullptr, nullptr);
        JceRenderPipelineDesc initial;
        jce_render_pipeline_preset_high(&initial);
        jce_render_pipeline_apply(&initial);
        jce_render_pipeline_set_observer(published, nullptr);
    }
    ~Fixture() { jce_render_pipeline_set_observer(nullptr, nullptr); }
};
static void check_descriptor(const JceRenderPipelineDesc &actual,
                             const JceRenderPipelineDesc &expected)
{
#define CHECK_FIELD(name) CHECK(actual.name == expected.name)
    CHECK_FIELD(enable_csm);
    CHECK_FIELD(enable_ssao);
    CHECK_FIELD(enable_ssr);
    CHECK_FIELD(enable_taa);
    CHECK_FIELD(enable_bloom);
    CHECK_FIELD(enable_volumetric_fog);
    CHECK_FIELD(enable_gpu_particles);
    CHECK_FIELD(enable_motion_blur);
    CHECK_FIELD(enable_cloth);
    CHECK_FIELD(enable_stylized_sky);
    CHECK_FIELD(shadow_resolution);
    CHECK_FIELD(csm_cascade_count);
    CHECK_FIELD(shadow_filter_quality);
    CHECK_FIELD(msaa_samples);
    CHECK_FIELD(render_scale);
    CHECK_FIELD(post_quality);
    CHECK_FIELD(hdr_color);
    CHECK_FIELD(depth_prepass);
    CHECK_FIELD(motion_blur_intensity);
    CHECK_FIELD(sun_soft_size);
    CHECK_FIELD(enable_ssgi);
#undef CHECK_FIELD
    for (int index = 0; index < JCE_RP_PERF_COUNT; ++index)
        CHECK(actual.perf[index] == expected.perf[index]);
}
static void exact_request(const JceRenderPipelineDesc &expected)
{ check_descriptor(requested(), expected); }
static JceRenderPipelineDesc authored()
{
    JceRenderPipelineDesc d;
    jce_render_pipeline_preset_high(&d);
    d.enable_volumetric_fog = false;
    d.enable_ssao = false;
    d.enable_ssr = false;
    d.csm_cascade_count = 3;
    d.msaa_samples = 1;
    d.render_scale = .85f;
    d.perf[JCE_RP_PERF_GPU_SCENE] = JCE_RP_AUTO;
    return d;
}
static void edit_bloom()
{
    ui.checkbox = "panel.render_pipeline.feature.bloom";
    jce_editor_panel_render_pipeline_content();
    REQUIRE(ui.checkbox_edits == 1);
    REQUIRE(ui.checkbox == nullptr);
}
static const void *JCE_CALL find_pak(const JcePakArchive *p, const char *key)
{ return jce_pak_find(p, key); }
static uint64_t JCE_CALL size_pak(const void *asset)
{ return static_cast<const JcePakAsset *>(asset)->original_size; }
static size_t JCE_CALL decode_pak(const void *asset, void *out, size_t size)
{ return jce_pak_decompress(static_cast<const JcePakAsset *>(asset), out, size); }
const JceFsPakProvider provider{find_pak, size_pak, decode_pak};
}

TEST_CASE("panel initialization then another project request cannot restore stale fields")
{
    Fixture fixture;
    jce_editor_panel_render_pipeline_tick();
    REQUIRE(s_rp.initialized);
    auto expected = authored();
    jce_render_pipeline_apply(&expected);
    ui.publications = 0;
    edit_bloom();
    CHECK(ui.checkbox_before == expected.enable_bloom);
    expected.enable_bloom = !expected.enable_bloom;
    exact_request(expected);
    CHECK(ui.publications == 1);
}

TEST_CASE("panel edits retain unclamped author intent on LOW hardware")
{
    Fixture fixture;
    jce_editor_panel_render_pipeline_tick();
    tier = JCE_GPU_TIER_LOW;
    auto expected = authored();
    jce_render_pipeline_apply(&expected);
    JceRenderPipelineDesc effective;
    jce_render_pipeline_get(&effective);
    REQUIRE_FALSE(effective.hdr_color);
    REQUIRE_FALSE(effective.enable_taa);
    REQUIRE(effective.csm_cascade_count == 1);
    ui.publications = 0;
    edit_bloom();
    expected.enable_bloom = !expected.enable_bloom;
    exact_request(expected);
    CHECK(ui.publications == 1);
    tier = JCE_GPU_TIER_HIGH;
    jce_render_pipeline_notify_tier_changed();
    jce_render_pipeline_get(&effective);
    CHECK(effective.hdr_color);
    CHECK(effective.enable_taa);
    CHECK(effective.csm_cascade_count == 3);
}

TEST_CASE("actual Bundle open after panel initialization owns unrelated checkbox edits")
{
    Fixture fixture;
    jce_editor_panel_render_pipeline_tick();
    const auto initial = requested();
    const auto *previous = jce_fs_get_pak_provider();
    jce_fs_set_pak_provider(&provider);
    const char bytes[] = R"({"enable_volumetric_fog":false,"enable_taa":true,"enable_bloom":true,"render_scale":0.65,"csm_cascade_count":3})";
    JceArchiveWriter *writer = jce_archive_writer_create(nullptr);
    REQUIRE(writer);
    REQUIRE(jce_archive_writer_add(writer, "settings/render_pipeline.rp.json", bytes, sizeof bytes - 1));
    void *blob = nullptr;
    size_t size = 0;
    REQUIRE(jce_archive_writer_finish(writer, &blob, &size));
    jce_archive_writer_destroy(writer);
    JcePakArchive *pak = jce_pak_open(blob, size);
    REQUIRE(pak);
    JceFileSystem *fs = jce_fs_create();
    REQUIRE(fs);
    REQUIRE(jce_fs_mount_pak(fs, pak));
    JceEditorBundlePipeline bundle;
    REQUIRE(jce_editor_bundle_pipeline_prepare(&bundle, fs));
    REQUIRE(jce_editor_bundle_pipeline_apply(&bundle));
    auto expected = requested();
    ui.publications = 0;
    edit_bloom();
    expected.enable_bloom = !expected.enable_bloom;
    exact_request(expected);
    CHECK_FALSE(requested().enable_volumetric_fog);
    CHECK(requested().render_scale == doctest::Approx(.65));
    CHECK(ui.publications == 1);
    jce_editor_bundle_pipeline_close(&bundle);
    exact_request(initial);
    jce_fs_destroy(fs);
    jce_pak_close(pak);
    jce_free(blob);
    jce_fs_set_pak_provider(previous);
}

TEST_CASE("refresh preserves independent unsubmitted path and status without applying")
{
    Fixture fixture;
    jce_editor_panel_render_pipeline_tick();
    std::strcpy(s_rp.asset_path, "draft/not-yet-saved.rp.json");
    std::strcpy(s_rp.status_msg, "retained status");
    const auto expected = authored();
    jce_render_pipeline_apply(&expected);
    ui.publications = 0;
    jce_editor_panel_render_pipeline_content();
    jce_editor_panel_render_pipeline_tick();
    CHECK(std::strcmp(s_rp.asset_path, "draft/not-yet-saved.rp.json") == 0);
    CHECK(std::strcmp(s_rp.status_msg, "retained status") == 0);
    CHECK(ui.publications == 0);
    exact_request(expected);
    check_descriptor(s_rp.desc, expected);
}

TEST_CASE("same-draw preset remains an intentional new request")
{
    Fixture fixture;
    jce_editor_panel_render_pipeline_tick();
    auto project = authored();
    jce_render_pipeline_apply(&project);
    ui.button = "panel.render_pipeline.preset.low";
    jce_editor_panel_render_pipeline_content();
    JceRenderPipelineDesc expected;
    jce_render_pipeline_preset_low(&expected);
    exact_request(expected);
    jce_editor_panel_render_pipeline_tick();
    exact_request(expected);
}

TEST_CASE("applied policy readout observes clamps without changing authored state")
{
    Fixture fixture;
    tier = JCE_GPU_TIER_LOW;
    const auto expected = authored();
    jce_render_pipeline_apply(&expected);
    JceRenderPipelineDesc effective;
    jce_render_pipeline_get(&effective);
    REQUIRE_FALSE(effective.hdr_color);
    REQUIRE_FALSE(effective.enable_taa);
    REQUIRE(effective.csm_cascade_count == 1);
    ui.publications = 0;
    jce_editor_panel_render_pipeline_content();
    REQUIRE(ui.readouts.size() == 4);
    for (const auto &row : ui.readouts) CHECK_FALSE(row.disabled);
    const char *on = "panel.project_settings.field.on";
    const char *off = "panel.project_settings.field.off";
    CHECK(ui.readouts[0].label == "panel.render_pipeline.format.hdr_color");
    CHECK(ui.readouts[0].authored == on);
    CHECK(ui.readouts[0].applied == off);
    CHECK(ui.readouts[1].label == "panel.render_pipeline.feature.taa");
    CHECK(ui.readouts[1].authored == on);
    CHECK(ui.readouts[1].applied == off);
    CHECK(ui.readouts[2].label == "panel.render_pipeline.quality.cascade_count");
    CHECK(ui.readouts[2].authored == "3");
    CHECK(ui.readouts[2].applied == "1");
    char scale[32];
    std::snprintf(scale, sizeof(scale), "%.2f", (double)effective.render_scale);
    CHECK(ui.readouts[3].label == "panel.render_pipeline.quality.render_scale");
    CHECK(ui.readouts[3].authored == "0.85");
    CHECK(ui.readouts[3].applied == scale);
    CHECK(ui.publications == 0);
    exact_request(expected);
    check_descriptor(s_rp.desc, expected);

    ui.readouts.clear();
    ui.checkbox = "panel.render_pipeline.format.hdr_color";
    jce_editor_panel_render_pipeline_content();
    REQUIRE(ui.checkbox_edits == 1);
    REQUIRE(ui.readouts.size() == 4);
    for (const auto &row : ui.readouts) CHECK_FALSE(row.disabled);
    CHECK(ui.readouts[0].authored == off);
    CHECK(ui.readouts[0].applied == off);
    CHECK_FALSE(requested().hdr_color);
    CHECK(ui.publications == 1);
}

TEST_CASE("Save serializes author intent while the applied-policy readout is clamped")
{
    Fixture fixture;
    JceTestOwnedDirectory directory("_ut_editor_pipeline_panel_save");
    const auto path = directory.path("authored.rp.json");
    REQUIRE(path.size() < sizeof(s_rp.asset_path));
    tier = JCE_GPU_TIER_LOW;
    const auto expected = authored();
    jce_render_pipeline_apply(&expected);
    jce_editor_panel_render_pipeline_tick();
    std::strcpy(s_rp.asset_path, path.c_str());
    ui.publications = 0;
    ui.button = "panel.render_pipeline.asset.save";
    jce_editor_panel_render_pipeline_content();
    REQUIRE(ui.button == nullptr);
    REQUIRE(ui.readouts.size() == 4);
    for (const auto &row : ui.readouts) CHECK_FALSE(row.disabled);
    CHECK(ui.readouts[0].applied == "panel.project_settings.field.off");
    JceRenderPipelineDesc saved;
    REQUIRE(jce_render_pipeline_load(path.c_str(), &saved));
    check_descriptor(saved, expected);
    check_descriptor(s_rp.desc, expected);
    exact_request(expected);
    CHECK(ui.publications == 0);
}
