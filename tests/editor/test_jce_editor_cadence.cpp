/* Actual authoring cadence owner + real canonical config fields. Counted
 * public FPS/renderer ports: no native window, waits, present or GPU evidence. */
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#ifndef JCE_TEST_EDITOR_CADENCE_HEADER
#define JCE_TEST_EDITOR_CADENCE_HEADER "core/jce_editor_cadence.h"
#endif
#include JCE_TEST_EDITOR_CADENCE_HEADER
#include "core/jce_editor_play_presentation.h"
#include "core/jce_editor_state.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_runtime_host_policy.inc.h>
#ifndef JCE_TEST_EDITOR_CADENCE_HOST_CALL
#error The cadence fixture must consume the actual Editor main Play callsite
#endif
#ifndef JCE_TEST_EDITOR_PLAY_HOST_BODY
#error The cadence fixture must consume the actual Editor Play receiver
#endif
#ifndef JCE_TEST_EDITOR_PLAY_POLICY_BEGIN
#error The cadence fixture must consume the actual Editor Play policy begin
#endif
#ifndef JCE_TEST_EDITOR_PLAY_PAUSE_BODY
#error The cadence fixture must consume the actual Editor Pause receiver
#endif

struct JceRenderer { bool vsync = false; };
namespace {
int fps = 0, maximum_written_fps = 0;
bool run_in_background = false;
unsigned fps_writes = 0, sync_writes = 0, audio_pauses = 0, audio_resumes = 0;
struct { JcePlayState play_state = JCE_PLAY_STOPPED; } s;
struct {
    JceRuntimeHostPolicy policy = {};
    JceEditorPlayPresentationState presentation;
    struct { JceRuntime *runtime = nullptr; } runtime_stats;
    struct { const JceServices *services = nullptr; } module;
    int requested_paused_fps = 30, requested_background_fps = 15;
    float render_dt = 0;
} s_play_host;
struct Fixture {
    JceEditorCadence state;
    JceEditorCadenceRates rates;
    Fixture() { fps = maximum_written_fps = 0; run_in_background = false; fps_writes = sync_writes = 0; s_play_host = {}; s.play_state = JCE_PLAY_STOPPED; }
    void begin(uint64_t now = 0, uint64_t frequency = 1000) {
        jce_editor_cadence_begin(state, rates, now, frequency);
        tick(now, frequency);
    }
    void tick(uint64_t now, uint64_t frequency = 1000,
              uint32_t flags = JCE_WINDOW_INPUT_FOCUS,
              bool valid = true, bool held = false, bool play = false) {
        jce_editor_cadence_tick(state, rates, now, frequency, flags, valid, held, play);
    }
};
}
extern "C" {
int JCE_CALL jce_engine_get_target_fps(void) { return fps; }
void JCE_CALL jce_engine_set_target_fps(int value) {
    fps = value; ++fps_writes;
    if (value > maximum_written_fps) maximum_written_fps = value;
}
bool JCE_CALL jce_engine_get_run_in_background(void) { return run_in_background; }
void JCE_CALL jce_engine_set_run_in_background(bool enabled) { run_in_background = enabled; }
bool jce_renderer_is_fallback(const JceRenderer *) { return false; }
bool jce_renderer_get_vsync(const JceRenderer *r) { return r->vsync; }
void jce_renderer_set_vsync(JceRenderer *r, bool enabled) { r->vsync = enabled; ++sync_writes; }
void JCE_CALL jce_runtime_pause_audio(JceRuntime *) { ++audio_pauses; }
void JCE_CALL jce_runtime_resume_audio(JceRuntime *) { ++audio_resumes; }
}
#include JCE_TEST_EDITOR_PLAY_HOST_BODY
#define LOG_INFO(...) ((void)0)
#include JCE_TEST_EDITOR_PLAY_PAUSE_BODY
#undef LOG_INFO
static bool actual_play_policy_begin(const JceEditorCadenceRates &cadence_rates)
{
    struct { struct { bool run_in_background = false; } player; } settings;
    const auto *ps = &settings;
#include JCE_TEST_EDITOR_PLAY_POLICY_BEGIN
}
static void actual_main_play_wire(const JceEditorCadenceRates &rates)
{
    struct { JceEditorCadenceRates cadence_rates; } state{rates};
#include JCE_TEST_EDITOR_CADENCE_HOST_CALL
}
TEST_CASE("authoring defaults are bounded without changing generic engine default") {
    Fixture f;
    CHECK(fps == 0);
    f.begin(); CHECK(fps == 60);
    f.tick(1999); CHECK(fps == 60);
    f.tick(2000); CHECK(fps == 30);
    f.tick(100000); CHECK(fps == 30);
    CHECK(sync_writes == 0);
}
TEST_CASE("keyboard mouse and touch activity restart the same two second clock") {
    Fixture f; f.begin(); f.tick(2000); CHECK(fps == 30);
    jce_editor_cadence_activity(f.state, 2500); f.tick(2500); CHECK(fps == 60);
    f.tick(4499); CHECK(fps == 60); f.tick(4500); CHECK(fps == 30);
    JceInputFrame input = {};
    CHECK_FALSE(jce_editor_cadence_input_held(input));
    input.keys_bits[63] = 1; CHECK(jce_editor_cadence_input_held(input));
    f.tick(9000, 1000, JCE_WINDOW_INPUT_FOCUS, true, jce_editor_cadence_input_held(input));
    CHECK(fps == 60);
    input = {}; input.mouse_buttons = 1; CHECK(jce_editor_cadence_input_held(input));
    input = {}; input.touch_count = 1; CHECK(jce_editor_cadence_input_held(input));
    CHECK(jce_editor_cadence_input_event(JCE_EVENT_MOUSE_WHEEL));
    CHECK(jce_editor_cadence_input_event(JCE_EVENT_TEXT_INPUT));
    CHECK_FALSE(jce_editor_cadence_input_event(JCE_EVENT_OTHER));
}
TEST_CASE("lost focus hidden minimized and unavailable flag observations use background rate") {
    Fixture f; f.begin();
    for (const auto flags : {0u, JCE_WINDOW_INPUT_FOCUS | JCE_WINDOW_HIDDEN,
                            JCE_WINDOW_INPUT_FOCUS | JCE_WINDOW_MINIMIZED}) {
        f.tick(3000, 1000, flags); CHECK(fps == 5);
        f.tick(4000); CHECK(fps == 60);
    }
    f.tick(5000, 1000, JCE_WINDOW_INPUT_FOCUS, false); CHECK(fps == 5);
    f.tick(6000); CHECK(fps == 60);
}
TEST_CASE("canonical persisted UI fields preserve explicit uncapped and high refresh rates") {
    Fixture f; JceEditorConfig cfg;
    jce_editor_config_defaults(&cfg);
    auto defaults = jce_editor_cadence_rates(cfg);
    CHECK(defaults.active == 60); CHECK(defaults.idle == 30); CHECK(defaults.background == 5);
    CHECK(defaults.play_background == 15);
    /* Previously saved explicit 15 stays authoritative; only absent keys use 5. */
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, 15));
    f.rates = jce_editor_cadence_rates(cfg); f.begin();
    f.tick(3000, 1000, 0); CHECK(fps == 15); CHECK(f.rates.play_background == 15);
    CHECK(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, -1) == 15);
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_ACTIVE, 144));
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_IDLE, 72));
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, 0));
    f.rates = jce_editor_cadence_rates(cfg); CHECK(f.rates.play_background == 0);
    f.tick(0); CHECK(fps == 144);
    f.tick(2000); CHECK(fps == 72); f.tick(3000, 1000, 0); CHECK(fps == 72);
    for (const char *key : {JCE_EDITOR_CADENCE_ACTIVE, JCE_EDITOR_CADENCE_IDLE,
                           JCE_EDITOR_CADENCE_BACKGROUND})
        REQUIRE(jce_editor_config_set_ui_int(&cfg, key, 0));
    f.rates = jce_editor_cadence_rates(cfg); f.tick(4000); CHECK(fps == 0);
    f.tick(6000); CHECK(fps == 0); f.tick(7000, 1000, 0); CHECK(fps == 0);
}
TEST_CASE("malformed legacy rates fall back without rewriting user configuration") {
    JceEditorConfig cfg; jce_editor_config_defaults(&cfg);
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_ACTIVE, -1));
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_IDLE, 1001));
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, 1000));
    const auto rates = jce_editor_cadence_rates(cfg);
    CHECK(rates.active == 60); CHECK(rates.idle == 30); CHECK(rates.background == 1000);
    CHECK(rates.play_background == 1000);
    CHECK(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_ACTIVE, 99) == -1);
    REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, -1));
    const auto invalid_background = jce_editor_cadence_rates(cfg);
    CHECK(invalid_background.background == 5); CHECK(invalid_background.play_background == 15);
    CHECK(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, 99) == -1);
}
TEST_CASE("stable policy does not rearm the engine deadline every frame and release is idempotent") {
    Fixture f; f.begin(); const auto writes = fps_writes;
    f.tick(100); f.tick(500); CHECK(fps_writes == writes);
    jce_editor_cadence_begin(f.state, f.rates, 1000, 1000); CHECK(fps_writes == writes);
    jce_editor_cadence_end(f.state); CHECK(fps == 0);
    const auto closed_writes = fps_writes;
    jce_editor_cadence_end(f.state); CHECK(fps_writes == closed_writes);
}
TEST_CASE("counter zero frequency change reverse clock and overflow do not manufacture idle") {
    Fixture f; f.begin(); f.tick(2000); CHECK(fps == 30);
    f.tick(100); CHECK(fps == 60);
    f.tick(5000, 2000); CHECK(fps == 60); f.tick(9000, 2000); CHECK(fps == 30);
    f.tick(10000, 0); CHECK(fps == 60); f.tick(UINT64_MAX, 0); CHECK(fps == 60);
    f.tick(UINT64_MAX, UINT64_MAX); CHECK(fps == 60);
}
TEST_CASE("live preference rates apply through existing target setter without renderer reset") {
    Fixture f; f.begin(); f.rates.active = 240; f.tick(100); CHECK(fps == 240);
    f.rates.idle = 90; f.tick(2000); CHECK(fps == 90);
    f.rates.background = 20; f.tick(2500, 1000, 0); CHECK(fps == 20);
    CHECK(sync_writes == 0);
}
TEST_CASE("authoring idle and background limits cannot raise an existing lower cap") {
    Fixture f;
    f.rates.active = 15;
    f.rates.idle = 30;
    f.rates.background = 90;
    f.begin(); CHECK(fps == 15);
    const auto writes = fps_writes;
    f.tick(2000); CHECK(fps == 15);
    f.tick(3000, 1000, 0); CHECK(fps == 15);
    CHECK(fps_writes == writes);
    f.tick(4000); CHECK(fps == 15);
    f.rates.active = 60;
    f.rates.idle = 10;
    f.tick(6000); CHECK(fps == 10);
    f.tick(7000, 1000, 0); CHECK(fps == 10);
    CHECK(sync_writes == 0);
}
TEST_CASE("actual Play adapter retains low project and preview caps while paused or background") {
    struct Rate { int project, preview, idle = 30, background = 90; };
    for (const auto &rate : {Rate{10,60}, Rate{144,10}, Rate{10,0}, Rate{10,60,0,0}}) {
        INFO("project " << rate.project << ", preview " << rate.preview);
        Fixture f; f.rates.play = rate.preview;
        f.rates.idle = rate.idle; f.rates.play_background = rate.background;
        f.begin(); JceRenderer renderer;
        JceRenderSettings authored = {}; authored.target_framerate = rate.project;
        jce_editor_play_presentation_begin(s_play_host.presentation, &renderer,
                                         authored, f.rates.play);
        jce_lifecycle_shutdown(); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        s.play_state = JCE_PLAY_PLAYING;
        REQUIRE(jce_runtime_host_policy_begin(&s_play_host.policy, nullptr,
                                              false, true, 30, 90));
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        maximum_written_fps = 0;
        s.play_state = JCE_PLAY_PAUSED;
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
        CHECK(fps == 10);
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        s.play_state = JCE_PLAY_PLAYING;
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
        CHECK(fps == 10);
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
        actual_main_play_wire(f.rates); CHECK(fps == 10);
        CHECK(s_play_host.policy.running_fps == 10);
        CHECK(maximum_written_fps <= 10);
        const auto stable_writes = fps_writes;
        actual_main_play_wire(f.rates); actual_main_play_wire(f.rates);
        CHECK(fps_writes == stable_writes);
        CHECK(sync_writes == 0);
        jce_runtime_host_policy_end(&s_play_host.policy);
        s.play_state = JCE_PLAY_STOPPED;
        jce_editor_play_presentation_restore(s_play_host.presentation);
        jce_editor_cadence_end(f.state);
        jce_lifecycle_shutdown();
    }
}
TEST_CASE("actual initial Play policy limits already lost focus and system pause") {
    struct Rate { int project, preview, idle, background, expected; };
    const Rate rates[] = {{10,60,30,90,10}, {0,10,30,90,10},
                          {144,0,0,0,144}, {0,0,0,0,0},
                          {0,0,30,15,15}, {144,0,30,90,90}};
    for (const auto &rate : rates) {
        for (const bool system_pause : {false,true}) {
            INFO("project " << rate.project << ", preview " << rate.preview
                 << ", system pause " << system_pause);
            Fixture f; f.begin(); JceRenderer renderer;
            f.rates.play = rate.preview; f.rates.idle = rate.idle;
            f.rates.play_background = rate.background;
            JceRenderSettings authored = {}; authored.target_framerate = rate.project;
            jce_editor_play_presentation_begin(s_play_host.presentation, &renderer,
                                             authored, f.rates.play);
            JceServices services = {};
            services.window = reinterpret_cast<JceWindow *>(uintptr_t(1));
            s_play_host.module.services = &services;
            jce_lifecycle_shutdown(); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
            jce_lifecycle_emit(system_pause ? JCE_LIFECYCLE_PAUSE : JCE_LIFECYCLE_FOCUS_LOST);
            s.play_state = JCE_PLAY_PLAYING;
            maximum_written_fps = 0;
            REQUIRE(actual_play_policy_begin(f.rates));
            CHECK(fps == rate.expected);
            CHECK(maximum_written_fps <= rate.expected);
            CHECK(s_play_host.policy.running_fps == s_play_host.presentation.running_target_fps);
            jce_runtime_host_policy_end(&s_play_host.policy);
            s.play_state = JCE_PLAY_STOPPED;
            jce_editor_play_presentation_restore(s_play_host.presentation);
            jce_editor_cadence_end(f.state);
            jce_lifecycle_shutdown();
        }
    }
}
TEST_CASE("actual Pause adapter retains its cap during focus loss and recalculates raw rates") {
    Fixture f; f.begin(); JceRenderer renderer;
    f.rates.play = 0; f.rates.idle = 30; f.rates.play_background = 90;
    JceRenderSettings authored = {}; authored.target_framerate = 144;
    jce_editor_play_presentation_begin(s_play_host.presentation, &renderer, authored, 0);
    jce_lifecycle_shutdown(); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
    s.play_state = JCE_PLAY_PLAYING;
    REQUIRE(jce_runtime_host_policy_begin(&s_play_host.policy, nullptr, false, true, 30, 90));
    actual_main_play_wire(f.rates); CHECK(fps == 144);
    jce_state_pause(); CHECK(fps == 30); CHECK(s_play_host.render_dt == 0);
    maximum_written_fps = 0;
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); CHECK(fps == 30);
    CHECK(maximum_written_fps <= 30);
    f.rates.play = 10;
    actual_main_play_wire(f.rates); CHECK(fps == 10);
    CHECK(s_play_host.policy.running_fps == 10);
    f.rates.play = 0;
    actual_main_play_wire(f.rates); CHECK(fps == 30);
    CHECK(s_play_host.policy.running_fps == 144);
    CHECK(s_play_host.requested_paused_fps == 30);
    CHECK(s_play_host.requested_background_fps == 90);
    jce_state_pause(); CHECK(fps == 90);
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED); CHECK(fps == 144);
    const auto writes = fps_writes;
    actual_main_play_wire(f.rates); actual_main_play_wire(f.rates);
    CHECK(fps_writes == writes);
    jce_runtime_host_policy_end(&s_play_host.policy);
    s.play_state = JCE_PLAY_STOPPED;
    jce_editor_play_presentation_restore(s_play_host.presentation); CHECK(fps == 60);
    jce_editor_cadence_end(f.state); CHECK(fps == 0);
    jce_lifecycle_shutdown();
}
TEST_CASE("zero temporary authoring limits do not remove another positive bound") {
    Fixture f; f.rates.active = 10; f.rates.idle = f.rates.background = 0;
    f.begin(); CHECK(fps == 10);
    f.tick(2000); CHECK(fps == 10);
    f.tick(3000, 1000, 0); CHECK(fps == 10);
    f.rates.active = 0; f.rates.idle = 10;
    f.tick(4000, 1000, 0); CHECK(fps == 10);
    f.rates.idle = 0;
    f.tick(5000, 1000, 0); CHECK(fps == 0);
}
TEST_CASE("Play and Pause preserve authored uncapped settings and Stop resumes authoring") {
    Fixture f; f.begin(); JceRenderer renderer;
    JceEditorPlayPresentationState presentation;
    JceRenderSettings authored = {}; authored.vsync = 0; authored.target_framerate = 0;
    jce_editor_play_presentation_begin(presentation, &renderer, authored, 0);
    CHECK(fps == 0); const auto writes = fps_writes;
    f.tick(10000, 1000, 0, true, false, true);
    f.tick(20000, 1000, JCE_WINDOW_INPUT_FOCUS, true, true, true);
    CHECK(fps == 0); CHECK(fps_writes == writes);
    CHECK(f.state.mode == JceEditorCadenceMode::Suspended);
    jce_editor_play_presentation_restore(presentation); CHECK(fps == 60);
    f.tick(25000); CHECK(fps == 60);
    f.tick(27000); CHECK(fps == 30);
    jce_editor_cadence_end(f.state); CHECK(fps == 0);
}
TEST_CASE("authored high refresh Play and failed-create restoration retain the editor entry value") {
    Fixture f; f.begin(); JceRenderer renderer;
    JceEditorPlayPresentationState presentation;
    JceRenderSettings authored = {}; authored.vsync = 1; authored.target_framerate = 144;
    jce_editor_play_presentation_begin(presentation, &renderer, authored, 0);
    f.tick(3000, 1000, 0, true, false, true); CHECK(fps == 144); CHECK(renderer.vsync);
    jce_editor_play_presentation_restore(presentation); CHECK(fps == 60); CHECK_FALSE(renderer.vsync);
    f.tick(4000); CHECK(fps == 60);
}

