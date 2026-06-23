/*
 * jce_editor_welcome_policy.cpp  Welcome startup policy.
 */

#include "jce_editor_welcome_policy.h"

bool jce_editor_welcome_should_open_on_startup(
    JceEditorStartupBehavior startup_behavior,
    bool has_current_project,
    bool has_known_project)
{
    if (has_current_project)
        return false;

    switch (startup_behavior) {
    case JCE_EDITOR_STARTUP_PICKER:
        return true;
    case JCE_EDITOR_STARTUP_EMPTY:
        return false;
    case JCE_EDITOR_STARTUP_LAST:
    default:
        return !has_known_project;
    }
}
