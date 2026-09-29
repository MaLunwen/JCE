#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "panels/jce_panel_hierarchy_input.h"
#include "core/jce_hotkeys.h"
#include <jce/tools/jce_imgui.hpp>

TEST_CASE("alpha-jump ignores a plain hotkey consumed by the hierarchy panel")
{
    JceHotkeyChord consumed[] = {
        { ImGuiKey_F, JCE_HKM_NONE }
    };

    CHECK(jce_hierarchy_alpha_jump_key_consumed('f', consumed, 1));
    CHECK(jce_hierarchy_alpha_jump_key_consumed('F', consumed, 1));
}

TEST_CASE("alpha-jump ignores shifted consumed hotkeys too")
{
    JceHotkeyChord consumed[] = {
        { ImGuiKey_F, JCE_HKM_SHIFT }
    };

    CHECK(jce_hierarchy_alpha_jump_key_consumed('f', consumed, 1));
}

TEST_CASE("alpha-jump keeps unrelated characters available")
{
    JceHotkeyChord consumed[] = {
        { ImGuiKey_F, JCE_HKM_NONE }
    };

    CHECK_FALSE(jce_hierarchy_alpha_jump_key_consumed('g', consumed, 1));
}

TEST_CASE("alpha-jump does not treat ctrl/alt/super chords as typed letters")
{
    JceHotkeyChord consumed[] = {
        { ImGuiKey_F, JCE_HKM_CTRL },
        { ImGuiKey_G, JCE_HKM_ALT },
        { ImGuiKey_H, JCE_HKM_SUPER }
    };

    CHECK_FALSE(jce_hierarchy_alpha_jump_key_consumed('f', consumed, 3));
    CHECK_FALSE(jce_hierarchy_alpha_jump_key_consumed('g', consumed, 3));
    CHECK_FALSE(jce_hierarchy_alpha_jump_key_consumed('h', consumed, 3));
}
