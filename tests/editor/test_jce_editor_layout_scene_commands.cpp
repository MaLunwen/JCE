#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "ui/jce_editor_layout_scene_commands.h"
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_layout_internal.h"

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

namespace { PendingGatedAction requested_action = PGA_NONE; unsigned gate_calls; }
void request_gated_action(PendingGatedAction action, const char *)
{
    requested_action = action;
    ++gate_calls;
}
TEST_CASE("new project requests the existing unsaved-scene gate before opening")
{
    requested_action = PGA_NONE;
    gate_calls = 0;
    g_new_default_called = g_focus_scene_called = false;
    jce_editor_layout_request_new_project();
    CHECK(gate_calls == 1);
    CHECK(requested_action == PGA_NEW_PROJECT);
    CHECK_FALSE(g_new_default_called);
    CHECK_FALSE(g_focus_scene_called);
}
