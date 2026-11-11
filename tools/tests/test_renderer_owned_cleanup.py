"""Execute the production renderer shader/uniform teardown with ownership spies."""
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

class RendererCleanupTests(unittest.TestCase):
    def test_sdf_and_invalid_handles(self):
        spec = importlib.util.spec_from_file_location("renderer_cleanup_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        source = (ROOT / "engine/src/renderer/jce_renderer.c").read_text(encoding="utf-8")
        start = source.index("    s_dbg_text_enabled = false;", source.index("void jce_renderer_destroy_impl("))
        end = source.index("    /* Subsystems and path-keyed", start)
        cleanup = source[start:end]
        required = {"program_text_sdf", "u_sdf_params", "u_sdf_outline", "u_sdf_shadow_offset", "u_sdf_shadow_color"}
        owned = (ROOT / "engine/src/renderer/jce_shader_set_owned_internal.h").read_text(encoding="utf-8")
        members = set(re.findall(r"X\(\w+, (\w+)\)", owned))
        fields = sorted(set(re.findall(r"r->(\w+)", cleanup)) | required | members)
        snapshot_start = source.index("static JceShaderSet renderer_owned_shader_set(")
        snapshot_end = source.index("\nbool jce_renderer_reload_shaders_fs", snapshot_start)
        snapshot = source[snapshot_start:snapshot_end]
        harness = "\n".join([
            "#include <assert.h>", "#include <stdint.h>", "#include <stdbool.h>",
            "#include <stdio.h>", "#include <string.h>",
            '#include "renderer/jce_shader_set_owned_internal.h"',
            "typedef JceShaderHandle bgfx_uniform_handle_t;",
            "typedef struct {" + "".join("JceShaderHandle " + f + ";" for f in fields) +
            "JceShaderHandle program_variant[JCE_SHADER_VARIANT_COUNT][JCE_SHADER_KEY_COUNT];} JceRenderer;",
            "static bool s_dbg_text_enabled;static unsigned programs,uniforms;",
            "void jce_shader_program_destroy(JceShaderHandle p){assert(p.idx==42||p.idx==47||p.idx==0);programs++;}",
            snapshot,
            "static void bgfx_destroy_uniform(bgfx_uniform_handle_t u){assert(u.idx>=43&&u.idx<=46);uniforms++;}",
            "static void cleanup(JceRenderer *r){" + cleanup + "}",
            "int main(void){JceRenderer r;memset(&r,255,sizeof r);r.program_text_sdf.idx=42;",
            "r.u_sdf_params.idx=43;r.u_sdf_outline.idx=44;r.u_sdf_shadow_offset.idx=45;r.u_sdf_shadow_color.idx=46;",
            "r.program_variant[0][0].idx=42;r.program_variant[1][0].idx=47;",
            "r.program_variant[1][1].idx=47;r.program.idx=0;",
            "cleanup(&r);assert(programs==3&&uniforms==4);memset(&r,255,sizeof r);",
            "cleanup(&r);assert(programs==3&&uniforms==4);puts(\"Renderer owned cleanup PASS\");return 0;}"
        ])
        base = ROOT / "build/tests/renderer-cleanup-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            if driver.HOST == "windows":
                command = ["cl", "/nologo", "/TC", "/std:c11", "/W3", "/I" + str(ROOT / "engine/src"), "/I" + str(ROOT / "engine/include"), str(file), "/Fe:probe.exe", "/Fo:probe.obj"]
                env = driver.msvc_env(driver.resolve_target(None))
                executable = directory / "probe.exe"
            else:
                compiler = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
                if not compiler:
                    self.skipTest("no C compiler available for production teardown probe")
                executable = directory / "probe"
                command = [compiler, "-std=c99", "-I" + str(ROOT / "engine/src"), "-I" + str(ROOT / "engine/include"), str(file), "-o", str(executable)]
                env = None
            result = subprocess.run(command, cwd=directory, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Renderer owned cleanup PASS", result.stdout)


    def test_device_loss_interleave_and_backend_session_reset(self):
        from tools.tests.test_material_owned_cleanup import execute, function
        text = (ROOT / "engine/src/renderer/jce_renderer_bgfx_callback.c").read_text(encoding="utf-8")
        start = text.index("static struct {")
        end = text.index(";", text.index("} g_rcb_host =", start)) + 1
        storage = text[start:end]
        fatal = function(text, "static void jce_bgfx_fatal(")
        consume = function(text, "bool jce_renderer_take_device_lost(void)")
        begin = function(text, "bgfx_callback_interface_t *jce_rcb_callback_interface(void)")
        header = (ROOT / "engine/src/renderer/jce_renderer_bgfx_callback.h").read_text(encoding="utf-8")
        enum_start = header.index("typedef enum JceBgfxCpuProfilerAttemptResult {")
        enum_end = header.index("} JceBgfxCpuProfilerAttemptResult;", enum_start) + len("} JceBgfxCpuProfilerAttemptResult;")
        attempt_result = header[enum_start:enum_end]
        prefix = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define LOG_ERROR(...) ((void)0)
typedef struct { struct { int value; } v; } JceAtomicI32;
typedef void (*JceAutoCaptureFn)(void);
typedef struct { int dummy; } bgfx_callback_interface_t;
typedef int bgfx_fatal_t;
#define BGFX_FATAL_DEVICE_LOST 4
static bgfx_callback_interface_t s_bgfx_callback;
static bool inject_event;
/* Capability publication is outside this device-loss interleave fixture.
 * Keep its actual factory call typed and intact. The complete-vtbl Native
 * fixture separately tests the real publisher, metadata and observed counts. */
typedef int JceTraceTaskState;
#define JCE_TRACE_TASK_SUCCEEDED 1
static void jce_bgfx_cpu_profiler_publish(const char *phase, const char *scope_name,
                                         JceTraceTaskState outcome) {
    (void)phase; (void)scope_name; (void)outcome;
}
static char *fixture_environment(const char *name) {(void)name; return NULL;}
#define getenv fixture_environment
/* Deterministic adversarial schedule: a producer arrives after the atomic
 * exchange, or between the broken replacement's separate read and clear. */
static int32_t jce_atomic_i32_load(const JceAtomicI32 *a) {
    int old=a->v.value;
    if(inject_event) { inject_event=false; ((JceAtomicI32 *)a)->v.value=1; }
    return old;
}
static void jce_atomic_i32_store(JceAtomicI32 *a,int32_t value) {a->v.value=value;}
static int32_t jce_atomic_i32_exchange(JceAtomicI32 *a,int32_t value) {
    int old=a->v.value; a->v.value=value;
    if(inject_event) { inject_event=false; a->v.value=1; }
    return old;
}
"""
        body = r"""
int main(void) {
    if(jce_renderer_take_device_lost()) return 1;
    jce_bgfx_fatal(NULL,"fixture",1,0,"non-device loss");
    if(jce_renderer_take_device_lost()) return 2;
    jce_bgfx_fatal(NULL,"fixture",2,BGFX_FATAL_DEVICE_LOST,"device loss");
    inject_event=true;
    if(!jce_renderer_take_device_lost() || !jce_renderer_take_device_lost()
       || jce_renderer_take_device_lost()) {
        fputs("Device-lost interleave lost signal",stderr); return 17;
    }
    jce_bgfx_fatal(NULL,"fixture",3,BGFX_FATAL_DEVICE_LOST,"old session");
    if(jce_rcb_callback_interface()!=&s_bgfx_callback || jce_renderer_take_device_lost()) {
        fputs("Device-lost session reset retained signal",stderr); return 18;
    }
    return 0;
}
"""
        def harness(take, factory):
            terminal_port = """void jce_rcb_cpu_profiler_attempt_end(JceBgfxCpuProfilerAttemptResult result) {
                (void)result;
            }"""
            return "\n".join((prefix, attempt_result, terminal_port, storage, fatal, take, factory, body))
        execute(self, harness(consume, begin))
        broken = """bool jce_renderer_take_device_lost(void) {
            if(!jce_atomic_i32_load(&g_rcb_host.device_lost)) return false;
            jce_atomic_i32_store(&g_rcb_host.device_lost,0); return true;
        }"""
        with self.assertRaisesRegex(AssertionError, "Device-lost interleave lost signal"):
            execute(self, harness(broken, begin))
        no_reset = begin.replace("jce_atomic_i32_store(&g_rcb_host.device_lost, 0);", "")
        self.assertNotEqual(begin, no_reset)
        with self.assertRaisesRegex(AssertionError, "Device-lost session reset retained signal"):
            execute(self, harness(consume, no_reset))


    def _vsync_cpu_probe(self, old_wrapper=False):
        from tools.tests.test_material_owned_cleanup import function
        source = (ROOT / "engine/src/renderer/jce_renderer.c").read_text(encoding="utf-8")
        sized = function(source, "void jce_renderer_set_vsync_for_size(")
        wrapper = function(source, "void jce_renderer_set_vsync(")
        policy = (ROOT / "engine/src/renderer/jce_renderer_launch_policy.inc.h").read_text(encoding="utf-8")
        launch_resolve = function(policy, "static bool renderer_launch_vsync_resolve(")
        presentation_resolve = function(policy, "static bool renderer_presentation_vsync_resolve(")
        helper = (ROOT / "engine/src/renderer/jce_renderer_reset_trace_internal.h").read_text(encoding="utf-8")
        enum_start = helper.index("typedef enum JceRendererResetReason {")
        enum_end = helper.index("} JceRendererResetReason;", enum_start) + len("} JceRendererResetReason;")
        reset_reason = helper[enum_start:enum_end]
        if old_wrapper:
            # Exact historical production wrapper before its early guards were
            # delegated to for_size. The stats port stays safe without bgfx;
            # the ordinary runtime count failure is the negative witness.
            wrapper = r"""void jce_renderer_set_vsync(JceRenderer *r, bool enabled)
{
    const bgfx_stats_t *stats = bgfx_get_stats();
    jce_renderer_set_vsync_for_size(r, enabled, stats->width, stats->height);
}"""
        ports = r"""
#include <jce/renderer/jce_renderer.h>
#include <stdio.h>
#define BGFX_RESET_VSYNC 0x80u
#define BGFX_TEXTURE_FORMAT_COUNT 37
#define OTHER_FLAGS 0x200u
#include "renderer/jce_renderer_launch_policy_internal.h"
struct JceRenderer {
    bool is_fallback;
    uint32_t reset_flags;
    JceRendererLaunchVsync launch_vsync;
};
typedef struct { uint32_t width, height; } bgfx_stats_t;
static bgfx_stats_t stats = {1643u, 987u};
static unsigned queries, resets;
static uint32_t last_width, last_height, last_flags;
static int last_format;
static const bgfx_stats_t *bgfx_get_stats(void) { ++queries; return &stats; }
/* Only trace observation is isolated. The actual private state and complete
 * launch/presentation resolvers are consumed below. Window observation and
 * complete real-ring publication belong to the Native renderer reset fixture. */
static void jce_renderer_launch_vsync_observe_internal(JceRenderer *r, int phase)
{ (void)r; (void)phase; }
static void bgfx_reset(uint32_t width, uint32_t height, uint32_t flags, int format) {
    ++resets; last_width=width; last_height=height; last_flags=flags; last_format=format;
}
"""
        # This fixture owns early guards and exact reset arguments. The complete
        # real-ring reset fixture independently consumes the actual trace helper.
        # Keep the unrelated observation dependency typed and backend-forwarding.
        reset_port = r"""
static void renderer_reset_cpu(JceRendererResetReason reason,
    uint32_t requested_width, uint32_t requested_height,
    uint32_t call_width, uint32_t call_height, uint32_t flags) {
    (void)reason; (void)requested_width; (void)requested_height;
    bgfx_reset(call_width, call_height, flags, BGFX_TEXTURE_FORMAT_COUNT);
}
"""
        control = r"""
#define REQUIRE_CONTROL(test_, text_) do { if (!(test_)) { \
    fputs(text_, stderr); return 17; } } while (0)
