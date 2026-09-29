/*
 * Test-only minimal stub for <jce/tools/jce_imgui.hpp>.
 *
 * The real header pulls in the entire Dear ImGui SDK plus the JCE Tools
 * backend.  For unit-testing pure helpers (such as the hotkey registry
 * lookup / chord-label formatting) that include this header transitively,
 * we only need a handful of enum values and two function symbols.
 *
 * SCOPE: do NOT extend this stub for production usage.  Only add what
 * a specific test absolutely requires.
 */

#ifndef JCE_TESTS_STUB_JCE_IMGUI_HPP
#define JCE_TESTS_STUB_JCE_IMGUI_HPP

#include <cstddef>
#include <cstdint>

using ImGuiKey      = int;
using ImGuiKeyChord = int;

/* Subset of ImGuiKey_ enum needed by editor/src/core/jce_hotkeys.cpp. */
enum : ImGuiKey {
    ImGuiKey_None   = 0,
    ImGuiKey_Space  = 533,
    ImGuiKey_0      = 536,
    ImGuiKey_1      = 537,
    ImGuiKey_2      = 538,
    ImGuiKey_3      = 539,
    ImGuiKey_4      = 540,
    ImGuiKey_5      = 541,
    ImGuiKey_6      = 542,
    ImGuiKey_7      = 543,
    ImGuiKey_8      = 544,
    ImGuiKey_9      = 545,
    ImGuiKey_A      = 546,
    ImGuiKey_B      = 547,
    ImGuiKey_C      = 548,
    ImGuiKey_D      = 549,
    ImGuiKey_E      = 550,
    ImGuiKey_F      = 551,
    ImGuiKey_G      = 552,
    ImGuiKey_H      = 553,
    ImGuiKey_I      = 554,
    ImGuiKey_J      = 555,
    ImGuiKey_K      = 556,
    ImGuiKey_L      = 557,
    ImGuiKey_M      = 558,
    ImGuiKey_N      = 559,
    ImGuiKey_O      = 560,
    ImGuiKey_P      = 561,
    ImGuiKey_Q      = 562,
    ImGuiKey_R      = 563,
    ImGuiKey_S      = 564,
    ImGuiKey_V      = 567,
    ImGuiKey_W      = 568,
    ImGuiKey_X      = 569,
    ImGuiKey_Y      = 570,
    ImGuiKey_Z      = 571,
    ImGuiKey_Comma  = 572,
    ImGuiKey_Minus  = 573,
    ImGuiKey_End    = 520,
    ImGuiKey_Backspace = 523,
    ImGuiKey_Delete = 524,
    ImGuiKey_Pause  = 532,
    ImGuiKey_F1     = 574,
    ImGuiKey_F2     = 575,
    ImGuiKey_F3     = 576,
    ImGuiKey_F4     = 577,
    ImGuiKey_F5     = 578,
    ImGuiKey_F6     = 579,
    ImGuiKey_F7     = 580,
    ImGuiKey_F8     = 581,
    ImGuiKey_F9     = 582,
    ImGuiKey_F10    = 583,
    ImGuiKey_F11    = 584,
    ImGuiKey_F12    = 585,
    ImGuiKey_Period = 586,
    ImGuiKey_Keypad0 = 599,
    ImGuiKey_Keypad1 = 600,
    ImGuiKey_Keypad3 = 602,
    ImGuiKey_Keypad5 = 604,
    ImGuiKey_Keypad7 = 606,
    ImGuiKey_Keypad9 = 608,
    ImGuiKey_KeypadDecimal = 609,
    ImGuiKey_KeypadSubtract = 611
};

/* Modifier bits ─ values match real ImGui so reading either is fine. */
enum : int {
    ImGuiMod_None  = 0,
    ImGuiMod_Ctrl  = 1 << 12,
    ImGuiMod_Shift = 1 << 13,
    ImGuiMod_Alt   = 1 << 14,
    ImGuiMod_Super = 1 << 15
};

namespace ImGui {

const char *GetKeyName(ImGuiKey key);
bool        IsKeyChordPressed(ImGuiKeyChord chord);
bool        IsKeyPressed(ImGuiKey key, bool repeat = true);
int         GetFrameCount();

}  /* namespace ImGui */

#endif  /* JCE_TESTS_STUB_JCE_IMGUI_HPP */
