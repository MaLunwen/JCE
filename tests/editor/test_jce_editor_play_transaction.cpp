#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include "../../editor/src/core/jce_editor_play_transaction.h"
#include "../../editor/src/core/jce_editor_state.h"
#include <string>

TEST_CASE("capture failure cannot admit a mutable runtime and can be retried") {
    bool valid = false;
    int scene = 7, snapshot = -1, creates = 0;
    CHECK_FALSE(jce_editor_play_capture(valid,
        [&] { snapshot = 99; return false; }, [&] { snapshot = -1; }));
    CHECK(snapshot == -1);
    CHECK_FALSE(jce_editor_play_create(valid,
        [&] { ++creates; scene = 8; return true; }, [] {}, [] { return true; }, [] {}));
    CHECK(creates == 0);
    CHECK(scene == 7);
    REQUIRE(jce_editor_play_capture(valid,
        [&] { snapshot = scene; return true; }, [] {}));
    CHECK(jce_editor_play_create(valid,
        [&] { ++creates; scene = 8; return true; }, [] {}, [] { return true; }, [] {}));
    CHECK(creates == 1);
    CHECK(valid);
}

TEST_CASE("failed runtime construction releases owners before authored rollback") {
    bool valid = true, audio = true, stats = true;
    int scene = 99, snapshot = 7;
    std::string order;
    CHECK_FALSE(jce_editor_play_create(valid,
        [&] { order += 'C'; scene = 100; return false; },
        [&] { order += 'T'; audio = stats = false; },
        [&] { order += 'R'; CHECK_FALSE(audio); CHECK_FALSE(stats);
              scene = snapshot; return true; },
        [&] { order += 'D'; snapshot = -1; }));
    CHECK(order == "CTRD");
    CHECK(scene == 7);
    CHECK_FALSE(valid);
    REQUIRE(jce_editor_play_capture(valid,
        [&] { snapshot = scene; return true; }, [] {}));
    CHECK(jce_editor_play_create(valid, [] { return true; }, [] {},
        [] { return false; }, [] {}));
}

TEST_CASE("rollback failure preserves snapshot until explicit Stop retry succeeds") {
    bool valid = true;
    int snapshot = 7, creates = 0;
    CHECK_FALSE(jce_editor_play_create(valid, [] { return false; }, [] {},
        [] { return false; }, [&] { snapshot = -1; }));
    CHECK(valid);
    CHECK(snapshot == 7);
    CHECK_FALSE(jce_editor_play_capture(valid,
        [&] { ++creates; return true; }, [&] { snapshot = -1; }));
    CHECK(creates == 0);
    CHECK_FALSE(jce_editor_play_restore(valid, [] { return false; },
        [&] { snapshot = -1; }));
    CHECK(snapshot == 7);
    CHECK(jce_editor_play_restore(valid, [] { return true; },
        [&] { snapshot = -1; }));
    CHECK_FALSE(valid);
    CHECK(snapshot == -1);
    CHECK(jce_editor_play_restore(valid, [] { FAIL("no second restore"); return false; }, [] {}));
}

/* Count only the host sequencing contract. Real VM callbacks remain covered
 * by the C/native backend suites and the actual Editor exit capture. */
struct ExitSequence {
    JcePlayState state = JCE_PLAY_STOPPED;
    bool game_alive = true, scene_alive = true, editor_alive = true;
    bool pending = false, snapshot_valid = false, fail_restore = false;
    int stops = 0, cleanup_calls = 0;
    std::string order;

    void stop() {
        CHECK(game_alive);
        CHECK(scene_alive);
        CHECK(editor_alive);
        ++stops;
        order += 'P';
        pending = false;
        state = JCE_PLAY_STOPPED;
        jce_editor_play_restore(snapshot_valid,
            [&] { return !fail_restore; }, [] {});
    }
    void shutdown() {
        CHECK(state == JCE_PLAY_STOPPED);
        CHECK_FALSE(pending);
        ++cleanup_calls;
        order += "GSU";
        game_alive = scene_alive = editor_alive = false;
    }
    void exit() {
        jce_editor_exit_after_play(state != JCE_PLAY_STOPPED,
            [&] { stop(); }, [&] { shutdown(); });
    }
};

TEST_CASE("normal host exit stops active Play before Game Scene and editor owners") {
    ExitSequence host;
    host.state = JCE_PLAY_PLAYING;
    host.exit();
    CHECK(host.order == "PGSU");
    CHECK(host.stops == 1);
    CHECK(host.cleanup_calls == 1);
}

TEST_CASE("normal host exit stops paused Play through the same authority") {
    ExitSequence host;
    host.state = JCE_PLAY_PAUSED;
    host.exit();
    CHECK(host.order == "PGSU");
    CHECK(host.stops == 1);
    CHECK(host.cleanup_calls == 1);
}