int main(void) {
    JceRenderer fallback = {true, OTHER_FLAGS};
    JceRenderer real = {false, OTHER_FLAGS};
    jce_renderer_set_vsync(NULL, true);
    REQUIRE_CONTROL(queries==0 && resets==0, "vsync early guard queried stats or reset");
    jce_renderer_set_vsync(&fallback, true);
    jce_renderer_set_vsync_for_size(NULL, true, 0, 0);
    jce_renderer_set_vsync_for_size(&fallback, true, 0, 0);
    REQUIRE_CONTROL(queries==0 && resets==0 && fallback.reset_flags==OTHER_FLAGS,
                    "vsync fallback queried backend or changed flags");
    jce_renderer_set_vsync(&real, false);
    jce_renderer_set_vsync_for_size(&real, false, 0, 0);
    REQUIRE_CONTROL(queries==0 && resets==0, "vsync equal flag queried stats or reset");
    jce_renderer_set_vsync(&real, true);
    REQUIRE_CONTROL(queries==1 && resets==1 && last_width==1643 && last_height==987 &&
                    last_flags==(OTHER_FLAGS|BGFX_RESET_VSYNC) &&
                    last_format==BGFX_TEXTURE_FORMAT_COUNT,
                    "vsync changed wrapper did not reset actual dimensions and flags");
    jce_renderer_set_vsync(&real, true);
    REQUIRE_CONTROL(queries==1 && resets==1, "vsync repeated enable queried stats or reset");
    stats.width=320; stats.height=180;
    jce_renderer_set_vsync(&real, false);
    REQUIRE_CONTROL(queries==2 && resets==2 && last_width==320 && last_height==180 &&
                    last_flags==OTHER_FLAGS && real.reset_flags==OTHER_FLAGS,
                    "vsync disable lost flags or current dimensions");
    jce_renderer_set_vsync_for_size(&real, true, 188, 244);
    REQUIRE_CONTROL(queries==2 && resets==3 && last_width==188 && last_height==244,
                    "vsync explicit size queried backend or lost dimensions");
    stats.width=640; stats.height=480;
    jce_renderer_set_vsync_for_size(&real, false, 123, 0);
    REQUIRE_CONTROL(queries==3 && resets==4 && last_width==640 && last_height==480 &&
                    last_flags==OTHER_FLAGS,
                    "vsync partial zero size failed actual-size fallback");
    jce_renderer_set_vsync_for_size(&real, false, 0, 0);
    REQUIRE_CONTROL(queries==3 && resets==4, "vsync final equal flag queried stats or reset");
    JceRenderer constrained = {false, OTHER_FLAGS};
    constrained.launch_vsync.hidden_d3d12=true;
    jce_renderer_set_vsync(&constrained, true);
    REQUIRE_CONTROL(queries==3 && resets==4 && constrained.reset_flags==OTHER_FLAGS &&
                    constrained.launch_vsync.requested,
                    "vsync actual presentation resolver lost hidden constraint or latest intent");
    constrained.launch_vsync.present=true; constrained.launch_vsync.value=true;
    jce_renderer_set_vsync(&constrained, false);
    REQUIRE_CONTROL(queries==3 && resets==4 && constrained.reset_flags==OTHER_FLAGS &&
                    !constrained.launch_vsync.requested,
                    "vsync forced launch bypassed constraint or overwrote latest request");
    puts("Vsync public wrapper CPU PASS");
    return 0;
}
"""
        return "\n".join((ports, launch_resolve, presentation_resolve,
                          reset_reason, reset_port, sized, wrapper, control))

    def test_vsync_actual_public_wrappers_guard_before_stats_and_reset(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._vsync_cpu_probe())

    def test_vsync_old_unconditional_stats_wrapper_fails_at_runtime(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._vsync_cpu_probe(old_wrapper=True),
                expected_runtime_failure="vsync early guard queried stats or reset")

    def _velocity_instance_cpu_probe(self, missing_program_release=False):
        from tools.tests.test_material_owned_cleanup import function
        source = (ROOT / "engine/src/middleware/scene/jce_sr_cull.c").read_text(encoding="utf-8")
        prepare = function(source, "bool sr_velocity_inst_prepare(")
        release = function(source, "void sr_velocity_inst_release(")
        if missing_program_release:
            marker = "jce_shader_program_destroy((JceShaderHandle){sr->prog_gbuffer_vel_inst.idx});"
            self.assertEqual(release.count(marker), 1)
            release = release.replace(marker, "(void)sr->prog_gbuffer_vel_inst.idx;")
        # The full production functions execute unchanged against counted
        # acquisition/retirement ports. These are CPU ownership witnesses,
        # not compiled shader admission or physical GPU completion.
        ports = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { uint16_t idx; } JceShaderHandle;
typedef struct { unsigned slot; } JceRenderQueue;
typedef struct {
    void *pak;
    JceShaderHandle prog_gbuffer_vel_inst;
    JceRenderQueue *velocity_instance_queue;
    bool gbuffer_vel_inst_prog_tried;
} JceSceneRenderer;
typedef struct { unsigned id; } JceTraceScope;
#define BGFX_HANDLE_IS_VALID(h_) ((h_).idx != UINT16_MAX)
#define JCE_TRACE_TASK_SUCCEEDED 1
#define JCE_TRACE_TASK_FAILED 2
#define LOG_TAG "scene_renderer"
#define LOG_WARN(...) (++warnings)
static unsigned loads, creates, program_destroys, queue_destroys, warnings;
static unsigned scopes_opened, scopes_closed, failed_scopes, bad_ports, next_program;
static bool fail_program, fail_queue, program_live[32], queue_live[32];
static JceRenderQueue queues[32];
static bool jce_shader_valid(JceShaderHandle h) { return h.idx != UINT16_MAX; }
static JceShaderHandle shader_load_program_named(void *pak, const char *vs, const char *fs) {
    ++loads;
    if (!pak || strcmp(vs,"gbuffer_vel_inst") || strcmp(fs,"gbuffer_vel")) ++bad_ports;
    if (fail_program) return (JceShaderHandle){UINT16_MAX};
    if (next_program >= 32) { ++bad_ports; return (JceShaderHandle){UINT16_MAX}; }
    program_live[next_program] = true;
    return (JceShaderHandle){(uint16_t)next_program++};
}
static void jce_shader_program_destroy(JceShaderHandle h) {
    ++program_destroys;
    if (h.idx >= 32 || !program_live[h.idx]) { ++bad_ports; return; }
    program_live[h.idx] = false;
}
static JceRenderQueue *jce_rq_create(unsigned capacity) {
    unsigned slot = creates++;
    if (capacity != 1024 || slot >= 32) { ++bad_ports; return NULL; }
    if (fail_queue) return NULL;
    queues[slot].slot = slot; queue_live[slot] = true;
    return &queues[slot];
}
static void jce_rq_destroy(JceRenderQueue *queue) {
    ++queue_destroys;
    if (!queue || queue->slot >= 32 || !queue_live[queue->slot]) { ++bad_ports; return; }
    queue_live[queue->slot] = false;
}
static JceTraceScope jce_trace_scope_begin(const char *name) {
    if (strcmp(name,"sr.velocity_inst.prepare") && strcmp(name,"sr.velocity_inst.release"))
        ++bad_ports;
    return (JceTraceScope){++scopes_opened};
}
static void jce_trace_scope_end(JceTraceScope scope, int outcome) {
    if (!scope.id || scopes_opened != scopes_closed + 1 ||
        (outcome != JCE_TRACE_TASK_SUCCEEDED && outcome != JCE_TRACE_TASK_FAILED)) ++bad_ports;
    ++scopes_closed;
    if (outcome == JCE_TRACE_TASK_FAILED) ++failed_scopes;
}
"""
        control = r"""
#define REQUIRE_CONTROL(test_, text_) do { if (!(test_)) { \
    fputs(text_, stderr); return 17; } } while (0)
static JceSceneRenderer cold(void) {
    JceSceneRenderer sr = {0}; sr.pak = (void *)&queues;
    sr.prog_gbuffer_vel_inst.idx = UINT16_MAX; return sr;
}
int main(void) {
    JceSceneRenderer a = cold(), b = cold();
    REQUIRE_CONTROL(!sr_velocity_inst_prepare(NULL), "velocity NULL prepare accepted");
    sr_velocity_inst_release(NULL); sr_velocity_inst_release(&a);
    REQUIRE_CONTROL(!loads && !creates && !program_destroys && !queue_destroys &&
                    !scopes_opened && !a.gbuffer_vel_inst_prog_tried,
                    "velocity cold release touched resources");
    REQUIRE_CONTROL(sr_velocity_inst_prepare(&a) && sr_velocity_inst_prepare(&a) &&
                    loads==1 && creates==1 && a.prog_gbuffer_vel_inst.idx==0 &&
                    a.gbuffer_vel_inst_prog_tried,
                    "velocity successful prepare was not lazy or rejected handle zero");
    REQUIRE_CONTROL(sr_velocity_inst_prepare(&b) && loads==2 && creates==2 &&
                    a.velocity_instance_queue!=b.velocity_instance_queue &&
                    a.prog_gbuffer_vel_inst.idx!=b.prog_gbuffer_vel_inst.idx,
                    "velocity two views shared resources");
    uint16_t first = a.prog_gbuffer_vel_inst.idx, other = b.prog_gbuffer_vel_inst.idx;
    sr_velocity_inst_release(&a);
    REQUIRE_CONTROL(!program_live[first] && program_live[other] && program_destroys==1,
                    "velocity program release lost ownership");
    REQUIRE_CONTROL(a.prog_gbuffer_vel_inst.idx==UINT16_MAX &&
                    !a.velocity_instance_queue && !a.gbuffer_vel_inst_prog_tried &&
                    queue_destroys==1 && queue_live[b.velocity_instance_queue->slot],
                    "velocity release affected another owner or kept partial state");
    sr_velocity_inst_release(&a);
    REQUIRE_CONTROL(program_destroys==1 && queue_destroys==1,
                    "velocity duplicate release retired twice");
    sr_velocity_inst_release(&b);
    a = cold();
    REQUIRE_CONTROL(sr_velocity_inst_prepare(&a) && a.prog_gbuffer_vel_inst.idx!=first &&
                    a.prog_gbuffer_vel_inst.idx!=other && loads==3 && creates==3,
                    "velocity recreated session reused retired resource");
    sr_velocity_inst_release(&a);
    a = cold(); fail_program = true;
    REQUIRE_CONTROL(!sr_velocity_inst_prepare(&a) && !sr_velocity_inst_prepare(&a) &&
                    loads==4 && creates==3 && warnings==1 &&
                    a.prog_gbuffer_vel_inst.idx==UINT16_MAX && !a.velocity_instance_queue &&
                    a.gbuffer_vel_inst_prog_tried,
                    "velocity missing shader republished or retried every frame");
    sr_velocity_inst_release(&a); fail_program = false; fail_queue = true;
    REQUIRE_CONTROL(!sr_velocity_inst_prepare(&a) && !sr_velocity_inst_prepare(&a) &&
                    loads==5 && creates==4 && program_destroys==4 && warnings==2 &&
                    a.prog_gbuffer_vel_inst.idx==UINT16_MAX && !a.velocity_instance_queue,
                    "velocity queue allocation failure retained program or published partial state");
    sr_velocity_inst_release(&a); sr_velocity_inst_release(&a); fail_queue = false;
    REQUIRE_CONTROL(program_destroys==4 && queue_destroys==3,
                    "velocity failed prepare release retired twice");
    a.prog_gbuffer_vel_inst = shader_load_program_named(a.pak,"gbuffer_vel_inst","gbuffer_vel");
    a.gbuffer_vel_inst_prog_tried = true;
    sr_velocity_inst_release(&a); sr_velocity_inst_release(&a);
    REQUIRE_CONTROL(program_destroys==5 && !a.gbuffer_vel_inst_prog_tried &&
                    a.prog_gbuffer_vel_inst.idx==UINT16_MAX,
                    "velocity partial program owner was not released");
    a.velocity_instance_queue = jce_rq_create(1024); a.gbuffer_vel_inst_prog_tried = true;
    sr_velocity_inst_release(&a); sr_velocity_inst_release(&a);
    REQUIRE_CONTROL(queue_destroys==4 && !a.velocity_instance_queue &&
                    !a.gbuffer_vel_inst_prog_tried,
                    "velocity partial queue owner was not released");
    for (unsigned i=0; i<32; ++i)
        REQUIRE_CONTROL(!program_live[i] && !queue_live[i], "velocity residual owned resources");
    REQUIRE_CONTROL(scopes_opened==scopes_closed && failed_scopes==2 && !bad_ports,
                    "velocity lifecycle trace or resource port contract failed");
    puts("Scene velocity instance actual-owner CPU PASS"); return 0;
}
"""
        return "\n".join((ports, prepare, release, control))

    def test_scene_velocity_instance_actual_owner_lifecycle(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._velocity_instance_cpu_probe())

    def test_scene_velocity_instance_missing_release_fails_at_runtime(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._velocity_instance_cpu_probe(missing_program_release=True),
                expected_runtime_failure="velocity program release lost ownership")


    def _depth_queue_cpu_probe(self, missing_destroy_wire=False):
        from tools.tests.test_material_owned_cleanup import function
        cull = (ROOT / "engine/src/middleware/scene/jce_sr_cull.c").read_text(encoding="utf-8")
        owner = (ROOT / "engine/src/middleware/scene/jce_scene_renderer.c").read_text(encoding="utf-8")
        prepare = function(cull, "bool sr_depth_prepass_queue_prepare(")
        release = function(cull, "void sr_depth_prepass_queue_release(")
        draw = function(cull, "void sr_draw_depth_prepass(")
        admission = draw[draw.index("    JceShaderHandle pre_inst_sh ="):draw.index("    /* TAA velocity mode")]
        destructor = function(owner, "void jce_scene_renderer_destroy(")
        wire = "sr_depth_prepass_queue_release(sr);"
        self.assertEqual(destructor.count(wire), 1)
        # Execute the changed real draw admission and real destructor wire, not
        # a parallel renderer. Counted ports use distinct actual heap objects;
        # the native Noop fixture separately executes the real queue allocator.
        destroy_wire = "(void)sr;" if missing_destroy_wire else wire
        ports = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint16_t idx; } JceShaderHandle;
