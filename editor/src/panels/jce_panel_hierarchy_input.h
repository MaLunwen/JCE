/*
 * jce_panel_hierarchy_input.h  Pure keyboard-routing helpers.
 */

#ifndef JCE_PANEL_HIERARCHY_INPUT_H
#define JCE_PANEL_HIERARCHY_INPUT_H

#include "core/jce_hotkeys.h"

bool jce_hierarchy_alpha_jump_key_consumed(
    char typed,
    const JceHotkeyChord *consumed_chords,
    int consumed_chord_count);

#endif /* JCE_PANEL_HIERARCHY_INPUT_H */
