#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include "core/jce_hotkeys.h"
#include "core/jce_editor_hotkey_diag.h"
#include "core/jce_editor_event_fixture.h"
#ifndef JCE_TEST_EDITOR_KEY_EVENT_HEADER
#define JCE_TEST_EDITOR_KEY_EVENT_HEADER "core/jce_editor_key_event.h"
#endif
#include JCE_TEST_EDITOR_KEY_EVENT_HEADER

/* Only localization is a counted fallback port. ImGui events, NewFrame,
 * edge/modifier queries and the compiled hotkey owner are real. */
extern "C" const char *jce_editor_i18n_or(const char *, const char *fallback)
{
    return fallback;
}
static void submit(JceKey scan, ImGuiKey mapped, bool down, uint16_t mods)
{
    JceEvent event{};
    REQUIRE(jce_editor_fixture_event('K', 0, 0, scan, down ? 1 : 0,
                                     0, 0, 0, 0, &event));
    event.key.mod = mods; // Explicit controlled event snapshot, not OS evidence.
    jce_editor_submit_key_event(event, mapped);
}

TEST_CASE("actual ImGui key queue preserves Preferences chords and negative controls")
{
    JceTestImGuiContext context;
    jce_hotkeys_reset_all();
    context.begin(); context.end();

    // A complete short press reaches the event queue in one application pump.
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, false, JCE_KMOD_LCTRL);
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    int burst_matches = 0;
    for (int frame = 0; frame < 8; ++frame) {
        context.begin();
        if (jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)) {
            ++burst_matches;
            CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
        }
        context.end();
    }
    CHECK(burst_matches == 1);
    CHECK_FALSE(ImGui::GetIO().KeyCtrl);

    // The same owner also accepts a modifier held across separate pumps.
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    context.begin(); CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); context.end();
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, true, JCE_KMOD_LCTRL);
    context.begin(); CHECK(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); context.end();
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, false, JCE_KMOD_LCTRL);
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    for (int frame = 0; frame < 4; ++frame) { context.begin(); context.end(); }

    // Comma without Ctrl must remain ordinary input, not open Preferences.
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, true, JCE_KMOD_NONE);
    context.begin(); CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); context.end();
    submit(JCE_KEY_COMMA, ImGuiKey_Comma, false, JCE_KMOD_NONE);
    context.begin(); context.end();

    // A different Ctrl chord cannot activate the Preferences mapping.
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, true, JCE_KMOD_LCTRL);
    context.begin(); CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); context.end();
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, false, JCE_KMOD_LCTRL);
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    for (int frame = 0; frame < 4; ++frame) { context.begin(); context.end(); }

    // Rebound chords use the same submission owner and actual registry.
    jce_hotkey_set(JCE_HK_EDIT_PREFERENCES, {ImGuiKey_Period, JCE_HKM_CTRL});
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, true, JCE_KMOD_LCTRL);
    context.begin(); CHECK(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); context.end();
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, false, JCE_KMOD_LCTRL);
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    for (int frame = 0; frame < 4; ++frame) { context.begin(); context.end(); }
    jce_hotkeys_reset_all();

    // Diagnostic state is observation-only, disabled-inert, once per phase
    // per frame and globally bounded even under repeated input.
    JceEditorHotkeyDiagState diag{};
    CHECK_FALSE(jce_editor_hotkey_diag_event_take(diag, false, true, 100));
    CHECK(diag.rows == 0); CHECK(diag.pending_until == -1);
    REQUIRE(jce_editor_hotkey_diag_event_take(diag, true, true, 100));
    CHECK(jce_editor_hotkey_diag_sample_take(diag, true, false, 101));
    CHECK_FALSE(jce_editor_hotkey_diag_sample_take(diag, true, false, 101));
    CHECK(jce_editor_hotkey_diag_sample_take(diag, true, true, 101));
    CHECK_FALSE(jce_editor_hotkey_diag_sample_take(diag, true, true, 101));
    CHECK_FALSE(jce_editor_hotkey_diag_sample_take(diag, true, true, 109));
    for (int i = 0; i < 40; ++i)
        jce_editor_hotkey_diag_event_take(diag, true, false, 110 + i);
    CHECK(diag.rows == 32);
    CHECK_FALSE(jce_editor_hotkey_diag_event_take(diag, true, true, 200));
    CHECK_FALSE(jce_editor_hotkey_diag_sample_take(diag, true, false, 200));
}


