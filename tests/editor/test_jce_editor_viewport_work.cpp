/* Real ImGui CPU clip/focus state + exact viewport work admission. No GPU. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "jce_test_imgui_context.h"
#include "jce_test_owned_directory.h"
#ifndef JCE_TEST_EDITOR_VIEWPORT_WORK_HEADER
#define JCE_TEST_EDITOR_VIEWPORT_WORK_HEADER "core/jce_editor_viewport_work.h"
#endif
#include JCE_TEST_EDITOR_VIEWPORT_WORK_HEADER
#include <limits>
#include <jce/tools/jce_imgui_internal.h>
/* Only public native-window flags are counted ports. Queue/NewFrame/clip
 * behavior and the generated production pre-frame callsite are actual ImGui. */
struct JceWindow {
    uint32_t flags = 0;
    bool available = true;
    mutable unsigned queries = 0;
};
extern "C" bool jce_window_get_flags(const JceWindow *window, uint32_t *out)
{
    if (!window || !out) return false;
    ++window->queries;
    if (!window->available) return false;
    *out = window->flags;
    return true;
}
#ifndef JCE_TEST_EDITOR_VIEWPORT_FRAME_BODY
#error The registered fixture must consume the actual editor pre-NewFrame body
#endif
namespace {
void production_begin_frame(JceWindow *window)
{
#include JCE_TEST_EDITOR_VIEWPORT_FRAME_BODY
}
struct FocusFixture {
    JceTestImGuiContext gui;
    JceWindow window;
    bool opened = false;
    void begin()
    {
        production_begin_frame(&window);
        gui.frame_open = true;
        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400,300), ImGuiCond_Always);
        ImGui::Begin("Native focus admission", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        opened = true;
    }
    void end() { ImGui::End(); gui.end(); opened = false; }
    ~FocusFixture() { if (opened) end(); }
};
struct Fixture {
    JceTestImGuiContext gui;
    Fixture() {
        gui.begin();
        ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400,300), ImGuiCond_Always);
        ImGui::Begin("Viewport work admission", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    }
    ~Fixture() { ImGui::End(); gui.end(); }
};
struct FrameLogEnvironment {
    JceTestEnvironment frame{"JCE_KPI_FRAME_LOG", nullptr};
    JceTestEnvironment draw{"JCE_KPI_DRAW_LOG", nullptr};
    JceTestEnvironment policy{"JCE_KPI_FRAME_LOG_VIEWPORT", nullptr};
    JceTestEnvironment draw_policy{"JCE_KPI_DRAW_LOG_VIEWPORT", nullptr};
    JceTestEnvironment replay{"JCE_INPUT_REPLAY", nullptr};
    JceTestEnvironment shot_frame{"JCE_SHOT_FRAME", nullptr};
    JceTestEnvironment shot_path{"JCE_SHOT_PATH", nullptr};
};
}
TEST_CASE("visible and partially clipped real viewport rectangles render") {
    Fixture f;
    CHECK(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), false));
    CHECK(jce_editor_viewport_wants_render(ImVec2(350,24), ImVec2(200,100), false));
}
TEST_CASE("fully clipped real viewport rectangles do not submit offscreen work") {
    Fixture f;
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), false));
}
TEST_CASE("actual ImGui focus-loss observation suppresses authoring viewport work") {
    Fixture f; ImGui::GetIO().AppFocusLost = true;
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), false));
}
TEST_CASE("explicit capture retains positive viewport work despite focus or clipping") {
    Fixture f; ImGui::GetIO().AppFocusLost = true;
    CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), true));
}
TEST_CASE("empty negative and nonfinite layout cannot become a fake sixteen pixel capture") {
    Fixture f;
    const float bad = std::numeric_limits<float>::quiet_NaN();
    for (const bool capture : {false, true}) {
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100), capture));
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(100,-1), capture));
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(bad,24), ImVec2(100,100), capture));
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(100,bad), capture));
    }
}

