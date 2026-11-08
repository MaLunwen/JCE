/* Real registry, JSON, host files and ImGui CPU context; no window or GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_owned_directory.h"
#include "jce_test_imgui_context.h"
#include "core/jce_hotkeys.h"
#include <jce/os/core/jce_json.h>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

extern "C" const char *jce_editor_i18n_or(const char *, const char *fallback)
{
    return fallback;
}

namespace {
struct Owner {
    JceTestOwnedDirectory directory{"_ut_hotkey_persistence"};
    JceTestEnvironment profile{"JCE_EDITOR_PROFILE_DIR", directory.root.c_str()};
    JceTestImGuiContext context;
    Owner() { jce_hotkeys_shutdown(); jce_hotkeys_init(); }
    ~Owner() { jce_hotkeys_shutdown(); }
    std::string path() const { return directory.path("hotkeys.json"); }
    void write(const std::string &text)
    {
        REQUIRE(jce_fs_host_write_all(path().c_str(), text.data(), text.size()));
    }
};

void check_binding(JceHotkeyId id, JceHotkeyChord expected)
{
    const auto actual = jce_hotkey_get(id);
    CHECK(actual.key == expected.key);
    CHECK(actual.mods == expected.mods);
}

std::string row(const char *id, const std::string &key,
                const std::string &mods = "0")
{
    return "{\"id\":\"" + std::string(id) + "\",\"key\":" + key +
           ",\"mods\":" + mods + "}";
}

std::string document(const std::string &rows)
{
    return "{\"hotkeys\":[" + rows + "]}";
}
}

TEST_CASE("actual hotkey persistence saves and reloads a fresh owner")
{
    Owner owner;
    CHECK_FALSE(jce_hotkeys_load()); // A fresh profile has no file.
    check_binding(JCE_HK_FILE_SAVE, jce_hotkey_get_default(JCE_HK_FILE_SAVE));
    const JceHotkeyChord bound{ImGuiKey_Period,
        (uint8_t)(JCE_HKM_CTRL | JCE_HKM_SHIFT | JCE_HKM_ALT | JCE_HKM_SUPER)};
    const JceHotkeyChord unbound{ImGuiKey_None, JCE_HKM_SHIFT};
    const JceHotkeyChord modifier{ImGuiKey_LeftCtrl, JCE_HKM_ALT};
    jce_hotkey_set(JCE_HK_FILE_SAVE, bound);
    jce_hotkey_set(JCE_HK_FILE_OPEN, unbound);
    jce_hotkey_set(JCE_HK_EDIT_COPY, modifier);
    REQUIRE(jce_hotkeys_save());
    JceJson *saved = jce_json_parse_file(owner.path().c_str());
    REQUIRE(saved != nullptr);
    CHECK(jce_json_array_size(jce_json_get(saved, "hotkeys")) == JCE_HK_COUNT);
    jce_json_free(saved);
    jce_hotkeys_shutdown();
    jce_hotkeys_reset_all();
    jce_hotkeys_init(); // Real owner defaults then actual host JSON load.
    check_binding(JCE_HK_FILE_SAVE, bound);
    check_binding(JCE_HK_FILE_OPEN, unbound);
    check_binding(JCE_HK_EDIT_COPY, modifier);

    char label[64];
    jce_hotkey_chord_label(jce_hotkey_get(JCE_HK_FILE_OPEN), label, sizeof(label));
    CHECK(std::strcmp(label, "(unbound)") == 0);
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftShift, true);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_O, true);
    owner.context.begin();
    CHECK(ImGui::GetIO().KeyShift);
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_FILE_OPEN));
    owner.context.end();

    owner.write(document(row("future.unknown_action", "{\"future\":true}", "null") +
        ",{\"id\":\"file.save\",\"mods\":\"ignored-without-key\"}," +
        "{\"id\":\"file.new\",\"key\":" + std::to_string(ImGuiKey_A) + "}"));
    REQUIRE(jce_hotkeys_load());
    check_binding(JCE_HK_FILE_SAVE, bound); // Missing key is still a no-op.
    check_binding(JCE_HK_FILE_NEW, {ImGuiKey_A, JCE_HKM_NONE}); // Missing mods = 0.
    check_binding(JCE_HK_FILE_OPEN, unbound);
    owner.write("{\"hotkeys\":[]}");
    CHECK(jce_hotkeys_load());
    check_binding(JCE_HK_FILE_SAVE, bound);
}

TEST_CASE("actual hotkey JSON refuses malformed admission without partial mutation")
{
    Owner owner;
    const JceHotkeyChord retained{ImGuiKey_Period, JCE_HKM_SUPER};
    const std::string earlier = row("file.save", std::to_string(ImGuiKey_B), "1");
    std::vector<std::string> invalid{
        "[]", "true", "1", "null", "{}", "{\"hotkeys\":{}}",
        "{\"hotkeys\":null}", "{\"hotkeys\":true}",
        document("null"), document("[]"), document("5"),
        document("{}"), document("{\"id\":3,\"key\":0}"),
        document("{\"id\":\"\",\"key\":0}"),
        document(earlier) + " trailing"
    };
    const std::vector<std::string> keys{
        "-1", "1", std::to_string(ImGuiKey_NamedKey_BEGIN - 1),
        std::to_string(ImGuiKey_NamedKey_END),
        std::to_string(ImGuiKey_ReservedForModCtrl),
        std::to_string(ImGuiKey_ReservedForModShift),
        std::to_string(ImGuiKey_ReservedForModAlt),
        std::to_string(ImGuiKey_ReservedForModSuper),
        std::to_string(INT_MAX), "4294967296", "1e999",
        std::to_string(ImGuiKey_S) + ".5", "true", "null", "\"512\"", "{}"
    };
    for (const auto &key : keys)
        invalid.push_back(document(earlier + "," + row("file.open", key)));
    const std::vector<std::string> mods{
        "-1", "16", "255", "256", "257", "4294967296",
        "1.5", "1e999", "true", "null", "\"1\"", "{}"
    };
    for (const auto &mod : mods)
        invalid.push_back(document(earlier + "," +
            row("file.open", std::to_string(ImGuiKey_O), mod)));
    for (const auto &text : invalid) {
        INFO("actual input: " << text);
        jce_hotkeys_reset_all();
        jce_hotkey_set(JCE_HK_FILE_SAVE, retained);
        const auto open = jce_hotkey_get(JCE_HK_FILE_OPEN);
        owner.write(text);
        CHECK_FALSE(jce_hotkeys_load());
        check_binding(JCE_HK_FILE_SAVE, retained);
        check_binding(JCE_HK_FILE_OPEN, open);
        /* Old loader negative can leave an unsafe key in the real table.
         * Dispatch must refuse it before any actual ImGui key query. */
        if (jce_hotkey_get(JCE_HK_FILE_OPEN).key >= ImGuiKey_NamedKey_END) {
            owner.context.begin();
            CHECK_FALSE(jce_hotkey_pressed(JCE_HK_FILE_OPEN));
            owner.context.end();
        }
    }
    owner.write(std::string((1 << 20) + 1, ' ') + "{}");
    CHECK_FALSE(jce_hotkeys_load());
    check_binding(JCE_HK_FILE_SAVE, retained);
}