TEST_CASE("repeated host exit cannot repeat an already completed Play Stop") {
    ExitSequence host;
    host.state = JCE_PLAY_PLAYING;
    host.exit();
    host.exit();
    CHECK(host.order == "PGSUGSU");
    CHECK(host.stops == 1);
    CHECK(host.cleanup_calls == 2);
}

TEST_CASE("ordinary Edit exit performs no simulation callback") {
    ExitSequence host;
    host.exit();
    CHECK(host.order == "GSU");
    CHECK(host.stops == 0);
    CHECK(host.cleanup_calls == 1);
}

TEST_CASE("explicit Stop before exit is not repeated by host teardown") {
    ExitSequence host;
    host.state = JCE_PLAY_PLAYING;
    host.stop();
    host.exit();
    CHECK(host.order == "PGSU");
    CHECK(host.stops == 1);
}

TEST_CASE("failed runtime creation leaves no active Play for normal host exit") {
    ExitSequence host;
    host.snapshot_valid = true;
    bool stats = true, audio = true;
    int teardown = 0;
    CHECK_FALSE(jce_editor_play_create(host.snapshot_valid,
        [] { return false; },
        [&] { ++teardown; stats = audio = false; },
        [] { return true; }, [] {}));
    CHECK(teardown == 1);
    CHECK_FALSE(stats);
    CHECK_FALSE(audio);
    host.exit();
    CHECK(host.order == "GSU");
    CHECK(host.stops == 0);
    CHECK(host.cleanup_calls == 1);
}

TEST_CASE("retained restore failure does not abandon downstream host cleanup") {
    ExitSequence host;
    host.state = JCE_PLAY_PLAYING;
    host.snapshot_valid = host.fail_restore = true;
    host.exit();
    CHECK(host.snapshot_valid);
    CHECK(host.state == JCE_PLAY_STOPPED);
    CHECK(host.order == "PGSU");
    CHECK(host.stops == 1);
    CHECK(host.cleanup_calls == 1);
}

TEST_CASE("Play Stop settles pending owner work before dependent views close") {
    ExitSequence host;
    host.state = JCE_PLAY_PLAYING;
    host.pending = true;
    host.exit();
    CHECK_FALSE(host.pending);
    CHECK(host.order == "PGSU");
    CHECK(host.stops == 1);
}


/* Compile the complete actual host exit owner as well as the unchanged pure
 * Play sequencing cases above. These counted boundary ports do not create a
 * recorder worker, audio device, renderer or native application. */
#ifndef JCE_TEST_EDITOR_EXIT_BODY
#error actual Editor exit owner is required
#endif
#include "core/jce_editor.h"
#include "core/jce_editor_cadence.h"
#include "core/jce_editor_headless_build.h"
#include "core/jce_build_manager.h"
#include "core/jce_editor_recorder.h"
#include "scene/jce_editor_game_render.h"
#include "scene/jce_editor_scene_render.h"
#include <jce/os/platform/jce_host_dialog.h>

static struct {
    bool startup_config_valid = true;
    bool cpu_services_ready = true;
    JceEditorCadence cadence;
} g_state;
static bool g_headless_build_mode;
static struct {
    JcePlayState state = JCE_PLAY_STOPPED;
    bool recorder = false, capture_sink = false, audio_tap = false, worker = false;
    bool game = true, scene = true, editor = true;
    int recorder_calls = 0, joins = 0, stops = 0, headless = 0, builds = 0;
    int fps = 60;
    std::string order;
} exit_ports;

