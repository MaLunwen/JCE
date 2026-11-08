/* Actual UI capture lifecycle owner with counted texture/FBO boundary ports. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/os/core/jce_math.h>
#include <jce/ui/jce_imgui_renderer.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

struct bgfx_texture_handle_t { uint16_t idx; };
struct bgfx_frame_buffer_handle_t { uint16_t idx; };
struct bgfx_program_handle_t { uint16_t idx; };
struct bgfx_uniform_handle_t { uint16_t idx; };
struct bgfx_vertex_layout_t { unsigned unused; };
struct bgfx_caps_t { bool originBottomLeft = true; bool homogeneousDepth = false; };
#define BGFX_HANDLE_IS_VALID(handle) ((handle).idx != UINT16_MAX)
#define LOG_INFO(...) ((void)0)
constexpr uint16_t BGFX_CLEAR_NONE = 0, BGFX_CLEAR_COLOR = 1;
constexpr unsigned BGFX_TEXTURE_FORMAT_RGBA16F = 1, BGFX_VIEW_MODE_SEQUENTIAL = 2;
constexpr uint64_t BGFX_TEXTURE_RT = 1, BGFX_SAMPLER_U_CLAMP = 2, BGFX_SAMPLER_V_CLAMP = 4;

struct ImTextureData { uint64_t GetTexID() const { return 0; } };
constexpr uint64_t ImTextureID_Invalid = 0;
struct ImGuiPlatformIO { std::array<ImTextureData *, 0> Textures; };
namespace ImGui {
static void *GetCurrentContext() { return nullptr; }
static ImGuiPlatformIO &GetPlatformIO() { static ImGuiPlatformIO io; return io; }
}
static void destroy_imgui_texture(ImTextureData *) { FAIL("no atlas owner in this fixture"); }

enum class FboFailure { None, InvalidAttachment, Exhaustion };
struct Ports {
    std::array<unsigned, 16> texture_refs{};
    std::array<uint16_t, 16> attachment{};
    std::array<uint16_t, 256> view_binding{};
    unsigned texture_creates = 0, texture_destroys = 0;
    unsigned fbo_creates = 0, fbo_destroys = 0, readbacks = 0;
    unsigned next_texture = 0, next_fbo = 0;
    bool fail_texture = false;
    FboFailure fail_fbo = FboFailure::None;
    uint16_t readback_texture = UINT16_MAX, readback_width = 0, readback_height = 0;
    std::string readback_path;
};
static Ports ports;
static bgfx_texture_handle_t bgfx_create_texture_2d(uint16_t width, uint16_t height,
    bool mips, uint16_t layers, unsigned format, uint64_t flags, const void *, uint64_t)
{
    ++ports.texture_creates;
    CHECK(width > 0); CHECK(height > 0); CHECK_FALSE(mips); CHECK(layers == 1);
    CHECK(format == BGFX_TEXTURE_FORMAT_RGBA16F);
    CHECK(flags == (BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP));
    if (ports.fail_texture) return { UINT16_MAX };
    REQUIRE(ports.next_texture < ports.texture_refs.size());
    const auto idx = static_cast<uint16_t>(ports.next_texture++);
    CHECK(ports.texture_refs[idx] == 0);
    ports.texture_refs[idx] = 1;
    return { idx };
}
static bgfx_frame_buffer_handle_t bgfx_create_frame_buffer_from_handles(uint8_t count,
    const bgfx_texture_handle_t *texture, bool transfer_texture)
{
    ++ports.fbo_creates;
    CHECK(count == 1); CHECK_FALSE(transfer_texture);
    REQUIRE(texture->idx < ports.texture_refs.size());
    auto &refs = ports.texture_refs[texture->idx];
    REQUIRE(refs == 1);
    if (ports.fail_fbo == FboFailure::InvalidAttachment) return { UINT16_MAX };
    if (ports.fail_fbo == FboFailure::Exhaustion) {
        /* The pinned provider still consumes the caller ref in this failure
         * mode when requested. That asymmetry is why this owner uses false. */
        if (transfer_texture) --refs;
        return { UINT16_MAX };
    }
    REQUIRE(ports.next_fbo < ports.attachment.size());
    const auto idx = static_cast<uint16_t>(ports.next_fbo++);
    CHECK(ports.attachment[idx] == UINT16_MAX);
    ports.attachment[idx] = texture->idx;
    if (!transfer_texture) ++refs;
    return { idx };
}
static void bgfx_destroy_frame_buffer(bgfx_frame_buffer_handle_t framebuffer)
{
    ++ports.fbo_destroys;
    REQUIRE(framebuffer.idx < ports.attachment.size());
    auto &attachment = ports.attachment[framebuffer.idx];
    REQUIRE(attachment < ports.texture_refs.size());
    for (const auto bound : ports.view_binding) CHECK(bound != framebuffer.idx);
    REQUIRE(ports.texture_refs[attachment] == 2);
    --ports.texture_refs[attachment];
    attachment = UINT16_MAX;
}
static void bgfx_destroy_texture(bgfx_texture_handle_t texture)
{
    ++ports.texture_destroys;
    REQUIRE(texture.idx < ports.texture_refs.size());
    REQUIRE(ports.texture_refs[texture.idx] == 1);
    --ports.texture_refs[texture.idx];
}
static void bgfx_set_view_frame_buffer(uint16_t view, bgfx_frame_buffer_handle_t framebuffer)
{
    REQUIRE(view < ports.view_binding.size());
    if (BGFX_HANDLE_IS_VALID(framebuffer)) {
        REQUIRE(framebuffer.idx < ports.attachment.size());
        REQUIRE(ports.attachment[framebuffer.idx] != UINT16_MAX);
    }
    ports.view_binding[view] = framebuffer.idx;
}
static void bgfx_set_view_name(uint16_t, const char *, int32_t) {}
static void bgfx_set_view_clear(uint16_t, uint16_t, uint32_t, float, uint8_t) {}
static void bgfx_set_view_rect(uint16_t, uint16_t, uint16_t, uint16_t, uint16_t) {}
static void bgfx_set_view_mode(uint16_t, unsigned) {}
static void bgfx_set_view_transform(uint16_t, const void *, const void *) {}
static void bgfx_destroy_uniform(bgfx_uniform_handle_t) { FAIL("fixture uniform invalid"); }
static void bgfx_destroy_program(bgfx_program_handle_t) { FAIL("fixture program invalid"); }
static const bgfx_caps_t *bgfx_get_caps() { static const bgfx_caps_t caps; return &caps; }
static bool jce_renderer_readback_capture_submit(uint16_t texture, uint16_t view,
    uint16_t width, uint16_t height, const char *path, int yflip)
{
    REQUIRE(texture < ports.texture_refs.size());
    CHECK(ports.texture_refs[texture] == 2); CHECK(view == 250); CHECK(yflip == 1);
    ++ports.readbacks;
    ports.readback_texture = texture;
    ports.readback_width = width;
    ports.readback_height = height;
    ports.readback_path = path;
    return true;
}