TEST_CASE("actual main and Play host wire compose legacy background defaults and authored FPS")
{
    for (const int preference : {-1, 15, 0, 240}) {
        for (const int authored_fps : {0, 144}) {
            INFO("stored background " << preference << ", authored FPS " << authored_fps);
            Fixture f;
            JceEditorConfig cfg;
            jce_editor_config_defaults(&cfg);
            if (preference >= 0)
                REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, preference));
            f.rates = jce_editor_cadence_rates(cfg);
            f.rates.play = 0; // Existing authored-only background controls opt out.
            const int stop_rate = preference < 0 ? 5 : preference;
            const int play_rate = preference < 0 ? 15 : preference;
            const int effective_stop = preference == 0 || preference == 240 ? 30 : stop_rate;
            const int effective_background = authored_fps == 144 && (play_rate == 0 || play_rate > 144)
                ? 144 : play_rate;
            const int paused_background = effective_background == 0 || effective_background > 30
                ? 30 : effective_background;
            CHECK(f.rates.background == stop_rate);
            CHECK(f.rates.play_background == play_rate);
            f.begin();
            JceRenderer renderer;
            auto &presentation = s_play_host.presentation;
            JceRenderSettings authored = {};
            authored.target_framerate = authored_fps;
            jce_editor_play_presentation_begin(presentation, &renderer, authored, 0);
            jce_lifecycle_shutdown();
            jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
            s.play_state = JCE_PLAY_PLAYING;
            /* The actual main/receiver wire must replace this witness value. */
            REQUIRE(jce_runtime_host_policy_begin(&s_play_host.policy, nullptr,
                                                  false, true, 333, 333));
            actual_main_play_wire(f.rates);
            CHECK(s_play_host.policy.background_fps == effective_background);
            CHECK(s_play_host.policy.paused_fps == 30);
            CHECK(fps == authored_fps);
            jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
            actual_main_play_wire(f.rates);
            CHECK(fps == effective_background);
            CHECK(s_play_host.policy.running_fps == authored_fps);
            jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
            actual_main_play_wire(f.rates);
            CHECK(fps == authored_fps);
            s.play_state = JCE_PLAY_PAUSED;
            actual_main_play_wire(f.rates);
            CHECK(fps == 30);
            jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
            actual_main_play_wire(f.rates);
            CHECK(fps == paused_background);
            jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
            actual_main_play_wire(f.rates);
            CHECK(fps == 30);
            s.play_state = JCE_PLAY_PLAYING;
            actual_main_play_wire(f.rates);
            CHECK(fps == authored_fps);
            jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
            actual_main_play_wire(f.rates);
            CHECK(fps == effective_background);
            jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
            actual_main_play_wire(f.rates);
            CHECK(fps == authored_fps);
            jce_runtime_host_policy_end(&s_play_host.policy);
            s.play_state = JCE_PLAY_STOPPED;
            const auto writes = fps_writes;
            actual_main_play_wire(f.rates);
            CHECK(fps_writes == writes);
            jce_editor_play_presentation_restore(presentation);
            f.tick(9000, 1000, 0);
            CHECK(fps == effective_stop);
            jce_editor_cadence_end(f.state);
            jce_lifecycle_shutdown();
        }
    }
}

