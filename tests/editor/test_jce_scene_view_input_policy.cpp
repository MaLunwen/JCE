#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "panels/jce_scene_view_input_policy.h"

TEST_CASE("scene selection starts only from a viewport-owned left click")
{
    CHECK(jce_scene_view_should_start_selection(true, false, false, false));

    CHECK_FALSE(jce_scene_view_should_start_selection(false, false, false, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, true, false, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, false, true, false));
    CHECK_FALSE(jce_scene_view_should_start_selection(true, false, false, true));
}

TEST_CASE("continuous left input belongs to the scene view only while its item owns it")
{
    CHECK(jce_scene_view_left_input_belongs_to_viewport(true, false, true));
    CHECK(jce_scene_view_left_input_belongs_to_viewport(false, true, true));

    CHECK_FALSE(jce_scene_view_left_input_belongs_to_viewport(false, false, true));
    CHECK_FALSE(jce_scene_view_left_input_belongs_to_viewport(false, true, false));
}

TEST_CASE("external left clicks cancel deferred scene picking")
{
    CHECK(jce_scene_view_should_cancel_deferred_pick(true, false, false));

    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(false, false, false));
    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(true, true, false));
    CHECK_FALSE(jce_scene_view_should_cancel_deferred_pick(true, false, true));
}

TEST_CASE("scene context menu opens only from a viewport-owned right click")
{
    CHECK(jce_scene_view_should_open_context_menu(true, false));

    CHECK_FALSE(jce_scene_view_should_open_context_menu(false, false));
    CHECK_FALSE(jce_scene_view_should_open_context_menu(true, true));
}