#include JCE_IMGUI_CAPTURE_OWNER

static const auto initial_capture = s_cap;
static const auto initial_record = s_rec;
struct Fixture {
    Fixture()
    {
        ports = Ports{};
        ports.attachment.fill(UINT16_MAX);
        ports.view_binding.fill(UINT16_MAX);
        s_cap = initial_capture;
        s_rec = initial_record;
        s_ctx = {};
        s_ctx.view_id = 250;
        s_ctx.program.idx = s_ctx.tex_uniform.idx = UINT16_MAX;
        s_ctx.initialized = true;
    }
    static void check_empty()
    {
        for (const auto refs : ports.texture_refs) CHECK(refs == 0);
        for (const auto attachment : ports.attachment) CHECK(attachment == UINT16_MAX);
        CHECK(s_cap.fb.idx == UINT16_MAX); CHECK(s_cap.tex.idx == UINT16_MAX);
        CHECK(s_rec.fb.idx == UINT16_MAX); CHECK(s_rec.tex.idx == UINT16_MAX);
        CHECK(s_cap.w == 0); CHECK(s_cap.h == 0);
        CHECK(s_rec.w == 0); CHECK(s_rec.h == 0);
        CHECK_FALSE(s_cap.request); CHECK_FALSE(s_cap.readback_pending);
        CHECK_FALSE(s_rec.active); CHECK(s_cap.path[0] == '\0');
    }
};

