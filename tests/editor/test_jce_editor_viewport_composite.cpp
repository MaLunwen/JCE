/* Exact shared composite/capture helpers and complete Scene/Game Canvas tails.
 * Typed SDK ports observe CPU routing; no device, pixels or performance claim. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_postfx.h>
#include <jce/middleware/scene/jce_ui_canvas.h>
#include <jce/application/jce_runtime.h>
#include <jce/os/platform/jce_window.h>
#include <cstdint>
#include <type_traits>

struct JceOffscreenTarget { uint16_t color = 7; };
struct JceRenderer { JceRendererBackend backend = JCE_BACKEND_D3D11; };
struct JceScene { unsigned canvases = 0; JceCanvasComponent canvas{}; };
struct JcePostFXPipeline { uint16_t output = UINT16_MAX, view_base = 0; };
static JcePostFXPipeline pipelines[2];
static unsigned creates;
static JceScene *current_scene;
static bool bottom_left, physics_debug, show_ui;
static unsigned copies, queries, canvas_calls, event_calls, shot_calls;
static uint16_t copied_source, shot_source, shot_width, shot_height;
static int shot_yflip;
static bool copied_flip;
static bool target_available, bridge_hdr;
static uint16_t expected_canvas_fb, canvas_fb;
static uint32_t bridge_width, bridge_height;
static JcePostFXOutputTarget target_info;
static bool target_port(const JcePostFXPipeline *pipeline, JcePostFXOutputTarget *out)
{
    REQUIRE(out->struct_size == sizeof(*out));
    REQUIRE(out->version == JCE_POSTFX_OUTPUT_TARGET_VERSION);
    if (!pipeline || !target_available) return false;
    *out = target_info;
    return true;
}
static bool hdr_port(const JceOffscreenTarget *) { return bridge_hdr; }
static JceCanvasComponent *canvas_component_port(JceScene *scene, JceEntity)
{ return &scene->canvas; }

static uint16_t color_port(const JceOffscreenTarget *target) { return target->color; }
static bool extent_port(const JceOffscreenTarget *, uint32_t *w, uint32_t *h)
{ *w = bridge_width; *h = bridge_height; return true; }
static bool origin_port() { return bottom_left; }
static JceRendererBackend backend_port(const JceRenderer *renderer) { return renderer->backend; }
static void each_canvas_port(JceScene *scene, JceEntityCallback callback, void *user)
{
    ++queries;
    for (unsigned i = 0; i < scene->canvases; ++i) callback(scene, i + 1, user);
}
static void composite_port(JceOffscreenTarget *, uint16_t, uint16_t source,
    uint16_t w, uint16_t h, bool flip)
{ ++copies; copied_source = source; copied_flip = flip; CHECK(w == 640); CHECK(h == 360); }
static bool shot_port(unsigned, uint16_t source, uint16_t, uint16_t w, uint16_t h,
    const char *, int flip)
{ ++shot_calls; shot_source = source; shot_width = w; shot_height = h; shot_yflip = flip; return true; }
static void canvas_port(JceUICanvas *, JceScene *scene, uint16_t, uint16_t fb,
    float w, float h, const JceUIPointer *, float)
{ ++canvas_calls; canvas_fb = fb; CHECK(scene == current_scene); CHECK(fb == expected_canvas_fb); CHECK(w == 640); CHECK(h == 360); }
static void events_port(JceRuntime *, JceUICanvas *, JceScene *) { ++event_calls; }
static uint64_t focused_port(const JceUICanvas *) { return 0; }
static bool text_start_port(JceWindow *) { return true; }
static bool text_stop_port(JceWindow *) { return true; }
static jce_allocator_t allocator_port() { return {}; }
static JcePostFXPipeline *create_port(jce_allocator_t, uint32_t, uint32_t)
{ REQUIRE(creates < 2); return &pipelines[creates++]; }
static bool load_port(JcePostFXPipeline *, const JcePakArchive *) { return true; }
static void view_base_port(JcePostFXPipeline *pipeline, uint16_t base) { pipeline->view_base = base; }
static uint16_t frame_buffer_port(const JceOffscreenTarget *) { return 19; }
static bool jce_state_get_show_physics_debug() { return physics_debug; }
enum { JCE_SHOW_FLAG_UI = 1 };
static bool jce_state_show_flag(int) { return show_ui; }
static JceScene *jce_state_get_scene() { return current_scene; }
static uint16_t scene_view_id() { return 3; }
static_assert(std::is_same_v<decltype(&target_port), decltype(&jce_postfx_get_output_target)>);
static_assert(std::is_same_v<decltype(&hdr_port), decltype(&jce_offscreen_target_is_hdr)>);
static_assert(std::is_same_v<decltype(&canvas_component_port), decltype(&jce_scene_get_canvas)>);
static_assert(std::is_same_v<decltype(&backend_port), decltype(&jce_renderer_get_backend)>);
static_assert(std::is_same_v<decltype(&each_canvas_port), decltype(&jce_scene_each_canvas)>);
static_assert(std::is_same_v<decltype(&canvas_port), decltype(&jce_ui_canvas_render)>);
static_assert(std::is_same_v<decltype(&events_port), decltype(&jce_runtime_dispatch_ui_events)>);
static_assert(std::is_same_v<decltype(&composite_port), decltype(&jce_offscreen_target_composite_texture)>);
static_assert(std::is_same_v<decltype(&create_port), decltype(&jce_postfx_create)>);
static_assert(std::is_same_v<decltype(&allocator_port), decltype(&jce_allocator_default)>);

struct ViewState {
    JceOffscreenTarget *bridge = nullptr;
    JceRenderer *renderer = nullptr;
    JceUICanvas *ui_canvas = nullptr;
    void *present = nullptr;
    uint16_t postfx_output_tex = 11;
    JceWindow *window = nullptr;
    JceUIPointer ui_pointer{};
    bool ui_text_input_on = false;
    JceRuntime *play_runtime = nullptr;
    JcePostFXPipeline *postfx = nullptr;
};
static ViewState g, s_sr;
struct ScenePipelineOwner { JcePostFXPipeline *postfx_pipeline = nullptr; };

#define jce_offscreen_target_get_color_texture color_port
#define jce_offscreen_target_get_extent extent_port
#define jce_offscreen_target_get_frame_buffer frame_buffer_port
#define jce_offscreen_target_composite_texture composite_port
#define jce_renderer_origin_bottom_left origin_port
#define jce_renderer_get_backend backend_port
#define jce_scene_each_canvas each_canvas_port
#define jce_editor_request_viewport_screenshot shot_port
#define jce_ui_canvas_render canvas_port
#define jce_ui_canvas_focused_input focused_port
#define jce_window_start_text_input text_start_port
#define jce_window_stop_text_input text_stop_port
#define jce_runtime_dispatch_ui_events events_port
#define jce_postfx_get_output_target target_port
#define jce_offscreen_target_is_hdr hdr_port
#define jce_scene_get_canvas canvas_component_port
#define jce_postfx_create create_port
#define jce_postfx_load_shaders load_port
#define jce_postfx_set_view_base view_base_port
#define jce_allocator_default allocator_port
#define LOG_WARN(...) ((void)0)
#include JCE_TEST_VIEWPORT_COMPOSITE_OWNER

struct Fixture {
    JceOffscreenTarget bridge;
    JceOffscreenTarget scene_bridge;
    JceRenderer renderer;
    JceScene scene;
    unsigned canvas_token = 0, present_token = 0;
    Fixture()
    {
        bottom_left = physics_debug = false; show_ui = true;
        copies = queries = canvas_calls = event_calls = shot_calls = 0;
        copied_source = shot_source = shot_width = shot_height = 0;
        copied_flip = false; shot_yflip = -1;
        current_scene = &scene; g = {}; s_sr = {};
        scene_bridge.color = 8; s_sr.postfx_output_tex = 13;
        g.bridge = &bridge; s_sr.bridge = &scene_bridge; g.renderer = s_sr.renderer = &renderer;
        g.ui_canvas = s_sr.ui_canvas = reinterpret_cast<JceUICanvas *>(&canvas_token);
        creates = 0; pipelines[0] = {}; pipelines[1] = {};
        target_available = true; bridge_hdr = true;
        expected_canvas_fb = 19; canvas_fb = UINT16_MAX;
        bridge_width = 320; bridge_height = 180;
        target_info = {};
        target_info.struct_size = sizeof(target_info);
        target_info.version = JCE_POSTFX_OUTPUT_TARGET_VERSION;
        target_info.kind = JCE_POSTFX_OUTPUT_OWNED;
        target_info.width = 640; target_info.height = 360;
        target_info.format = JCE_POSTFX_OUTPUT_FORMAT_RGBA16F;
        target_info.texture.idx = 11; target_info.framebuffer = 23;
        g.postfx = &pipelines[0];
    }
    bool composite(bool canvas_enabled = true, bool bridge_tail = false, JceScene *owner = nullptr)
    {
        return jce_editor_viewport_composite_postfx(&bridge, 3, 11, 640, 360,
            &renderer, owner ? owner : &scene, canvas_enabled, bridge_tail);
    }
};

TEST_CASE("empty Canvas query elides copy and production Game tail keeps lifecycle")
{
    Fixture f;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 0); CHECK(queries == 1);
    CHECK(g.postfx_output_tex == 11);
    CHECK(canvas_calls == 1); CHECK(event_calls == 1);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "capture.png", 1));
    CHECK(shot_calls == 1); CHECK(shot_source == 11);
    CHECK(shot_width == 640); CHECK(shot_height == 360); CHECK(shot_yflip == 0);
}

TEST_CASE("any Canvas keeps production copy and bridge screenshot source")
{
    Fixture f; f.scene.canvases = 1;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 1); CHECK(copied_source == 11); CHECK_FALSE(copied_flip);
    CHECK(g.postfx_output_tex == UINT16_MAX);
    CHECK(canvas_calls == 1); CHECK(event_calls == 1);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "capture.png", 1));
    CHECK(shot_source == 7); CHECK(shot_width == 320); CHECK(shot_height == 180);
    CHECK(shot_yflip == 0);
}

TEST_CASE("OpenGL direct and composited screenshots preserve bottom-left orientation")
{
    Fixture f; f.renderer.backend = JCE_BACKEND_OPENGL; bottom_left = true;
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "direct.png", 1));
    CHECK(shot_source == 11); CHECK(shot_width == 640); CHECK(shot_height == 360);
    CHECK(shot_yflip == 1);
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 1); CHECK(copied_flip); CHECK(g.postfx_output_tex == UINT16_MAX);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "bridge.png", 1));
    CHECK(shot_source == 7); CHECK(shot_width == 320); CHECK(shot_height == 180);
    CHECK(shot_yflip == 1); CHECK(shot_calls == 2);
}

TEST_CASE("Canvas existence is fresh and conservative, including multiple components")
{
    Fixture f;
    CHECK_FALSE(f.composite());
    f.scene.canvases = 3; CHECK(f.composite());
    f.scene.canvases = 0; CHECK_FALSE(f.composite());
    CHECK(copies == 1); CHECK(queries == 3);
    CHECK_FALSE(f.composite(false)); CHECK(queries == 3);
    CHECK(jce_editor_viewport_composite_postfx(&f.bridge, 3, 11, 640, 360,
        &f.renderer, nullptr, true, false));
}

TEST_CASE("backend, orientation, unknown renderer and late bridge writers retain copy")
{
    Fixture f;
    f.renderer.backend = JCE_BACKEND_OPENGL; CHECK(f.composite());
    f.renderer.backend = JCE_BACKEND_D3D11; bottom_left = true; CHECK(f.composite());
    CHECK(copied_flip); bottom_left = false;
    CHECK(f.composite(true, true));
    CHECK(jce_editor_viewport_composite_postfx(&f.bridge, 3, 11, 640, 360,
        nullptr, &f.scene, false, false));
    CHECK(copies == 4); CHECK(queries == 0);
}

TEST_CASE("invalid or borrowed bridge output never copies an attachment into itself")
{
    Fixture f;
    CHECK_FALSE(jce_editor_viewport_composite_postfx(nullptr, 3, 11, 640, 360,
        &f.renderer, &f.scene, true, false));
    CHECK_FALSE(jce_editor_viewport_composite_postfx(&f.bridge, 3, UINT16_MAX, 640, 360,
        &f.renderer, &f.scene, true, false));
    CHECK_FALSE(jce_editor_viewport_composite_postfx(&f.bridge, 3, 7, 640, 360,
        &f.renderer, &f.scene, true, false));
    CHECK(copies == 0); CHECK(queries == 0);
}

TEST_CASE("production Game physics tail keeps copy without skipping Canvas retirement")
{
    Fixture f; physics_debug = true;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 1); CHECK(queries == 0); CHECK(g.postfx_output_tex == UINT16_MAX);
    CHECK(canvas_calls == 1); CHECK(event_calls == 1);
}

TEST_CASE("production Scene tail excludes actual scaled TSR and RCAS consumers")
{
    Fixture f; s_sr.present = &f.present_token;
    test_scene_composite(&f.scene, 640, 360, 1280, 720, true, 0);
    CHECK(copies == 1); CHECK(s_sr.postfx_output_tex == UINT16_MAX); CHECK(canvas_calls == 1);
    s_sr.postfx_output_tex = 13;
    test_scene_composite(&f.scene, 640, 360, 1280, 720, false, 1);
    CHECK(copies == 2); CHECK(s_sr.postfx_output_tex == UINT16_MAX); CHECK(canvas_calls == 2);
    s_sr.postfx_output_tex = 13;
    test_scene_composite(&f.scene, 640, 360, 640, 360, true, 1);
    CHECK(copies == 2); CHECK(s_sr.postfx_output_tex == 13); CHECK(canvas_calls == 3);
}

TEST_CASE("production Scene UI toggle preserves its original Canvas admission")
{
    Fixture f; f.scene.canvases = 1; show_ui = false;
    test_scene_composite(&f.scene, 640, 360, 640, 360, false, 0);
    CHECK(copies == 0); CHECK(queries == 0); CHECK(canvas_calls == 0);
    CHECK(s_sr.postfx_output_tex == 13);
    show_ui = true;
    test_scene_composite(&f.scene, 640, 360, 640, 360, false, 0);
    CHECK(copies == 1); CHECK(queries == 1); CHECK(canvas_calls == 1);
    CHECK(s_sr.postfx_output_tex == UINT16_MAX);
}

TEST_CASE("actual Scene and Game create wires acquire distinct PostFX producers")
{
    Fixture f; ScenePipelineOwner scene_owner;
    test_create_scene_postfx(&scene_owner, nullptr);
    test_create_game_postfx(nullptr);
    REQUIRE(creates == 2);
    CHECK(scene_owner.postfx_pipeline == &pipelines[0]);
    CHECK(g.postfx == &pipelines[1]);
    CHECK(scene_owner.postfx_pipeline != g.postfx);
    CHECK(g.postfx->view_base == GAME_VIEW_BASE + JCE_EDITOR_VP_POSTFX_BASE_OFFSET);
    test_scene_composite(&f.scene, 640, 360, 640, 360, false, 0);
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 0); CHECK(s_sr.postfx_output_tex == 13); CHECK(g.postfx_output_tex == 11);
    CHECK(jce_editor_viewport_screenshot_submit(&f.scene_bridge, 3, s_sr.postfx_output_tex,
        640, 360, "scene.png", 1));
    CHECK(shot_source == 13); CHECK(shot_yflip == 0);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "game.png", 1));
    CHECK(shot_source == 11); CHECK(shot_yflip == 0);
}

TEST_CASE("Game overlay uses the current complete PostFX target for Canvas display and capture")
{
    Fixture f; f.scene.canvases = 2;
    bridge_width = 640; bridge_height = 360; expected_canvas_fb = 23;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 0); CHECK(g.postfx_output_tex == 11);
    CHECK(canvas_fb == 23); CHECK(canvas_calls == 1); CHECK(event_calls == 1);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "overlay.png", 1));
    CHECK(shot_source == 11); CHECK(shot_width == 640); CHECK(shot_height == 360);
}

TEST_CASE("Game target handle zero is valid for both Canvas and screenshot")
{
    Fixture f; f.scene.canvases = 1;
    bridge_width = 640; bridge_height = 360;
    target_info.texture.idx = g.postfx_output_tex = 0;
    target_info.framebuffer = expected_canvas_fb = 0;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 0); CHECK(g.postfx_output_tex == 0); CHECK(canvas_fb == 0);
    CHECK(jce_editor_viewport_screenshot_submit(&f.bridge, 120, g.postfx_output_tex,
        640, 360, "zero.png", 1));
    CHECK(shot_source == 0);
}

TEST_CASE("Game output descriptor rejects unknown borrowed resized aliased or mismatched targets")
{
    for (unsigned failure = 0; failure != 13; ++failure) {
        Fixture f; f.scene.canvases = 1;
        bridge_width = 640; bridge_height = 360;
        switch (failure) {
        case 0: target_available = false; break;
        case 1: target_info.kind = JCE_POSTFX_OUTPUT_BORROWED; break;
        case 2: target_info.width = 320; break;
        case 3: target_info.height = 180; break;
        case 4: bridge_width = 320; break;
        case 5: target_info.format = JCE_POSTFX_OUTPUT_FORMAT_RGBA8; break;
        case 6: target_info.texture.idx = 7; break;
        case 7: target_info.framebuffer = 19; break;
        case 8: target_info.framebuffer = UINT16_MAX; break;
        case 9: target_info.texture.idx = 99; break;
        case 10: target_info.format = JCE_POSTFX_OUTPUT_FORMAT_UNKNOWN; break;
        case 11: g.postfx = nullptr; break;
        case 12: target_info.width = bridge_width = 320;
            target_info.height = bridge_height = 180; break;
        }
        test_game_composite(&f.scene, 640, 360);
        CHECK(copies == 1); CHECK(g.postfx_output_tex == UINT16_MAX);
        CHECK(canvas_fb == 19); CHECK(event_calls == 1);
    }
}

TEST_CASE("Game physics and non-overlay Canvas retain the legacy bridge with current output available")
{
    for (unsigned mode = 0; mode != 4; ++mode) {
        Fixture f; f.scene.canvases = 1;
        bridge_width = 640; bridge_height = 360;
        if (mode == 0) physics_debug = true;
        else f.scene.canvas.render_mode = mode == 1 ? JCE_CANVAS_CAMERA :
            mode == 2 ? JCE_CANVAS_WORLD : 99;
        test_game_composite(&f.scene, 640, 360);
        CHECK(copies == 1); CHECK(canvas_fb == 19);
        CHECK(g.postfx_output_tex == UINT16_MAX); CHECK(event_calls == 1);
    }
}

TEST_CASE("Game overlays also admit matching actual RGBA8 targets")
{
    Fixture f; f.scene.canvases = 1;
    bridge_width = 640; bridge_height = 360; bridge_hdr = false;
    target_info.format = JCE_POSTFX_OUTPUT_FORMAT_RGBA8; expected_canvas_fb = 23;
    test_game_composite(&f.scene, 640, 360);
    CHECK(copies == 0); CHECK(canvas_fb == 23); CHECK(g.postfx_output_tex == 11);
}
