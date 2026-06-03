/*
 * jce_panel_hierarchy_input.cpp
 */

#include "jce_panel_hierarchy_input.h"

#include <jce/tools/jce_imgui.hpp>

static char hierarchy_ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static char hierarchy_alpha_jump_char_from_key(int key)
{
    if (key >= ImGuiKey_A && key <= ImGuiKey_Z)
        return (char)('a' + (key - ImGuiKey_A));
    if (key >= ImGuiKey_0 && key <= ImGuiKey_9)
        return (char)('0' + (key - ImGuiKey_0));
    if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9)
        return (char)('0' + (key - ImGuiKey_Keypad0));
    if (key == ImGuiKey_Space)
        return ' ';
    if (key == ImGuiKey_Minus || key == ImGuiKey_KeypadSubtract)
        return '-';
    if (key == ImGuiKey_Period || key == ImGuiKey_KeypadDecimal)
        return '.';
    return '\0';
}

bool jce_hierarchy_alpha_jump_key_consumed(
    char typed,
    const JceHotkeyChord *consumed_chords,
    int consumed_chord_count)
{
    if (!typed || !consumed_chords || consumed_chord_count <= 0)
        return false;

    char want = hierarchy_ascii_tolower(typed);
    for (int i = 0; i < consumed_chord_count; i++) {
        JceHotkeyChord chord = consumed_chords[i];
        if (chord.key <= 0)
            continue;
        if (chord.mods & (JCE_HKM_CTRL | JCE_HKM_ALT | JCE_HKM_SUPER))
            continue;

        char got = hierarchy_alpha_jump_char_from_key(chord.key);
        if (got && hierarchy_ascii_tolower(got) == want)
            return true;
    }

    return false;
}