#ifdef JCE_TEST_HOTKEY_DIAG_RECORDS
#ifndef JCE_DIST
#error "DIST diagnostic controls require ordinary INFO macros to be stripped"
#endif
#include <cstring>
#include <cstdio>
namespace {
struct HotkeyDiagnosticCapture {
    unsigned count = 0;
    char messages[32][384] = {};
    JceLogLevel levels[32] = {};
    static void sink(const JceLogRecord *record, void *user)
    {
        auto &capture = *static_cast<HotkeyDiagnosticCapture *>(user);
        if (std::strcmp(record->tag, "editor-input") != 0) return;
        if (capture.count < 32) {
            std::snprintf(capture.messages[capture.count], 384, "%s", record->message);
            capture.levels[capture.count] = record->level;
        }
        ++capture.count;
    }
    HotkeyDiagnosticCapture()
    {
        // The actual logger's documented pre-init synchronous sink. No log
        // thread/file is started, and callback pointers never outlive this owner.
        jce_log_set_sink(sink, this);
    }
    ~HotkeyDiagnosticCapture() { jce_log_set_sink(nullptr, nullptr); }
};
}
TEST_CASE("DIST opt-in diagnostic survives stripped INFO macros through real logger")
{
    HotkeyDiagnosticCapture capture;
    jce_log_set_level(JCE_LOG_LEVEL_INFO);
    LOG_INFO("editor-input", "ordinary_INFO_must_be_compiled_out");
    CHECK(capture.count == 0); // Actual DIST macro negative contrast.
    REQUIRE(jce_editor_hotkey_diag_enabled());
    auto &state = jce_editor_hotkey_diag_state();
    REQUIRE(state.rows == 0);
    jce_hotkeys_reset_all();
    const auto binding = jce_hotkey_get(JCE_HK_EDIT_PREFERENCES);
    JceEvent event{};
    event.type = JCE_EVENT_KEY_DOWN;
    event.key.scancode = JCE_KEY_COMMA;
    event.key.mod = JCE_KMOD_LCTRL;

    // Direct diagnostics retain the public logger's runtime level/sink policy.
    jce_log_set_level(JCE_LOG_LEVEL_ERROR);
    jce_editor_hotkey_diag_event(event, binding.key, 100, true, true);
    jce_editor_hotkey_diag_gate(101, "ready", ImGuiMod_Ctrl);
    jce_editor_hotkey_diag_query(101, binding, ImGuiMod_Ctrl, true, true, false);
    CHECK(capture.count == 0);
    CHECK(state.rows == 3); // Attempt budget still bounds filtered records.

    jce_log_set_level(JCE_LOG_LEVEL_INFO);
    jce_editor_hotkey_diag_event(event, binding.key, 110, true, true);
    jce_editor_hotkey_diag_gate(111, "ready", ImGuiMod_Ctrl);
    jce_editor_hotkey_diag_query(111, binding, ImGuiMod_Ctrl, true, true, false);
    REQUIRE(capture.count == 3);
    CHECK(capture.levels[0] == JCE_LOG_LEVEL_INFO);
    CHECK(std::strstr(capture.messages[0], "event frame=110 scancode=54 down=1 mods=64 mapped=") != nullptr);
    CHECK(std::strstr(capture.messages[0], "initialized=1 active=1") != nullptr);
    CHECK(std::strstr(capture.messages[1], "gate frame=111 reason=ready imgui_mods=4096") != nullptr);
    CHECK(std::strstr(capture.messages[2], "binding_mods=1 imgui_mods=4096 edge=1 chord=1 consumed=0") != nullptr);
    jce_editor_hotkey_diag_gate(111, "ready", ImGuiMod_Ctrl);
    jce_editor_hotkey_diag_query(111, binding, ImGuiMod_Ctrl, true, true, false);
    CHECK(capture.count == 3);

    event.type = JCE_EVENT_KEY_UP;
    event.key.scancode = JCE_KEY_LCTRL;
    event.key.mod = JCE_KMOD_NONE;
    for (int i = 0; i < 40; ++i)
        jce_editor_hotkey_diag_event(event, ImGuiKey_LeftCtrl, 112 + i, true, true);
    CHECK(state.rows == 32);
    CHECK(capture.count == 29); // Three level-filtered attempts + 29 real records.
    jce_editor_hotkey_diag_gate(113, "modal", ImGuiMod_None);
    jce_editor_hotkey_diag_query(113, binding, ImGuiMod_None, false, false, false);
    CHECK(capture.count == 29);
}
#endif


