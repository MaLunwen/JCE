/*
 * jce_editor_layout_scene_commands.cpp
 */

#include "jce_editor_layout_scene_commands.h"

#include "core/jce_editor_state.h"
#include "ui/jce_editor_layout.h"

extern "C" bool jce_editor_layout_run_new_scene_command(void)
{
    if (!jce_state_new_default_scene())
        return false;

    jce_editor_layout_request_focus_scene_view();
    return true;
}