typedef struct { unsigned count; void *entries; void *binder; void *user; } JceRenderQueue;
typedef struct { void *renderer; JceRenderQueue *depth_prepass_queue; } JceSceneRenderer;
/* Counted diagnostic ports only; this probe does not claim ring publication. */
typedef struct { unsigned id; } JceTraceScope;
#define JCE_TRACE_TASK_SUCCEEDED 1
#define JCE_TRACE_TASK_FAILED 2
static unsigned creates, destroys, live, opened, closed, failed, bad_ports;
static int s_pre_batch_env=1;
static bool fail_queue;
static JceShaderHandle inst_program={0};
static JceShaderHandle jce_renderer_get_program_shadow_inst(void *r) {
    if (!r) ++bad_ports;
    return inst_program;
}
static JceRenderQueue *jce_rq_create(unsigned capacity) {
    ++creates;
    if (capacity!=1024) ++bad_ports;
    if (fail_queue) return NULL;
    JceRenderQueue *queue=(JceRenderQueue *)calloc(1,sizeof(*queue));
    if (!queue) return NULL;
    queue->entries=calloc(capacity,sizeof(unsigned));
    if (!queue->entries) { free(queue); return NULL; }
    ++live; return queue;
}
static void jce_rq_set_material_binder(JceRenderQueue *q,void *binder,void *user) {
    if (!q || binder || user) ++bad_ports;
    q->binder=binder; q->user=user;
}
static void jce_rq_clear(JceRenderQueue *q) { q->count=0; }
static void jce_rq_destroy(JceRenderQueue *q) {
    if (!q || !live) { ++bad_ports; return; }
    --live; ++destroys; free(q->entries); free(q);
}
static JceTraceScope jce_trace_scope_begin(const char *name) {
    if (strcmp(name,"sr.depth_queue.prepare") && strcmp(name,"sr.depth_queue.release")) ++bad_ports;
    return (JceTraceScope){++opened};
}
static void jce_trace_scope_end(JceTraceScope scope,int result) {
    if (!scope.id || opened!=closed+1 ||
        (result!=JCE_TRACE_TASK_SUCCEEDED && result!=JCE_TRACE_TASK_FAILED)) ++bad_ports;
    ++closed; if (result==JCE_TRACE_TASK_FAILED) ++failed;
}
#define REQUIRE_CONTROL(test_,text_) do { if (!(test_)) { fputs(text_,stderr); return 17; } } while(0)
"""
        controls = r"""