namespace {
struct HotkeyCaptureSession {
    JceTestImGuiContext context;
    HotkeyCaptureSession()
    {
        // Reset only the compiled registry session; no user JSON is loaded.
        jce_hotkeys_shutdown();
        jce_hotkeys_reset_all();
    }
    ~HotkeyCaptureSession() { jce_hotkeys_shutdown(); }
};

static void submit_ctrl_shift_p(bool down)
{
    constexpr uint16_t both = JCE_KMOD_LCTRL | JCE_KMOD_LSHIFT;
    if (down) {
        submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
        submit(JCE_KEY_LSHIFT, ImGuiKey_LeftShift, true, both);
        submit(JCE_KEY_P, ImGuiKey_P, true, both);
    } else {
        submit(JCE_KEY_P, ImGuiKey_P, false, both);
        submit(JCE_KEY_LSHIFT, ImGuiKey_LeftShift, false, JCE_KMOD_LCTRL);
        submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    }
}
}

TEST_CASE("actual capture blocks dispatch before and after a conflicting commit")
{
    HotkeyCaptureSession session;
    auto &context = session.context;
    context.begin();
    REQUIRE(jce_hotkeys_capture_begin(JCE_HK_EDIT_PREFERENCES));
    context.end();

    submit_ctrl_shift_p(true);
    context.begin();
    // Production layout queries Project Settings before Preferences polling.
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_PANEL_PROJECT_SETTINGS));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    REQUIRE(jce_hotkeys_capture_poll());
    CHECK(jce_hotkeys_capture_id() == JCE_HK_COUNT);
    const auto rebound = jce_hotkey_get(JCE_HK_EDIT_PREFERENCES);
    CHECK(rebound.key == ImGuiKey_P);
    CHECK(rebound.mods == (JCE_HKM_CTRL | JCE_HKM_SHIFT));
    CHECK(jce_hotkey_find_conflict(JCE_HK_EDIT_PREFERENCES, rebound)
          == JCE_HK_PANEL_PROJECT_SETTINGS);
    // Warning-only conflicts retain the old action; capture itself cannot fire it.
    CHECK(jce_hotkey_chord_equal(
        jce_hotkey_get(JCE_HK_PANEL_PROJECT_SETTINGS), rebound));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_PANEL_PROJECT_SETTINGS));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    CHECK_FALSE(jce_hotkeys_capture_poll()); // Commit happens exactly once.
    context.end();

    context.begin();
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_PANEL_PROJECT_SETTINGS)); // Held P.
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    context.end();
    submit_ctrl_shift_p(false);
    for (int frame = 0; frame < 4; ++frame) { context.begin(); context.end(); }
    submit_ctrl_shift_p(true);
    context.begin();
    // Capture has ended. Existing first-query conflict policy remains intact.
    CHECK(jce_hotkey_pressed(JCE_HK_PANEL_PROJECT_SETTINGS));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    context.end();
}

TEST_CASE("actual capture completion also blocks consumers queried only after polling")
{
    HotkeyCaptureSession session;
    auto &context = session.context;
    context.begin();
    REQUIRE(jce_hotkeys_capture_begin(JCE_HK_EDIT_PREFERENCES));
    context.end();
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, true, JCE_KMOD_LCTRL);
    context.begin();
    // Poll first, with no action query to pre-consume the chord.
    REQUIRE(jce_hotkeys_capture_poll());
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    CHECK(jce_hotkey_get(JCE_HK_EDIT_PREFERENCES).key == ImGuiKey_Period);
    context.end();
    context.begin();
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); // Held key is no edge.
    context.end();
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, false, JCE_KMOD_LCTRL);
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, false, JCE_KMOD_NONE);
    for (int frame = 0; frame < 4; ++frame) { context.begin(); context.end(); }
    submit(JCE_KEY_LCTRL, ImGuiKey_LeftCtrl, true, JCE_KMOD_LCTRL);
    submit(JCE_KEY_PERIOD, ImGuiKey_Period, true, JCE_KMOD_LCTRL);
    context.begin();
    CHECK(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES)); // Fresh edge restores dispatch.
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_EDIT_PREFERENCES));
    context.end();
}