TEST_CASE("actual registry set and labels reject unsafe chords before ImGui")
{
    Owner owner;
    const JceHotkeyChord retained{ImGuiKey_Period, JCE_HKM_SUPER};
    const JceHotkeyChord invalid[]{
        {-1, 0}, {1, 0}, {ImGuiKey_NamedKey_BEGIN - 1, 0},
        {ImGuiKey_NamedKey_END, 0}, {INT_MAX, 0},
        {ImGuiKey_ReservedForModCtrl, 0}, {ImGuiKey_ReservedForModShift, 0},
        {ImGuiKey_ReservedForModAlt, 0}, {ImGuiKey_ReservedForModSuper, 0},
        {ImGuiKey_S, 16}, {ImGuiKey_S, 255}, {ImGuiKey_None, 16}
    };
    for (const auto chord : invalid) {
        INFO("key=" << chord.key << " mods=" << (int)chord.mods);
        jce_hotkey_set(JCE_HK_FILE_SAVE, retained);
        jce_hotkey_set(JCE_HK_FILE_SAVE, chord);
        check_binding(JCE_HK_FILE_SAVE, retained);
        char label[64] = "unchanged";
        jce_hotkey_chord_label(chord, label, sizeof(label));
        CHECK(label[0] == '\0');
        /* Reset through a legal value even in the ordinary old-set control. */
        jce_hotkey_set(JCE_HK_FILE_SAVE, retained);
    }
    const ImGuiKey physical_modifiers[]{ImGuiKey_LeftCtrl, ImGuiKey_RightCtrl,
        ImGuiKey_LeftShift, ImGuiKey_RightShift, ImGuiKey_LeftAlt,
        ImGuiKey_RightAlt, ImGuiKey_LeftSuper, ImGuiKey_RightSuper};
    for (const auto key : physical_modifiers) {
        jce_hotkey_set(JCE_HK_FILE_SAVE, {key, JCE_HKM_NONE});
        check_binding(JCE_HK_FILE_SAVE, {key, JCE_HKM_NONE});
    }
}

TEST_CASE("actual hotkey writer reports failure and can retry without losing state")
{
    Owner owner;
    const JceHotkeyChord retained{ImGuiKey_Period, JCE_HKM_CTRL};
    jce_hotkey_set(JCE_HK_FILE_SAVE, retained);
    /* A real directory at the destination refuses atomic file replacement. */
    REQUIRE(jce_fs_host_create_directory(owner.path().c_str()));
    const auto sentinel = owner.path() + "/sentinel";
    REQUIRE(jce_fs_host_write_all(sentinel.c_str(), "keep", 4));
    CHECK_FALSE(jce_hotkeys_save());
    check_binding(JCE_HK_FILE_SAVE, retained);
    CHECK(jce_fs_host_exists_file(sentinel.c_str()));
    REQUIRE(jce_fs_host_remove_recursive(owner.path().c_str()));
    REQUIRE(jce_hotkeys_save());
    jce_hotkeys_shutdown();
    jce_hotkeys_init();
    check_binding(JCE_HK_FILE_SAVE, retained);
}
