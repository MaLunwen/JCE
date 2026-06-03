/*
 * jce_scene_outline_policy.cpp
 */

#include "jce_scene_outline_policy.h"

bool jce_editor_scene_outline_should_use_debug_fallback(
    bool has_visual_renderer,
    bool drew_visual_outline)
{
    return !has_visual_renderer && !drew_visual_outline;
}
