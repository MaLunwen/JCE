// test_jce_hotkeys.cpp — registry-side hotkey tests (no ImGui context).
//
// We do NOT call jce_hotkeys_init() because that loads from a real JSON
// file on disk (`.jce/hotkeys.json`).  Instead we exercise the pure
// helpers that operate on the static table directly.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "core/jce_hotkeys.h"
#include <jce/tools/jce_imgui.hpp>  /* stubbed for tests; provides ImGuiKey_* */

#include <cstring>

extern "C" void jce_test_imgui_set_pressed(ImGuiKeyChord chord,
                                            ImGuiKey key,
                                            int frame);

TEST_CASE("jce_hotkey_id_string returns the canonical JSON id")
{
    CHECK(std::strcmp(jce_hotkey_id_string(JCE_HK_FILE_SAVE), "file.save") == 0);
    CHECK(std::strcmp(jce_hotkey_id_string(JCE_HK_EDIT_UNDO), "edit.undo") == 0);
    CHECK(std::strcmp(jce_hotkey_id_string(JCE_HK_PLAY_TOGGLE), "play.toggle") == 0);
}

TEST_CASE("jce_hotkey_name returns the human-readable label")
{
    CHECK(std::strcmp(jce_hotkey_name(JCE_HK_FILE_SAVE), "File / Save") == 0);
    CHECK(std::strcmp(jce_hotkey_name(JCE_HK_EDIT_COPY), "Edit / Copy") == 0);
}

TEST_CASE("invalid hotkey id returns empty string")
{
    CHECK(jce_hotkey_id_string(static_cast<JceHotkeyId>(-1))[0] == '\0');
    CHECK(jce_hotkey_id_string(static_cast<JceHotkeyId>(JCE_HK_COUNT))[0] == '\0');
    CHECK(jce_hotkey_name(static_cast<JceHotkeyId>(-1))[0] == '\0');
    CHECK(jce_hotkey_name(static_cast<JceHotkeyId>(JCE_HK_COUNT))[0] == '\0');
}

TEST_CASE("invalid hotkey id yields zero-chord on get / get_default")
{
    JceHotkeyChord c = jce_hotkey_get(static_cast<JceHotkeyId>(-1));
    CHECK(c.key  == 0);
    CHECK(c.mods == 0);
    JceHotkeyChord d = jce_hotkey_get_default(static_cast<JceHotkeyId>(JCE_HK_COUNT));
    CHECK(d.key  == 0);
    CHECK(d.mods == 0);
}

TEST_CASE("jce_hotkeys_reset_all installs defaults into the live chord slot")
{
    /* Make sure the current binding is different from the default first. */
    jce_hotkey_set(JCE_HK_EDIT_UNDO, JceHotkeyChord{ 999, 0 });
    REQUIRE(jce_hotkey_get(JCE_HK_EDIT_UNDO).key == 999);

    jce_hotkeys_reset_all();

    JceHotkeyChord def = jce_hotkey_get_default(JCE_HK_EDIT_UNDO);
    JceHotkeyChord cur = jce_hotkey_get(JCE_HK_EDIT_UNDO);
    CHECK(cur.key  == def.key);
    CHECK(cur.mods == def.mods);
    CHECK(def.mods == JCE_HKM_CTRL);  /* Default = Ctrl+Z. */
}

TEST_CASE("jce_hotkey_set / jce_hotkey_get round-trip")
{
    JceHotkeyChord want{ 1234, (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT) };
    jce_hotkey_set(JCE_HK_EDIT_REDO, want);
    JceHotkeyChord got = jce_hotkey_get(JCE_HK_EDIT_REDO);
    CHECK(got.key  == want.key);
    CHECK(got.mods == want.mods);
    /* Out-of-range writes are silently dropped (no crash, no side effect). */
    JceHotkeyChord before = jce_hotkey_get(JCE_HK_EDIT_REDO);
    jce_hotkey_set(static_cast<JceHotkeyId>(JCE_HK_COUNT), want);
    JceHotkeyChord after = jce_hotkey_get(JCE_HK_EDIT_REDO);
    CHECK(after.key  == before.key);
    CHECK(after.mods == before.mods);

    /* Restore. */
    jce_hotkeys_reset_all();
}

TEST_CASE("jce_hotkey_chord_label formats modifier+key")
{
    char buf[64] = {0};
    JceHotkeyChord ctrl_s{ ImGuiKey_S, JCE_HKM_CTRL };
    jce_hotkey_chord_label(ctrl_s, buf, sizeof(buf));
    CHECK(std::strstr(buf, "Ctrl+") != nullptr);
    CHECK(std::strstr(buf, "S")     != nullptr);
}

TEST_CASE("jce_hotkey_chord_label includes every active modifier in order")
{
    char buf[64] = {0};
    JceHotkeyChord c{
        ImGuiKey_Z,
        (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT | JCE_HKM_ALT)
    };
    jce_hotkey_chord_label(c, buf, sizeof(buf));
    /* Order is Ctrl, Shift, Alt, Super per the implementation. */
    const char *ctrl  = std::strstr(buf, "Ctrl+");
    const char *shift = std::strstr(buf, "Shift+");
    const char *alt   = std::strstr(buf, "Alt+");
    REQUIRE(ctrl  != nullptr);
    REQUIRE(shift != nullptr);
    REQUIRE(alt   != nullptr);
    CHECK(ctrl < shift);
    CHECK(shift < alt);
}

TEST_CASE("jce_hotkey_chord_label reports unbound chords")
{
    char buf[64] = {0};
    JceHotkeyChord none{ 0, 0 };
    jce_hotkey_chord_label(none, buf, sizeof(buf));
    CHECK(std::strstr(buf, "unbound") != nullptr);
}

TEST_CASE("jce_hotkey_chord_label is null/zero-cap safe")
{
    JceHotkeyChord c{ ImGuiKey_A, JCE_HKM_CTRL };
    /* Must not crash; should return the (null) buffer it was given. */
    char *r = jce_hotkey_chord_label(c, nullptr, 0);
    CHECK(r == nullptr);
}

TEST_CASE("jce_hotkey_pressed requires the bound key edge, not only a chord report")
{
    jce_hotkeys_reset_all();

    ImGuiKeyChord ctrl_o = ImGuiMod_Ctrl | ImGuiKey_O;
    jce_test_imgui_set_pressed(ctrl_o, ImGuiKey_None, 100);
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_FILE_OPEN));

    jce_test_imgui_set_pressed(ctrl_o, ImGuiKey_O, 101);
    CHECK(jce_hotkey_pressed(JCE_HK_FILE_OPEN));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_FILE_OPEN));
}
