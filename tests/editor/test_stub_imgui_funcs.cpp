// test_stub_imgui_funcs.cpp — minimal definitions for the ImGui stub header.
//
// Linked into test exes that compile editor TUs which call into ImGui.
// We only implement the two symbols hotkeys.cpp actually invokes; both
// return safe defaults that exercise the surrounding pure logic.

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>
#include <cstring>

namespace ImGui {

static ImGuiKeyChord g_pressed_chord = ImGuiKey_None;
static ImGuiKey      g_pressed_key   = ImGuiKey_None;
static int           g_frame_count   = 1;

const char *GetKeyName(ImGuiKey key)
{
    /* Tiny rotating buffer so successive calls don't clobber each other.
       The hotkey label builder copies into its own char[] immediately, so
       a 4-slot ring is plenty for any reasonable test pattern. */
    static char ring[4][16];
    static unsigned idx = 0;
    char *slot = ring[idx & 3];
    idx++;

    switch (key) {
        case ImGuiKey_A:        std::strcpy(slot, "A");        break;
        case ImGuiKey_S:        std::strcpy(slot, "S");        break;
        case ImGuiKey_Z:        std::strcpy(slot, "Z");        break;
        case ImGuiKey_F2:       std::strcpy(slot, "F2");       break;
        case ImGuiKey_Backspace: std::strcpy(slot, "Backspace"); break;
        case ImGuiKey_Delete:   std::strcpy(slot, "Delete");   break;
        case ImGuiKey_Pause:    std::strcpy(slot, "Pause");    break;
        case ImGuiKey_None:     slot[0] = '\0';                break;
        default:
            std::snprintf(slot, sizeof(ring[0]), "Key%d", (int)key);
            break;
    }
    return slot;
}

bool IsKeyChordPressed(ImGuiKeyChord chord)
{
    return chord != ImGuiKey_None && chord == g_pressed_chord;
}

bool IsKeyPressed(ImGuiKey key, bool)
{
    return key == g_pressed_key;
}

int GetFrameCount()
{
    return g_frame_count;
}

}  // namespace ImGui

extern "C" void jce_test_imgui_set_pressed(ImGuiKeyChord chord,
                                            ImGuiKey key,
                                            int frame)
{
    ImGui::g_pressed_chord = chord;
    ImGui::g_pressed_key = key;
    ImGui::g_frame_count = frame;
}

/* jce_hotkey_name() localizes display names via the editor i18n table
 * (b3103625); tests have no i18n catalog, so the fallback IS the answer. */
extern "C" const char *jce_editor_i18n_or(const char *key, const char *fallback)
{
    (void)key;
    return fallback;
}
