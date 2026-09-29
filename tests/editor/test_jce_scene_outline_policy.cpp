#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "scene/jce_scene_outline_policy.h"

TEST_CASE("visual renderers do not fall back to debug placeholder outlines")
{
    CHECK_FALSE(jce_editor_scene_outline_should_use_debug_fallback(true, false));
}

TEST_CASE("transform-only selections still get an editor handle outline")
{
    CHECK(jce_editor_scene_outline_should_use_debug_fallback(false, false));
}

TEST_CASE("a drawn visual outline suppresses debug fallback")
{
    CHECK_FALSE(jce_editor_scene_outline_should_use_debug_fallback(false, true));
    CHECK_FALSE(jce_editor_scene_outline_should_use_debug_fallback(true, true));
}
