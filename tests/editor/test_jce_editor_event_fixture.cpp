#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/core/jce_editor_event_fixture.h"
#include <limits>

TEST_CASE("F12 QA keyboard payload enters normal key event representation") {
    JceEvent event = {};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(jce_editor_fixture_event('K', nan, nan, JCE_KEY_F12, 1,
        0, 0, 0, 0, &event));
    CHECK(event.type == JCE_EVENT_KEY_DOWN);
    CHECK(event.key.scancode == 69);
    CHECK(event.key.mod == JCE_KMOD_NONE);
    CHECK_FALSE(event.key.repeat);
    REQUIRE(jce_editor_fixture_event('K', 0, 0, 69, 0, 0, 0, 0, 0, &event));
    CHECK(event.type == JCE_EVENT_KEY_UP);
    CHECK(event.key.scancode == JCE_KEY_F12);
}

TEST_CASE("invalid key and down values reject before narrowing and preserve output") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (double code : {nan, inf, -inf, -1.0, 0.0, 69.5, 512.0, 1e30}) {
        JceEvent event = {}; event.type = JCE_EVENT_QUIT;
        CHECK_FALSE(jce_editor_fixture_event('K', 0, 0, code, 1, 0, 0, 1, 1, &event));
        CHECK(event.type == JCE_EVENT_QUIT);
    }
    for (double down : {nan, inf, -1.0, 0.5, 2.0}) {
        JceEvent event = {}; event.type = JCE_EVENT_QUIT;
        CHECK_FALSE(jce_editor_fixture_event('K', 0, 0, 69, down, 0, 0, 1, 1, &event));
        CHECK(event.type == JCE_EVENT_QUIT);
    }
    CHECK_FALSE(jce_editor_fixture_event('K', 0, 0, 69, 1, 0, 0, 1, 1, nullptr));
}

TEST_CASE("existing motion button and wheel fixture mappings remain unchanged") {
    JceEvent event = {};
    REQUIRE(jce_editor_fixture_event('M', .25, .5, .1, -.2, 10, 20, 200, 100, &event));
    CHECK(event.type == JCE_EVENT_MOUSE_MOTION);
    CHECK(event.motion.x == 60); CHECK(event.motion.y == 70);
    CHECK(event.motion.xrel == 20); CHECK(event.motion.yrel == -20);
    REQUIRE(jce_editor_fixture_event('B', .25, .5, 1, 1, 10, 20, 200, 100, &event));
    CHECK(event.type == JCE_EVENT_MOUSE_BUTTON_DOWN);
    CHECK(event.button.button == JCE_MOUSE_BUTTON_LEFT);
    CHECK(event.button.x == 60); CHECK(event.button.y == 70);
    REQUIRE(jce_editor_fixture_event('B', 0, 0, 3, 0, 10, 20, 200, 100, &event));
    CHECK(event.type == JCE_EVENT_MOUSE_BUTTON_UP);
    REQUIRE(jce_editor_fixture_event('W', 0, 0, -2, 0, 0, 0, 0, 0, &event));
    CHECK(event.type == JCE_EVENT_MOUSE_WHEEL);
    CHECK(event.wheel.x == 0); CHECK(event.wheel.y == -2);
    CHECK_FALSE(jce_editor_fixture_event('Q', 0, 0, 0, 0, 0, 0, 1, 1, &event));
}

TEST_CASE("fixture frame and pointer narrowing reject invalid finite ranges") {
    unsigned frame = 17;
    for (double value : {-1.0, .5, 4294967296.0,
                         std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_FALSE(jce_editor_fixture_frame(value, &frame));
        CHECK(frame == 17);
    }
    REQUIRE(jce_editor_fixture_frame(4294967295.0, &frame));
    CHECK(frame == UINT_MAX);
    CHECK_FALSE(jce_editor_fixture_frame(0, nullptr));
    JceEvent event = {}; event.type = JCE_EVENT_QUIT;
    CHECK_FALSE(jce_editor_fixture_event('M', 1e300, 0, 0, 0, 0, 0, 200, 100, &event));
    CHECK_FALSE(jce_editor_fixture_event('M', 0, 0, 1e300, 0, 0, 0, 200, 100, &event));
    CHECK_FALSE(jce_editor_fixture_event('B', 0, 0, 1.5, 1, 0, 0, 200, 100, &event));
    CHECK_FALSE(jce_editor_fixture_event('B', 0, 0, 1, 2, 0, 0, 200, 100, &event));
    CHECK_FALSE(jce_editor_fixture_event('W', 0, 0,
        std::numeric_limits<double>::infinity(), 0, 0, 0, 0, 0, &event));
    CHECK(event.type == JCE_EVENT_QUIT);
}