int main(void) {
    JceSceneRenderer a={0},b={0}; a.renderer=&creates; b.renderer=&destroys;
    REQUIRE_CONTROL(!sr_depth_prepass_queue_prepare(NULL), "depth NULL prepare accepted");
    sr_depth_prepass_queue_release(NULL); destroy_queue_wire(&a);
    REQUIRE_CONTROL(!creates&&!destroys&&!opened, "depth cold release touched resources");
    REQUIRE_CONTROL(!admit(&a,true)&&!creates, "depth normals path allocated queue");
    inst_program.idx=UINT16_MAX;
    REQUIRE_CONTROL(!admit(&a,false)&&!creates, "depth invalid shader allocated queue");
    inst_program.idx=0; s_pre_batch_env=0;
    REQUIRE_CONTROL(!admit(&a,false)&&!creates, "depth disabled path allocated queue");
    s_pre_batch_env=1; fail_queue=true;
    REQUIRE_CONTROL(!admit(&a,false)&&creates==1&&!a.depth_prepass_queue&&failed==1,
                    "depth allocation failure did not keep inline fallback");
    fail_queue=false;
    REQUIRE_CONTROL(admit(&a,false)&&admit(&b,false)&&live==2&&creates==3&&
                    a.depth_prepass_queue!=b.depth_prepass_queue,
                    "depth queue retry or per-owner isolation failed");
    JceRenderQueue *saved=a.depth_prepass_queue;
    a.depth_prepass_queue->count=7; a.depth_prepass_queue->binder=&creates;
    b.depth_prepass_queue->count=9;
    REQUIRE_CONTROL(admit(&a,false)&&creates==3&&a.depth_prepass_queue==saved&&
                    !a.depth_prepass_queue->count&&!a.depth_prepass_queue->binder&&
                    b.depth_prepass_queue->count==9,
                    "depth queue reuse retained commands or disturbed another owner");
    destroy_queue_wire(&a);
    REQUIRE_CONTROL(!a.depth_prepass_queue&&live==1&&destroys==1&&b.depth_prepass_queue->count==9,
                    "depth queue lifetime not released");
    destroy_queue_wire(&a); destroy_queue_wire(&b);
    REQUIRE_CONTROL(!live&&destroys==2, "depth duplicate release retained or double freed queue");
    REQUIRE_CONTROL(admit(&a,false)&&creates==4&&live==1&&!a.depth_prepass_queue->count,
                    "depth recreated owner retained commands");
    destroy_queue_wire(&a);
    REQUIRE_CONTROL(!live&&destroys==3&&opened==closed&&failed==1&&!bad_ports,
                    "depth queue final CPU ownership or scope imbalance");
    puts("Scene depth queue actual-owner CPU PASS"); return 0;
}
"""
        return "\n".join((ports, prepare, release,
            "static bool admit(JceSceneRenderer *sr,bool want_normals){\n"+admission+"return pre_batch;\n}",
            "static void destroy_queue_wire(JceSceneRenderer *sr){\n"+destroy_wire+"\n}", controls))

    def test_scene_depth_queue_actual_owner_and_draw_admission(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._depth_queue_cpu_probe())

    def test_scene_depth_queue_missing_destroy_wire_fails_at_runtime(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._depth_queue_cpu_probe(missing_destroy_wire=True),
                expected_runtime_failure="depth queue lifetime not released")

    def _scene_constructor_cpu_probe(self, missing_light_release=False):
        from tools.tests.test_material_owned_cleanup import function
        text = (ROOT / "engine/src/middleware/scene/jce_scene_renderer.c").read_text(encoding="utf-8")
        create = function(text, "JceSceneRenderer *jce_scene_renderer_create(")
        end = create.index("    sr->ecull_cap = 1024;")+len("    sr->ecull_cap = 1024;")
        prefix = create[:end]+"\n    return sr;\n}"
        if missing_light_release:
            marker = "JCE_FREE(sr->ecull_light_byte);"
            self.assertEqual(prefix.count(marker), 1)
            prefix = prefix.replace(marker, "(void)sr->ecull_light_byte;")
        # Actual constructor prefix through its initial CPU buffers. Minimal
        # private storage provides only accessed fields; this is failure unwind,
        # not the full constructor, GPU admission or real SrEntityCull sizing.
        ports = r"""
