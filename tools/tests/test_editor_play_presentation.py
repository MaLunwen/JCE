"""Compile actual Play/Stop call sites against counted public CPU ports.

Fragments come from the current production owner, including its capture gate,
composition call and runtime-create failure transaction after viewport admission.
No Play implementation is cloned. Typed camera ports count admission/release
and enforce ordering; the dedicated camera owner verifies real value/heap state.
The scene-content port consumes the private declaration and checks host-root
forwarding only; content resolution has separate actual-owner controls.
Typed viewport ports admit this presentation seam only; the independent viewport fixture owns layout/options validity. These
controls prove wiring/ordering, not Editor UI or GPU pacing.
"""
import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def load(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


function = load("test_canvas_transparent_draw.py", "play_wire_source").production_function
execute = load("test_host_font_selection.py", "play_wire_compiler").execute_stock


PORTS = r"""
#include <cstdlib>
#include <stdio.h>
[[noreturn]] static void presentation_control_fail(const char *condition, int line)
{
    fprintf(stderr,"play presentation failed %d: %s\n",line,condition);
    fflush(stderr);
    std::exit(4);
}
#define PROBE_REQUIRE(value) do { if(!(value)) presentation_control_fail(#value,__LINE__); } while(false)
#include <string.h>
#include <jce/application/jce_runtime_stats_host.h>
#include <jce/application/jce_project.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include "__PRESENTATION__"
#include "__CADENCE__"
#include "__VIEWPORT__"
#include "__GAME_RENDER__"
#include "__SCENE_RENDER__"
#include "__TRANSACTION__"
#include "__COMPOSITION__"
#include "__PROJECT_OWNER__"
#include "__MODULE_OWNER__"
#include "__HOST_POLICY_OWNER__"
struct JceRenderer { bool vsync = false; bool fallback = false; };
static JceRenderer renderer;
static int target_fps = 45, creates, closes, captures, rollbacks;
static bool capture_ok = true, create_ok = true, rollback_ok = true;
static bool camera_ok = true, camera_live = false;
static int camera_begins, camera_ends, camera_acquired, camera_released, camera_detaches;
static bool desired_vsync = true, expected_vsync = true, settings_ok = true;
static int desired_fps = 30, expected_fps = 30, preview_cap = 60;
static int runtime_token, fs_token;
static char root_path[] = "/cpu-project", source_path[] = "Author", cooked_path[] = "Cooked";
static char scene_content_root[] = "/cpu-loose";
static JceProject project = {};
static bool isolated = false;
struct EditorHistorySnapshot {};
static EditorHistorySnapshot s_play_snapshot;
static bool s_play_snapshot_valid = false;
static JceAudio *s_play_audio = nullptr;
#define LOG_TAG "cpu-play-wire"
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
/* Presentation-only observation port. Preserve typed task outcomes without
 * replacing any Play result; canonical CPU/ring tracing has separate controls. */
struct JceEditorOperationScope {
    explicit JceEditorOperationScope(const char *) {}
    void result(JceTraceTaskState state) {
        PROBE_REQUIRE(state == JCE_TRACE_TASK_SUCCEEDED || state == JCE_TRACE_TASK_FAILED);
    }
};
bool jce_editor_config_load(JceEditorConfig *cfg) { (void)cfg; return true; }
int jce_editor_config_get_ui_int_or(const JceEditorConfig *cfg, const char *key, int fallback) {
    (void)cfg; return strcmp(key,JCE_EDITOR_CADENCE_PLAY) == 0 ? preview_cap : fallback;
}
extern "C" {
const char *jce_editor_scene_content_root(void) { return scene_content_root; }
bool jce_editor_game_render_begin_play_camera(void) {
    ++camera_begins;
    if (!camera_ok) return false;
    if (!camera_live) { camera_live = true; ++camera_acquired; }
    return true;
}
void jce_editor_game_render_end_play_camera(void) {
    ++camera_ends;
    if (camera_live) { camera_live = false; ++camera_released; }
}
void jce_editor_game_render_set_play_runtime(JceRuntime *runtime) {
    PROBE_REQUIRE(runtime == nullptr); ++camera_detaches;
}
bool jce_renderer_is_fallback(const JceRenderer *r) { PROBE_REQUIRE(r); return r->fallback; }
bool jce_renderer_get_vsync(const JceRenderer *r) { PROBE_REQUIRE(r && !r->fallback); return r->vsync; }
void jce_renderer_set_vsync(JceRenderer *r, bool enabled) {
    PROBE_REQUIRE(r && !r->fallback); r->vsync = enabled;
}
/* This fragment only borrows the newly composed host types. Policy/lifecycle
 * calls have dedicated actual-registry controls; reaching these ports here
 * would expand this presentation-only seam and must fail explicitly. */
bool JCE_CALL jce_lifecycle_is_focused(void) { PROBE_REQUIRE(false); return true; }
bool JCE_CALL jce_lifecycle_is_paused(void) { PROBE_REQUIRE(false); return false; }
JceLifecycleHandle JCE_CALL jce_lifecycle_register(JceLifecycleCallback, int32_t, void *) {
    PROBE_REQUIRE(false); JceLifecycleHandle handle = {}; return handle;
}
void JCE_CALL jce_lifecycle_unregister(JceLifecycleHandle) { PROBE_REQUIRE(false); }
void JCE_CALL jce_runtime_pause_audio(JceRuntime *) { PROBE_REQUIRE(false); }
void JCE_CALL jce_runtime_resume_audio(JceRuntime *) { PROBE_REQUIRE(false); }
int JCE_CALL jce_engine_get_target_fps(void) { return target_fps; }
void JCE_CALL jce_engine_set_target_fps(int fps) { PROBE_REQUIRE(fps >= 0); target_fps = fps; }
JceRenderSettings jce_render_settings_default(void) {
    JceRenderSettings rs = {}; rs.vsync = 1; return rs;
}
JceFileSystem *JCE_CALL jce_fs_get_active(void) {
    return isolated ? reinterpret_cast<JceFileSystem *>(&fs_token) : nullptr;
}
JceFsActivePolicy JCE_CALL jce_fs_get_active_policy(void) { return JCE_FS_ACTIVE_ISOLATED; }
bool JCE_CALL jce_platform_stats_available(const JcePlatformServices *) { return true; }
void jce_audio_destroy(JceAudio *) { PROBE_REQUIRE(false && "fixture owns no audio"); }
void jce_editor_game_render_resolve_extent(uint32_t w, uint32_t h, uint32_t *out_w, uint32_t *out_h) {
    PROBE_REQUIRE(out_w && out_h); *out_w = w; *out_h = h;
}
JceRuntime *JCE_CALL jce_runtime_create_with_options(
    const JceRuntimeDesc *, JcePlatformServices *, const JceRuntimeCreateOptions *options,
    size_t options_size) {
    PROBE_REQUIRE(options && options_size == sizeof *options);
    ++creates;
    PROBE_REQUIRE(camera_live);
    PROBE_REQUIRE(renderer.vsync == expected_vsync && target_fps == expected_fps);
    return create_ok ? reinterpret_cast<JceRuntime *>(&runtime_token) : nullptr;
}
}
extern "C" const JceProject *jce_editor_project_get(void) { return &project; }
JceRenderer *jce_editor_get_renderer(void) { return &renderer; }
bool jce_editor_effective_render_settings(bool requested_isolated,
    const char *requested_root, JceRenderSettings *out, std::string *,
    const char *requested_source, const char *requested_cooked,
    const char *requested_loose) {
    PROBE_REQUIRE(requested_isolated == isolated);
    PROBE_REQUIRE(requested_loose == scene_content_root);
    PROBE_REQUIRE(requested_root == root_path && requested_source == source_path &&
           requested_cooked == cooked_path);
    if (!settings_ok) return false;
    out->vsync = desired_vsync ? 1 : 0;
    out->target_framerate = desired_fps;
    return true;
}
static bool history_capture_snapshot(EditorHistorySnapshot *) { ++captures; return capture_ok; }
static bool history_restore_snapshot(const EditorHistorySnapshot &, const char *) {
    ++rollbacks; PROBE_REQUIRE(!camera_live && !renderer.vsync && target_fps == 45); return rollback_ok;
}
static bool counted_attach(JceRuntimeStatsHost *) { return true; }
static bool counted_close(JceRuntimeStatsHost *host) {
    ++closes; PROBE_REQUIRE(!camera_live && !host->runtime && !renderer.vsync && target_fps == 45); return true;
}
/* Count these unrelated service ports; their actual owner has dedicated tests. */
#define jce_runtime_stats_host_attach_env counted_attach
#define jce_runtime_stats_host_close counted_close
"""

CONTROL = r"""
/* Stop's actual camera detach/release and presentation restore are consumed.
 * Release only CPU-port-owned runtime/snapshot state between admitted sessions;
 * never repair camera/presentation/FPS/VSync, whose actual wires are under test. */
static void release_fixture_session(void) {
    PROBE_REQUIRE(!s_play_host.presentation.restore_pending && !camera_live);
    PROBE_REQUIRE(camera_acquired == camera_released);
    s_play_snapshot_valid = false;
    s_play_host.runtime_stats.runtime = nullptr;
}
int main(void) {
    project.project_root = root_path;
    project.source_assets = source_path;
    project.cooked_assets = cooked_path;
    capture_ok = false;
    wire_play();
    PROBE_REQUIRE(captures == 1 && creates == 0 && !renderer.vsync && target_fps == 45);
    PROBE_REQUIRE(camera_begins == 0 && camera_acquired == 0);
    PROBE_REQUIRE(!s_play_host.presentation.restore_pending);
    capture_ok = true;
    wire_play();
    PROBE_REQUIRE(renderer.vsync && target_fps == 30 && s_play_host.presentation.restore_pending);
    PROBE_REQUIRE(camera_live && camera_begins == 1 && camera_acquired == 1);
    wire_stop();
    PROBE_REQUIRE(!renderer.vsync && target_fps == 45 && !s_play_host.presentation.restore_pending);
    wire_stop();
    PROBE_REQUIRE(!renderer.vsync && target_fps == 45);
    PROBE_REQUIRE(!camera_live && camera_ends == 2 && camera_released == 1 && camera_detaches == 2);
    s_play_host.runtime_stats.runtime = nullptr; s_play_snapshot_valid = false;
    create_ok = false; rollback_ok = false; isolated = true;
    wire_play();
    PROBE_REQUIRE(closes == 1 && rollbacks == 1 && s_play_snapshot_valid);
    PROBE_REQUIRE(!camera_live && camera_acquired == 2 && camera_released == 2);
    PROBE_REQUIRE(!renderer.vsync && target_fps == 45 && !s_play_host.presentation.restore_pending);
    wire_stop();
    PROBE_REQUIRE(!renderer.vsync && target_fps == 45);
    /* Explicit uncapped values reach the same actual production apply call. */
    s_play_snapshot_valid = false; create_ok = true; rollback_ok = true;
    desired_vsync = expected_vsync = false; desired_fps = 0; expected_fps = 0; preview_cap = 0; renderer.vsync = true;
    wire_play();
    PROBE_REQUIRE(!renderer.vsync && target_fps == 0);
    wire_stop();
    PROBE_REQUIRE(renderer.vsync && target_fps == 45);
    /* Current config reaches the actual begin before runtime construction. */
    release_fixture_session();
    desired_fps = 0; expected_fps = 60; preview_cap = 60;
    wire_play(); PROBE_REQUIRE(target_fps == 60); wire_stop();
    release_fixture_session();
    desired_fps = 144; expected_fps = 60;
    wire_play(); PROBE_REQUIRE(target_fps == 60); wire_stop();
    release_fixture_session();
    desired_fps = 30; expected_fps = 30; preview_cap = 144;
    wire_play(); PROBE_REQUIRE(target_fps == 30); wire_stop();
    /* Missing composition retains entry VSync while the same FPS owner still begins. */
    release_fixture_session();
    settings_ok = false; expected_vsync = true;
    desired_fps = 30; preview_cap = 60; expected_fps = 60;
    wire_play(); PROBE_REQUIRE(renderer.vsync && target_fps == 60); wire_stop();
    PROBE_REQUIRE(renderer.vsync && target_fps == 45);
    release_fixture_session();
    preview_cap = 0; expected_fps = 0;
    wire_play(); PROBE_REQUIRE(renderer.vsync && target_fps == 0); wire_stop();
    PROBE_REQUIRE(renderer.vsync && target_fps == 45);
    release_fixture_session();
    preview_cap = 144; expected_fps = 144;
    wire_play(); PROBE_REQUIRE(renderer.vsync && target_fps == 144); wire_stop();
    PROBE_REQUIRE(renderer.vsync && target_fps == 45);
    /* Camera allocation refusal reaches the same rollback, retains a failed
     * snapshot, never creates Runtime, and permits a later admitted retry. */
    release_fixture_session();
    renderer.vsync = false; settings_ok = true;
    desired_vsync = expected_vsync = true;
    desired_fps = expected_fps = 30; preview_cap = 60;
    const int before_creates = creates, before_closes = closes;
    const int before_rollbacks = rollbacks, before_acquired = camera_acquired;
    camera_ok = false; rollback_ok = false;
    wire_play();
    PROBE_REQUIRE(creates == before_creates && closes == before_closes + 1);
    PROBE_REQUIRE(rollbacks == before_rollbacks + 1 && s_play_snapshot_valid);
    PROBE_REQUIRE(!camera_live && camera_acquired == before_acquired);
    PROBE_REQUIRE(camera_acquired == camera_released);
    PROBE_REQUIRE(!renderer.vsync && target_fps == 45 && !s_play_host.presentation.restore_pending);
    s_play_snapshot_valid = false; camera_ok = rollback_ok = true;
    wire_play(); PROBE_REQUIRE(camera_live && creates == before_creates + 1);
    wire_stop(); release_fixture_session();
    PROBE_REQUIRE(camera_acquired == before_acquired + 1 && camera_acquired == camera_released);
    puts("Play presentation wire PASS");
    return 0;
}
"""


def probe(mutation=None):
    source = (ROOT / "editor/src/core/jce_editor_play.cpp").read_text(encoding="utf-8")
    apply = function(source, "static JceEditorCadenceRates play_apply_presentation_settings(")
    play = function(source, "static void play_begin_ready(")
    stop = function(source, "void jce_state_stop(")
    begin = source.index("struct EditorPlayHost {")
    end = source.index("static EditorPlayHost s_play_host = {};", begin)
    state = source[begin:end] + "static EditorPlayHost s_play_host = {};\n"
    begin = play.index('    {\n        JceEditorOperationScope operation("ed_play_capture");')
    end = play.index("\n\n    s_play_audio =", begin)
    capture = play[begin:end]
    apply_wire = "    const auto cadence_rates = play_apply_presentation_settings();"
    if play.count(apply_wire) != 1:
        raise AssertionError("expected one actual Play presentation/config transaction")
    begin = play.index(apply_wire)
    end = play.index("    if (!created) return;", begin) + len("    if (!created) return;")
    creation = play[begin:end]
    begin = stop.index("        jce_editor_game_render_set_play_runtime(NULL);")
    end = stop.index("        /* Unload streamed chunks", begin)
    camera_restoration = stop[begin:end]
    begin = stop.index("        jce_editor_play_presentation_restore(s_play_host.presentation);")
    restoration = stop[begin:begin + len("        jce_editor_play_presentation_restore(s_play_host.presentation);")]
    if mutation == "apply":
        creation = creation.replace(apply_wire, "    const JceEditorCadenceRates cadence_rates;", 1)
    elif mutation == "create_cleanup":
        creation = creation.replace(
            "jce_editor_play_presentation_restore(s_play_host.presentation);", "(void)0;", 1)
    elif mutation == "camera_admission":
        wire = "            if (!jce_editor_game_render_begin_play_camera()) return false;"
        if creation.count(wire) != 1:
            raise AssertionError("expected one actual Play camera admission wire")
        creation = creation.replace(wire, "            (void)0;", 1)
    elif mutation == "camera_cleanup":
        wire = "            jce_editor_game_render_end_play_camera();"
        if creation.count(wire) != 1:
            raise AssertionError("expected one actual create-failure camera cleanup wire")
        creation = creation.replace(wire, "            (void)0;", 1)
    elif mutation == "stop":
        restoration = "        (void)0;"
    elif mutation == "preview":
        wire = "jce_editor_get_renderer(), settings, rates.play, resolved);"
        if apply.count(wire) != 1:
            raise AssertionError("expected one actual Play preview configuration wire")
        apply = apply.replace(wire, "jce_editor_get_renderer(), settings, 0, resolved);", 1)
    elif mutation == "unresolved_gate":
        wire = "    JceEditorConfig config;"
        if apply.count(wire) != 1 or "const bool resolved =" not in apply:
            raise AssertionError("expected one actual Play composition/config boundary")
        apply = apply.replace(wire, "    if (!resolved) return JceEditorCadenceRates{};\n" + wire, 1)
    ports = PORTS.replace("__PRESENTATION__", (ROOT / "editor/src/core/jce_editor_play_presentation.h").as_posix())
    ports = ports.replace("__CADENCE__", (ROOT / "editor/src/core/jce_editor_cadence.h").as_posix())
    ports = ports.replace("__VIEWPORT__", (ROOT / "editor/src/core/jce_editor_play_viewport.h").as_posix())
    ports = ports.replace("__GAME_RENDER__", (ROOT / "editor/src/scene/jce_editor_game_render.h").as_posix())
    ports = ports.replace("__SCENE_RENDER__", (ROOT / "editor/src/scene/jce_editor_scene_render.h").as_posix())
    ports = ports.replace("__TRANSACTION__", (ROOT / "editor/src/core/jce_editor_play_transaction.h").as_posix())
    ports = ports.replace("__COMPOSITION__", (ROOT / "editor/src/core/jce_editor_effective_render_settings.h").as_posix())
    ports = ports.replace("__PROJECT_OWNER__", (ROOT / "editor/src/core/jce_editor_project.h").as_posix())
    ports = ports.replace("__MODULE_OWNER__", (ROOT / "editor/src/core/jce_editor_play_module.h").as_posix())
    ports = ports.replace("__HOST_POLICY_OWNER__", (ROOT / "engine/include/jce/application/jce_runtime_host_policy.inc.h").as_posix())
    return (ports + state + apply + "\nstatic void wire_play(void) {\n" + capture +
            "\nJceRuntimeDesc rd = {};\n" + creation + "\n}\nstatic void wire_stop(void) {\n" +
            camera_restoration + restoration + "\n}\n" + CONTROL)


class PlayPresentationWireTests(unittest.TestCase):
    def test_actual_play_stop_and_failed_create_wiring(self):
        self.assertIn("Play presentation wire PASS", execute(self, probe()))

    def test_removed_camera_admission_fails_at_runtime(self):
        execute(self, probe("camera_admission"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_removed_camera_create_cleanup_fails_at_runtime(self):
        execute(self, probe("camera_cleanup"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_removed_play_apply_wire_fails_at_runtime(self):
        execute(self, probe("apply"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_removed_create_cleanup_restore_fails_at_runtime(self):
        execute(self, probe("create_cleanup"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_removed_actual_preview_config_wire_fails_at_runtime(self):
        execute(self, probe("preview"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_old_unresolved_settings_gate_fails_at_runtime(self):
        execute(self, probe("unresolved_gate"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")

    def test_removed_stop_restore_fails_at_runtime(self):
        execute(self, probe("stop"), negative=True,
            negative_exit=4, negative_marker="play presentation failed ")


if __name__ == "__main__":
    unittest.main()