TEST_CASE_FIXTURE(Fixture, "ordinary UI never allocates a capture target or destroys handle zero")
{
    jce_imgui_renderer_setup_view(640, 480);
    jce_imgui_renderer_shutdown();
    jce_imgui_renderer_shutdown();
    CHECK(ports.texture_creates == 0); CHECK(ports.fbo_creates == 0);
    CHECK(ports.texture_destroys == 0); CHECK(ports.fbo_destroys == 0);
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "completed window capture reuses valid zero then closes both references")
{
    jce_imgui_renderer_request_capture("first.png");
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.view_binding[250] == 0);
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.readbacks == 1); CHECK(ports.readback_texture == 0);
    CHECK(ports.readback_path == "first.png");
    jce_imgui_renderer_request_capture("second.png");
    jce_imgui_renderer_setup_view(640, 480);
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.readbacks == 2); CHECK(ports.readback_path == "second.png");
    CHECK(ports.fbo_creates == 1); CHECK(ports.texture_creates == 1);
    jce_imgui_renderer_shutdown();
    jce_imgui_renderer_shutdown();
    CHECK(ports.fbo_destroys == 1); CHECK(ports.texture_destroys == 1);
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "shutdown cancels unsubmitted capture intent and detaches its view")
{
    jce_imgui_renderer_request_capture("pending.png");
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(s_cap.readback_pending);
    jce_imgui_renderer_shutdown();
    CHECK(ports.readbacks == 0);
    CHECK(ports.view_binding[250] == UINT16_MAX);
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "resized window releases old capture after its readback has been submitted")
{
    jce_imgui_renderer_request_capture("old.png");
    jce_imgui_renderer_setup_view(640, 480);
    jce_imgui_renderer_setup_view(800, 600);
    CHECK(ports.readback_width == 640); CHECK(ports.readback_height == 480);
    jce_imgui_renderer_request_capture("new.png");
    jce_imgui_renderer_setup_view(800, 600);
    CHECK(ports.fbo_destroys == 1); CHECK(ports.texture_destroys == 1);
    CHECK(ports.view_binding[250] == 1);
    jce_imgui_renderer_shutdown();
    CHECK(ports.fbo_destroys == 2); CHECK(ports.texture_destroys == 2);
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "record resize and stop retire at setup, then active shutdown also closes")
{
    jce_imgui_renderer_set_recording(true);
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.view_binding[251] == 0);
    jce_imgui_renderer_setup_view(800, 600);
    CHECK(ports.fbo_destroys == 1); CHECK(ports.texture_destroys == 1);
    CHECK(ports.view_binding[251] == 1);
    jce_imgui_renderer_set_recording(false);
    CHECK(ports.fbo_destroys == 1);
    jce_imgui_renderer_setup_view(800, 600);
    CHECK(ports.fbo_destroys == 2); CHECK(ports.texture_destroys == 2);
    CHECK(ports.view_binding[251] == UINT16_MAX);
    jce_imgui_renderer_set_recording(true);
    jce_imgui_renderer_setup_view(800, 600);
    jce_imgui_renderer_request_capture("same-frame.png");
    jce_imgui_renderer_setup_view(800, 600);
    CHECK(ports.view_binding[250] != ports.view_binding[251]);
    jce_imgui_renderer_shutdown();
    CHECK(ports.fbo_destroys == 4); CHECK(ports.texture_destroys == 4);
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "FBO rejection and exhaustion both return the explicit caller texture once")
{
    SUBCASE("attachment validation rejection") { ports.fail_fbo = FboFailure::InvalidAttachment; }
    SUBCASE("framebuffer handles exhausted") { ports.fail_fbo = FboFailure::Exhaustion; }
    jce_imgui_renderer_request_capture("failed.png");
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.fbo_creates == 1); CHECK(ports.texture_destroys == 1);
    CHECK(ports.fbo_destroys == 0); CHECK(ports.readbacks == 0);
    CHECK(s_cap.fb.idx == UINT16_MAX); CHECK(s_cap.tex.idx == UINT16_MAX);
    CHECK_FALSE(s_cap.request); CHECK_FALSE(s_cap.readback_pending);
    CHECK(ports.texture_refs[0] == 0);
    ports.fail_fbo = FboFailure::None;
    jce_imgui_renderer_request_capture("retry.png");
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.view_binding[250] == 0);
    jce_imgui_renderer_shutdown();
    check_empty();
}

TEST_CASE_FIXTURE(Fixture, "texture allocation failure submits no FBO attachment")
{
    ports.fail_texture = true;
    jce_imgui_renderer_request_capture("allocation-failed.png");
    jce_imgui_renderer_setup_view(640, 480);
    CHECK(ports.texture_creates == 1); CHECK(ports.fbo_creates == 0);
    CHECK(ports.texture_destroys == 0); CHECK(ports.fbo_destroys == 0);
    jce_imgui_renderer_shutdown();
    check_empty();
}