TEST_CASE("actual pre-NewFrame native no-focus observation suppresses initial and repeated frames") {
    FocusFixture f;
    CHECK_FALSE(ImGui::GetIO().AppFocusLost); // Fresh context has no native event.
    for (int frame = 0; frame < 2; ++frame) {
        f.begin();
        CHECK(ImGui::GetIO().AppFocusLost);
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), false));
        CHECK(f.window.queries == static_cast<unsigned>(frame + 1));
        f.end();
    }
}
TEST_CASE("actual native gained lost hidden and minimized flags reach the ImGui queue") {
    FocusFixture f;
    for (uint32_t flags : {JCE_WINDOW_INPUT_FOCUS, 0u,
            JCE_WINDOW_INPUT_FOCUS | JCE_WINDOW_HIDDEN,
            JCE_WINDOW_INPUT_FOCUS | JCE_WINDOW_MINIMIZED,
            JCE_WINDOW_INPUT_FOCUS}) {
        f.window.flags = flags;
        f.begin();
        const bool expected = flags == JCE_WINDOW_INPUT_FOCUS;
        CHECK(ImGui::GetIO().AppFocusLost == !expected);
        CHECK(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), false) == expected);
        f.end();
    }
    CHECK(f.window.queries == 5);
}
TEST_CASE("unavailable native flags preserve queued and current ImGui observations") {
    FocusFixture f;
    f.window.available = false;
    ImGui::GetIO().AddFocusEvent(false);
    const int queued = f.gui.context->InputEventsQueue.Size;
    const auto next_id = f.gui.context->InputEventsNextEventId;
    jce_editor_viewport_sync_window_focus(&f.window);
    jce_editor_viewport_sync_window_focus(nullptr);
    CHECK(f.gui.context->InputEventsQueue.Size == queued);
    CHECK(f.gui.context->InputEventsNextEventId == next_id);
    f.begin();
    CHECK(ImGui::GetIO().AppFocusLost);
    jce_editor_viewport_sync_window_focus(&f.window);
    CHECK(ImGui::GetIO().AppFocusLost);
    CHECK(f.gui.context->InputEventsQueue.Size == 0);
    f.end();
    f.window.flags = JCE_WINDOW_INPUT_FOCUS;
    f.window.available = true;
    f.begin();
    CHECK_FALSE(ImGui::GetIO().AppFocusLost);
    f.window.available = false;
    jce_editor_viewport_sync_window_focus(&f.window);
    CHECK_FALSE(ImGui::GetIO().AppFocusLost);
    CHECK(f.gui.context->InputEventsQueue.Size == 0);
}
TEST_CASE("same-frame native synchronization relies on actual ImGui focus-event deduplication") {
    FocusFixture f;
    jce_editor_viewport_sync_window_focus(&f.window);
    REQUIRE(f.gui.context->InputEventsQueue.Size == 1);
    CHECK(f.gui.context->InputEventsQueue[0].Type == ImGuiInputEventType_Focus);
    CHECK_FALSE(f.gui.context->InputEventsQueue[0].AppFocused.Focused);
    const auto next_id = f.gui.context->InputEventsNextEventId;
    jce_editor_viewport_sync_window_focus(&f.window);
    CHECK(f.gui.context->InputEventsQueue.Size == 1);
    CHECK(f.gui.context->InputEventsNextEventId == next_id);
    f.begin();
    CHECK(ImGui::GetIO().AppFocusLost);
    CHECK(f.gui.context->InputEventsQueue.Size == 0);
    jce_editor_viewport_sync_window_focus(&f.window);
    CHECK(f.gui.context->InputEventsQueue.Size == 0);
    CHECK(f.gui.context->InputEventsNextEventId == next_id);
}
TEST_CASE("current native focus supersedes an earlier queued stale loss without another input owner") {
    FocusFixture f;
    ImGui::GetIO().AddFocusEvent(false);
    f.window.flags = JCE_WINDOW_INPUT_FOCUS;
    f.begin();
    CHECK_FALSE(ImGui::GetIO().AppFocusLost);
    CHECK(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), false));
}
TEST_CASE("explicit capture survives synchronized native no-focus but never invalid layout") {
    FocusFixture f;
    f.begin();
    CHECK(ImGui::GetIO().AppFocusLost);
    CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), true));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100), true));
}