TEST_CASE("actual capture ignores modifier edges and Escape cancels without rebinding")
{
    HotkeyCaptureSession session;
    auto &context = session.context;
    const auto before = jce_hotkey_get(JCE_HK_EDIT_PREFERENCES);
    jce_hotkey_set(JCE_HK_UI_SCREENSHOT, {ImGuiKey_Escape, JCE_HKM_NONE});
    context.begin();
    REQUIRE(jce_hotkeys_capture_begin(JCE_HK_EDIT_PREFERENCES));
    CHECK_FALSE(jce_hotkeys_capture_begin(JCE_HK_COUNT));
    CHECK(jce_hotkeys_capture_id() == JCE_HK_EDIT_PREFERENCES);
    context.end();
    submit(JCE_KEY_LSHIFT, ImGuiKey_LeftShift, true, JCE_KMOD_LSHIFT);
    context.begin();
    CHECK_FALSE(jce_hotkeys_capture_poll());
    CHECK(jce_hotkeys_capture_id() == JCE_HK_EDIT_PREFERENCES);
    context.end();
    submit(JCE_KEY_LSHIFT, ImGuiKey_LeftShift, false, JCE_KMOD_NONE);
    submit(JCE_KEY_ESCAPE, ImGuiKey_Escape, true, JCE_KMOD_NONE);
    context.begin();
    CHECK_FALSE(jce_hotkeys_capture_poll());
    CHECK(jce_hotkeys_capture_id() == JCE_HK_COUNT);
    CHECK(jce_hotkey_chord_equal(before, jce_hotkey_get(JCE_HK_EDIT_PREFERENCES)));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_UI_SCREENSHOT)); // Cancel edge is protected.
    context.end();
    submit(JCE_KEY_ESCAPE, ImGuiKey_Escape, false, JCE_KMOD_NONE);
    context.begin(); context.end();
    submit(JCE_KEY_ESCAPE, ImGuiKey_Escape, true, JCE_KMOD_NONE);
    context.begin();
    CHECK(jce_hotkey_pressed(JCE_HK_UI_SCREENSHOT));
    context.end();
}

TEST_CASE("actual capture lifecycle cancel protects early globals and then releases input")
{
    HotkeyCaptureSession session;
    auto &context = session.context;
    const auto before = jce_hotkey_get(JCE_HK_EDIT_PREFERENCES);
    context.begin();
    REQUIRE(jce_hotkeys_capture_begin(JCE_HK_EDIT_PREFERENCES));
    context.end();
    submit(JCE_KEY_F11, ImGuiKey_F11, true, JCE_KMOD_NONE);
    context.begin();
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW));
    // This same production cancel port is used by tab-away/close/focus-loss.
    // Native modal/pointer consumption still requires Root's actual GUI check.
    jce_hotkeys_capture_cancel();
    CHECK(jce_hotkeys_capture_id() == JCE_HK_COUNT);
    CHECK(jce_hotkey_chord_equal(before, jce_hotkey_get(JCE_HK_EDIT_PREFERENCES)));
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW));
    context.end();
    context.begin();
    CHECK_FALSE(jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW));
    context.end();
    submit(JCE_KEY_F11, ImGuiKey_F11, false, JCE_KMOD_NONE);
    context.begin(); context.end();
    submit(JCE_KEY_F11, ImGuiKey_F11, true, JCE_KMOD_NONE);
    context.begin();
    CHECK(jce_hotkey_pressed(JCE_HK_UI_TOGGLE_FULLSCREEN_VIEW));
    context.end();
}

TEST_CASE("capture shutdown clears target and completion barrier for a new session")
{
    HotkeyCaptureSession session;
    auto &context = session.context;
    context.begin();
    REQUIRE(jce_hotkeys_capture_begin(JCE_HK_EDIT_PREFERENCES));
    jce_hotkeys_shutdown();
    jce_hotkeys_reset_all();
    CHECK(jce_hotkeys_capture_id() == JCE_HK_COUNT);
    context.end();
    submit(JCE_KEY_F12, ImGuiKey_F12, true, JCE_KMOD_NONE);
    context.begin();
    CHECK(jce_hotkey_pressed(JCE_HK_UI_SCREENSHOT));
    context.end();
}