static void editor_shutdown_diag(const char *) {}
static void check_recorder_retired()
{
    CHECK_FALSE(g_state.cpu_services_ready);
    CHECK_FALSE(exit_ports.recorder);
    CHECK_FALSE(exit_ports.capture_sink);
    CHECK_FALSE(exit_ports.audio_tap);
    CHECK_FALSE(exit_ports.worker);
}
extern "C" int jce_engine_get_target_fps(void) { return exit_ports.fps; }
extern "C" void jce_engine_set_target_fps(int fps)
{
    CHECK_FALSE(exit_ports.editor);
    exit_ports.fps = fps;
    exit_ports.order += 'C';
}
void jce_editor_headless_build_shutdown() { ++exit_ports.headless; }
extern "C" void jce_build_manager_shutdown() { ++exit_ports.builds; }
extern "C" void jce_host_dialog_set_parent_jce_window(JceWindow *window)
{
    CHECK(window == nullptr);
    CHECK_FALSE(g_state.cpu_services_ready);
    exit_ports.order += 'D';
}
extern "C" void jce_editor_recorder_stop(void)
{
    ++exit_ports.recorder_calls;
    if (!exit_ports.recorder) return;
    CHECK(exit_ports.game); CHECK(exit_ports.scene); CHECK(exit_ports.editor);
    exit_ports.capture_sink = exit_ports.audio_tap = exit_ports.worker = false;
    exit_ports.recorder = false;
    ++exit_ports.joins;
    exit_ports.order += 'R';
}
extern "C" JcePlayState jce_state_get_play_state(void) { return exit_ports.state; }
extern "C" void jce_state_stop(void)
{
    check_recorder_retired();
    CHECK(exit_ports.game); CHECK(exit_ports.scene); CHECK(exit_ports.editor);
    ++exit_ports.stops;
    exit_ports.state = JCE_PLAY_STOPPED;
    exit_ports.order += 'P';
}
extern "C" void jce_editor_play_bind_services(const JceServices *services)
{
    CHECK(services == nullptr);
    check_recorder_retired();
    CHECK(exit_ports.state == JCE_PLAY_STOPPED);
    exit_ports.order += 'B';
}
extern "C" void jce_editor_game_render_shutdown(void)
{
    check_recorder_retired();
    exit_ports.game = false;
    exit_ports.order += 'G';
}
extern "C" void jce_editor_scene_render_shutdown(void)
{
    check_recorder_retired();
    CHECK_FALSE(exit_ports.game);
    exit_ports.scene = false;
    exit_ports.order += 'S';
}
extern "C" void jce_editor_shutdown(void)
{
    check_recorder_retired();
    CHECK_FALSE(exit_ports.scene);
    exit_ports.editor = false;
    exit_ports.order += 'U';
}
#include JCE_TEST_EDITOR_EXIT_BODY

static void reset_actual_exit(JcePlayState state, bool recording)
{
    exit_ports = {};
    exit_ports.state = state;
    exit_ports.recorder = exit_ports.capture_sink = exit_ports.audio_tap =
        exit_ports.worker = recording;
    g_state = {};
    g_state.cadence.initialized = true;
    g_state.cadence.suspended = true;
    g_state.cadence.previous_fps = 30;
    g_state.cadence.last_applied = 60;
    g_headless_build_mode = false;
}

TEST_CASE("actual GUI exit retires recorder before active or paused Play and views")
{
    for (const JcePlayState state : {JCE_PLAY_PLAYING, JCE_PLAY_PAUSED}) {
        reset_actual_exit(state, true);
        editor_app_exit(nullptr);
        CHECK(exit_ports.order == "DRPBGSUC");
        CHECK(exit_ports.recorder_calls == 1); CHECK(exit_ports.joins == 1);
        CHECK(exit_ports.stops == 1); CHECK(exit_ports.fps == 30);
        CHECK_FALSE(g_state.cadence.initialized);
    }
}
TEST_CASE("actual Edit exit and inactive recorder keep the normal teardown route")
{
    for (const bool recording : {false, true}) {
        reset_actual_exit(JCE_PLAY_STOPPED, recording);
        editor_app_exit(nullptr);
        CHECK(exit_ports.order == (recording ? "DRBGSUC" : "DBGSUC"));
        CHECK(exit_ports.recorder_calls == 1);
        CHECK(exit_ports.joins == (recording ? 1 : 0)); CHECK(exit_ports.stops == 0);
    }
}
TEST_CASE("actual repeated exit and prior F9 stop do not join a recorder twice")
{
    reset_actual_exit(JCE_PLAY_PLAYING, true);
    editor_app_exit(nullptr);
    editor_app_exit(nullptr);
    CHECK(exit_ports.recorder_calls == 2); CHECK(exit_ports.joins == 1);
    CHECK(exit_ports.stops == 1);
    reset_actual_exit(JCE_PLAY_STOPPED, true);
    jce_editor_recorder_stop(); /* Prior F9 Stop while the application is live. */
    CHECK(g_state.cpu_services_ready);
    editor_app_exit(nullptr);
    CHECK(exit_ports.recorder_calls == 2); CHECK(exit_ports.joins == 1);
}
TEST_CASE("actual invalid startup and headless exit do not borrow GUI recorder owners")
{
    reset_actual_exit(JCE_PLAY_STOPPED, false);
    g_state.startup_config_valid = false;
    editor_app_exit(nullptr);
    CHECK(exit_ports.order.empty()); CHECK(exit_ports.recorder_calls == 0);
    CHECK_FALSE(g_state.cpu_services_ready);
    reset_actual_exit(JCE_PLAY_STOPPED, false);
    g_headless_build_mode = true;
    editor_app_exit(nullptr);
    CHECK(exit_ports.headless == 1); CHECK(exit_ports.builds == 1);
    CHECK(exit_ports.order.empty()); CHECK(exit_ports.recorder_calls == 0);
    CHECK_FALSE(g_state.cpu_services_ready);
}