TEST_CASE("FrameLog viewport policy recognizes only the exact natural selector") {
    using Policy = JceEditorFrameLogViewportPolicy;
    CHECK(jce_editor_frame_log_viewport_policy(nullptr) == Policy::Capture);
    CHECK(jce_editor_frame_log_viewport_policy("") == Policy::Capture);
    CHECK(jce_editor_frame_log_viewport_policy("capture") == Policy::Capture);
    CHECK(jce_editor_frame_log_viewport_policy("natural") == Policy::Natural);
    for (const char *value : {"Natural", "natural ", "0", "1", "normal", "invalid"}) {
        CAPTURE(value);
        CHECK(jce_editor_frame_log_viewport_policy(value) == Policy::Invalid);
    }
}

TEST_CASE("unset or empty FrameLog cannot manufacture a capture request") {
    FrameLogEnvironment env;
    for (const char *mode : {"", "capture", "natural", "invalid"}) {
        env.policy.set(mode);
        CHECK_FALSE(jce_editor_viewport_capture_requested(false));
        env.frame.set("");
        CHECK_FALSE(jce_editor_viewport_capture_requested(false));
        env.frame.set(nullptr);
    }
}

TEST_CASE("default and invalid FrameLog selectors retain existing forced viewport work") {
    FrameLogEnvironment env;
    env.frame.set("frame-phases.csv");
    Fixture f;
    ImGui::GetIO().AppFocusLost = true;
    CHECK(jce_editor_viewport_capture_requested(false));
    for (const char *mode : {"", "capture", "invalid"}) {
        CAPTURE(mode);
        env.policy.set(mode);
        const bool capture = jce_editor_viewport_capture_requested(false);
        CHECK(capture);
        CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), capture));
    }
    CHECK(jce_editor_frame_log_viewport_policy("invalid") != JceEditorFrameLogViewportPolicy::Natural);
}

TEST_CASE("natural FrameLog alone consumes the real focus and clip gates") {
    FrameLogEnvironment env;
    env.frame.set("frame-phases.csv");
    env.policy.set("natural");
    Fixture f;
    const bool capture = jce_editor_viewport_capture_requested(false);
    CHECK_FALSE(capture);
    CHECK(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), capture));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), capture));
    ImGui::GetIO().AppFocusLost = true;
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), capture));
}

TEST_CASE("active Game capture and DrawLog preserve their precedence over natural FrameLog") {
    FrameLogEnvironment env;
    env.frame.set("frame-phases.csv");
    env.policy.set("natural");
    Fixture f;
    ImGui::GetIO().AppFocusLost = true;
    CHECK(jce_editor_viewport_capture_requested(true));
    CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100),
                                         jce_editor_viewport_capture_requested(true)));
    env.draw.set("draw.csv");
    CHECK(jce_editor_viewport_capture_requested(false));
    CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100),
                                         jce_editor_viewport_capture_requested(false)));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100),
                                               jce_editor_viewport_capture_requested(false)));
    env.draw.set("");
    CHECK_FALSE(jce_editor_viewport_capture_requested(false));
}

TEST_CASE("natural selection does not add Replay or scheduled-shot capture overrides") {
    FrameLogEnvironment env;
    env.policy.set("natural");
    env.replay.set("replay.jirc");
    env.shot_frame.set("30");
    env.shot_path.set("backbuffer.png");
    CHECK_FALSE(jce_editor_viewport_capture_requested(false));
    env.frame.set("frame-phases.csv");
    CHECK_FALSE(jce_editor_viewport_capture_requested(false));
    env.policy.set("capture");
    CHECK(jce_editor_viewport_capture_requested(false));
}