#include <jce/renderer/jce_scene_renderer.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned fixture_field; } SrEntityCull;
struct JceSceneRenderer {
    struct { uint16_t idx; } prog_gbuffer_vel_inst;
    JceRenderer *renderer; const JcePakArchive *pak;
    unsigned nodraw_registry_count;
    JceSceneRendererCallbacks cbs; bool has_cbs;
    bool model_bounds_enabled; /* Constructor writes this independent feature latch. */
    SrEntityCull *ecull; jce_mat4 *ecull_world;
    uint8_t *ecull_light_byte,*ecull_fast_byte; unsigned ecull_cap;
};
static unsigned attempts,fail_at,live_blocks,bad_free;
static size_t live_bytes;
static struct { void *ptr; size_t bytes; } allocations[16];
static unsigned jce_component_count(void) { return 9; }
static void *counted_calloc(size_t count,size_t size) {
    ++attempts;
    if (attempts==fail_at) return NULL;
    void *ptr=calloc(count,size);
    if (!ptr) return NULL;
    for (unsigned i=0;i<16;++i) if (!allocations[i].ptr) {
        allocations[i].ptr=ptr; allocations[i].bytes=count*size;
        ++live_blocks; live_bytes+=count*size; return ptr;
    }
    free(ptr); return NULL;
}
static void counted_free(void *ptr) {
    if (!ptr) return;
    for (unsigned i=0;i<16;++i) if (allocations[i].ptr==ptr) {
        live_bytes-=allocations[i].bytes; --live_blocks;
        allocations[i].ptr=NULL; free(ptr); return;
    }
    ++bad_free;
}
#define JCE_CALLOC(count_,size_) counted_calloc(count_,size_)
#define JCE_FREE(ptr_) counted_free(ptr_)
#define REQUIRE_CONTROL(test_,text_) do { if (!(test_)) { fputs(text_,stderr); return 17; } } while(0)
"""
        controls = r"""
