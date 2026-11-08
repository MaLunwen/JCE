/* Exact shared Scene/Game screenshot source selection with typed CPU ports.
 * These assertions observe request dimensions, not GPU pixels or completion. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <jce/renderer/jce_offscreen_target.h>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

struct JceOffscreenTarget {
    bool live = true;
    uint32_t width = 16, height = 8192;
    uint16_t color = 7;
};
static unsigned extent_calls, source_calls, submit_calls;
static bool origin_bottom_left, accept_submit = true;
static uint16_t captured_source, captured_view, captured_width, captured_height;
static unsigned captured_owner;
static int captured_yflip;
static const char *captured_path;

static bool extent_port(const JceOffscreenTarget *target, uint32_t *width, uint32_t *height)
{
    ++extent_calls;
    *width = target->live ? target->width : 0;
    *height = target->live ? target->height : 0;
    return target->live;
}
static_assert(std::is_same_v<decltype(&extent_port), decltype(&jce_offscreen_target_get_extent)>);
static uint16_t source_port(const JceOffscreenTarget *target)
{ ++source_calls; return target->color; }
static bool origin_port() { return origin_bottom_left; }
static bool submit_port(unsigned owner, uint16_t source, uint16_t view,
    uint16_t width, uint16_t height, const char *path, int yflip)
{
    ++submit_calls;
    captured_owner = owner; captured_source = source; captured_view = view;
    captured_width = width; captured_height = height;
    captured_path = path; captured_yflip = yflip;
    return accept_submit;
}
#define jce_offscreen_target_get_extent extent_port
#define jce_offscreen_target_get_color_texture source_port
#define jce_renderer_origin_bottom_left origin_port
#define jce_editor_request_viewport_screenshot submit_port
#include JCE_TEST_VIEWPORT_CAPTURE_OWNER
#undef jce_offscreen_target_get_extent
#undef jce_offscreen_target_get_color_texture
#undef jce_renderer_origin_bottom_left
#undef jce_editor_request_viewport_screenshot

struct Fixture {
    JceOffscreenTarget target;
    const char *path = "capture.png";
    Fixture()
    {
        extent_calls = source_calls = submit_calls = 0;
        origin_bottom_left = false; accept_submit = true;
        captured_source = captured_view = captured_width = captured_height = 0;
        captured_owner = 0; captured_yflip = -1; captured_path = nullptr;
    }
};

TEST_CASE_FIXTURE(Fixture, "Scene and Game bridge captures use admitted asymmetric dimensions")
{
    for (unsigned owner : {2u, 3u}) {
        CHECK(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
            1, 9000, path, owner));
        CHECK(captured_width == 16); CHECK(captured_height == 8192);
        CHECK(captured_source == 7); CHECK(captured_owner == owner);
        CHECK(captured_view == 3 + JCE_EDITOR_VP_SCREENSHOT_OFFSET);
        CHECK(captured_path == path); CHECK(captured_yflip == 0);
    }
    CHECK(extent_calls == 2); CHECK(submit_calls == 2);
    origin_bottom_left = true;
    CHECK(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        1, 9000, path, 2));
    CHECK(captured_yflip == 1);
}

TEST_CASE_FIXTURE(Fixture, "PostFX source retains its own requested extent and orientation")
{
    CHECK(jce_editor_viewport_screenshot_submit(&target, 120, 9,
        640, 360, path, 3));
    CHECK(extent_calls == 0); CHECK(source_calls == 0);
    CHECK(captured_width == 640); CHECK(captured_height == 360);
    CHECK(captured_source == 9); CHECK(captured_yflip == 0);
    origin_bottom_left = true;
    CHECK(jce_editor_viewport_screenshot_submit(&target, 120, 9,
        640, 360, path, 3));
    CHECK(extent_calls == 0); CHECK(source_calls == 0);
    CHECK(captured_width == 640); CHECK(captured_height == 360);
    CHECK(captured_source == 9); CHECK(captured_yflip == 1);
}

TEST_CASE_FIXTURE(Fixture, "missing bridge or live target cannot publish a readback request")
{
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(nullptr, 3, UINT16_MAX,
        640, 360, path, 2));
    CHECK(extent_calls == 0); CHECK(submit_calls == 0);
    target.live = false;
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        640, 360, path, 2));
    CHECK(extent_calls == 1); CHECK(submit_calls == 0);
    target.live = true; target.color = UINT16_MAX;
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        640, 360, path, 2));
    CHECK(submit_calls == 0);
}

TEST_CASE_FIXTURE(Fixture, "bridge source observes resize and propagates submission refusal")
{
    target.width = 96; target.height = 48; accept_submit = false;
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        64, 64, path, 2));
    CHECK(captured_width == 96); CHECK(captured_height == 48);
    CHECK(submit_calls == 1);
}

TEST_CASE_FIXTURE(Fixture, "unrepresentable bridge dimensions refuse narrowing")
{
    target.width = 65536;
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        64, 64, path, 2));
    target.width = 16; target.height = 0;
    CHECK_FALSE(jce_editor_viewport_screenshot_submit(&target, 3, UINT16_MAX,
        64, 64, path, 2));
    CHECK(submit_calls == 0);
}
