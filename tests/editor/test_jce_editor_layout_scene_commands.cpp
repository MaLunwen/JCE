#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "ui/jce_editor_layout_scene_commands.h"

namespace {
bool g_new_default_called = false;
bool g_focus_scene_called = false;
bool g_new_default_result = true;
} /* namespace */

extern "C" bool jce_state_new_default_scene(void)
{
    g_new_default_called = true;
    return g_new_default_result;
}

extern "C" void jce_editor_layout_request_focus_scene_view(void)
{
    g_focus_scene_called = true;
}

TEST_CASE("new scene command creates an untitled default scene directly")
{
    g_new_default_called = false;
    g_focus_scene_called = false;
    g_new_default_result = true;

    CHECK(jce_editor_layout_run_new_scene_command());
    CHECK(g_new_default_called);
    CHECK(g_focus_scene_called);
}

TEST_CASE("new scene command does not focus scene view when creation fails")
{
    g_new_default_called = false;
    g_focus_scene_called = false;
    g_new_default_result = false;

    CHECK(!jce_editor_layout_run_new_scene_command());
    CHECK(g_new_default_called);
    CHECK(!g_focus_scene_called);
}
