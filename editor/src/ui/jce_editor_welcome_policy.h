/*
 * jce_editor_welcome_policy.h  Pure startup policy for the Welcome screen.
 */

#ifndef JCE_EDITOR_WELCOME_POLICY_H
#define JCE_EDITOR_WELCOME_POLICY_H

#include "panels/jce_panel_preferences.h"

bool jce_editor_welcome_should_open_on_startup(
    JceEditorStartupBehavior startup_behavior,
    bool has_current_project,
    bool has_known_project);

#endif /* JCE_EDITOR_WELCOME_POLICY_H */