int main(void) {
    REQUIRE_CONTROL(!jce_scene_renderer_create(NULL,NULL,NULL)&&!attempts,
                    "scene NULL renderer allocated CPU buffers");
    for (unsigned failure=1;failure<=5;++failure) {
        attempts=0; fail_at=failure;
        JceSceneRenderer *sr=jce_scene_renderer_create((JceRenderer *)&attempts,NULL,NULL);
        REQUIRE_CONTROL(!sr, "scene CPU allocation failure returned partial renderer");
        REQUIRE_CONTROL(!live_blocks&&!live_bytes&&!bad_free,
                        "scene renderer failure left live CPU bytes");
    }
    attempts=0; fail_at=0;
    JceSceneRendererCallbacks cbs={0};
    JceSceneRenderer *sr=jce_scene_renderer_create((JceRenderer *)&attempts,NULL,&cbs);
    REQUIRE_CONTROL(sr&&attempts==5&&live_blocks==5&&sr->ecull_cap==1024&&
                    sr->nodraw_registry_count==9&&sr->has_cbs&&
                    sr->prog_gbuffer_vel_inst.idx==UINT16_MAX,
                    "scene CPU constructor prefix changed successful initialization");
    /* This prefix never reaches GPU resources; release only its acquired CPU
     * buffers here, separate from the real native public destructor control. */
    counted_free(sr->ecull_fast_byte); counted_free(sr->ecull_light_byte);
    counted_free(sr->ecull_world); counted_free(sr->ecull); counted_free(sr);
    REQUIRE_CONTROL(!live_blocks&&!live_bytes&&!bad_free,
                    "scene constructor prefix fixture did not close CPU ownership");
    puts("Scene constructor actual-prefix CPU PASS"); return 0;
}
"""
        return "\n".join((ports,prefix,controls))

    def test_scene_constructor_actual_cpu_allocation_failure_matrix(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._scene_constructor_cpu_probe())

    def test_scene_constructor_missing_light_release_fails_at_runtime(self):
        from tools.tests.test_material_owned_cleanup import execute
        execute(self, self._scene_constructor_cpu_probe(missing_light_release=True),
                expected_runtime_failure="scene renderer failure left live CPU bytes")

if __name__ == "__main__":
    unittest.main()