TEST_CASE("unset or empty DrawLog cannot manufacture a capture request") {
    FrameLogEnvironment env;
    for (const char *mode : {"", "capture", "natural", "Natural", "natural ", "invalid"}) {
        CAPTURE(mode);
        env.draw_policy.set(mode);
        CHECK_FALSE(jce_editor_viewport_capture_requested(false));
        env.draw.set("");
        CHECK_FALSE(jce_editor_viewport_capture_requested(false));
        env.draw.set(nullptr);
    }
}

TEST_CASE("default and invalid DrawLog selectors preserve legacy capture admission") {
    FrameLogEnvironment env;
    env.draw.set("draw.csv");
    Fixture f;
    ImGui::GetIO().AppFocusLost = true;
    CHECK(jce_editor_viewport_capture_requested(false));
    for (const char *mode : {"", "capture", "Natural", "natural ", "0", "1", "invalid"}) {
        CAPTURE(mode);
        env.draw_policy.set(mode);
        const bool capture = jce_editor_viewport_capture_requested(false);
        CHECK(capture);
        CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), capture));
        CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100), capture));
    }
}

TEST_CASE("natural DrawLog consumes unchanged real focus clip and layout gates") {
    FrameLogEnvironment env;
    env.draw.set("draw.csv");
    env.draw_policy.set("natural");
    Fixture f;
    const bool capture = jce_editor_viewport_capture_requested(false);
    CHECK_FALSE(capture);
    CHECK(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), capture));
    CHECK(jce_editor_viewport_wants_render(ImVec2(350,24), ImVec2(200,100), capture));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), capture));
    const float bad = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100), capture));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(100,-1), capture));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(bad,24), ImVec2(100,100), capture));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(100,bad), capture));
    ImGui::GetIO().AppFocusLost = true;
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(200,100), capture));
}

TEST_CASE("FrameLog and DrawLog selectors independently remove only their own override") {
    FrameLogEnvironment env;
    env.frame.set("frames.csv");
    env.draw.set("draw.csv");
    Fixture f;
    ImGui::GetIO().AppFocusLost = true;
    for (const char *frame_mode : {"capture", "natural", "invalid"}) {
        for (const char *draw_mode : {"capture", "natural", "invalid"}) {
            CAPTURE(frame_mode);
            CAPTURE(draw_mode);
            env.policy.set(frame_mode);
            env.draw_policy.set(draw_mode);
            const bool expected = std::strcmp(frame_mode,"natural") != 0 ||
                                  std::strcmp(draw_mode,"natural") != 0;
            const bool capture = jce_editor_viewport_capture_requested(false);
            CHECK(capture == expected);
            CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100), capture) == expected);
        }
    }
}

TEST_CASE("Game capture wins over both natural loggers without adding replay or shot overrides") {
    FrameLogEnvironment env;
    env.frame.set("frames.csv");
    env.draw.set("draw.csv");
    env.policy.set("natural");
    env.draw_policy.set("natural");
    env.replay.set("replay.jirc");
    env.shot_frame.set("30");
    env.shot_path.set("backbuffer.png");
    Fixture f;
    ImGui::GetIO().AppFocusLost = true;
    CHECK_FALSE(jce_editor_viewport_capture_requested(false));
    CHECK(jce_editor_viewport_capture_requested(true));
    CHECK(jce_editor_viewport_wants_render(ImVec2(900,900), ImVec2(200,100),
                                         jce_editor_viewport_capture_requested(true)));
    CHECK_FALSE(jce_editor_viewport_wants_render(ImVec2(16,24), ImVec2(0,100),
                                               jce_editor_viewport_capture_requested(true)));
}