TEST_CASE("preview preference defaults invalid values and zero use the canonical config owner") {
    JceEditorConfig cfg; jce_editor_config_defaults(&cfg);
    CHECK(jce_editor_cadence_rates(cfg).play == 60);
    for (const int value : {0,144,1000}) {
        REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_PLAY, value));
        CHECK(jce_editor_cadence_rates(cfg).play == value);
    }
    for (const int value : {-1,1001}) {
        REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_PLAY, value));
        CHECK(jce_editor_cadence_rates(cfg).play == 60);
        CHECK(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_PLAY, 0) == value);
    }
}
TEST_CASE("actual Play main wire applies preview cap live and restores it across temporary modes") {
    struct Rate { int project, preference, expected; };
    const Rate rates[] = {{0,-1,60}, {30,-1,30}, {144,-1,60},
                          {0,0,0}, {144,0,144}, {0,144,144}, {144,240,144}};
    for (const auto &rate : rates) {
        INFO("project " << rate.project << ", preview " << rate.preference);
        Fixture f; JceEditorConfig cfg; jce_editor_config_defaults(&cfg);
        if (rate.preference >= 0)
            REQUIRE(jce_editor_config_set_ui_int(&cfg, JCE_EDITOR_CADENCE_PLAY, rate.preference));
        f.rates = jce_editor_cadence_rates(cfg); f.begin();
        JceRenderer renderer;
        JceRenderSettings authored = {}; authored.target_framerate = rate.project;
        auto &presentation = s_play_host.presentation;
        jce_editor_play_presentation_begin(presentation, &renderer, authored, f.rates.play);
        jce_lifecycle_shutdown(); jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        s.play_state = JCE_PLAY_PLAYING;
        REQUIRE(jce_runtime_host_policy_begin(&s_play_host.policy,nullptr,false,true,30,15));
        actual_main_play_wire(f.rates); CHECK(fps == rate.expected);
        const auto stable_writes = fps_writes;
        actual_main_play_wire(f.rates); actual_main_play_wire(f.rates);
        CHECK(fps_writes == stable_writes);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
        actual_main_play_wire(f.rates); CHECK(fps == 15);
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED);
        actual_main_play_wire(f.rates); CHECK(fps == rate.expected);
        s.play_state = JCE_PLAY_PAUSED;
        actual_main_play_wire(f.rates); CHECK(fps == 30);
        const auto paused_writes = fps_writes;
        f.rates.play = 0;
        actual_main_play_wire(f.rates); CHECK(fps == 30);
        CHECK(fps_writes == paused_writes);
        CHECK(s_play_host.policy.running_fps == rate.project);
        s.play_state = JCE_PLAY_PLAYING;
        actual_main_play_wire(f.rates); CHECK(fps == rate.project);
        f.rates.play = 60;
        actual_main_play_wire(f.rates); CHECK(fps == (rate.project == 30 ? 30 : 60));
        /* The actual main loop also suspends authoring cadence during Play. */
        f.tick(8000, 1000, JCE_WINDOW_INPUT_FOCUS, true, false, true);
        CHECK(f.state.suspended);
        jce_runtime_host_policy_end(&s_play_host.policy);
        s.play_state = JCE_PLAY_STOPPED;
        jce_editor_play_presentation_restore(presentation); CHECK(fps == 60);
        f.tick(9000); CHECK(fps == 60);
        jce_editor_cadence_end(f.state); CHECK(fps == 0);
        jce_lifecycle_shutdown();
    }
}
