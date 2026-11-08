"""Exact production failure blocks and cleanup helpers, with ownership ports.

Acquisition results are injected; this does not certify actual GPU allocation,
complete init, renderer shutdown, or a real low-memory device. The registration
refusal block consumes a counted shutdown-call boundary; complete rollback and
the real Lifecycle registry are covered by the Editor native TSR fixture.
"""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OWNER = ROOT / "editor/src/scene/jce_editor_scene_render.cpp"


def block(source, marker):
    if source.count(marker) != 1:
        raise AssertionError("Production block marker must be unique: " + marker)
    start = source.index(marker)
    brace, depth = source.index("{", start), 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if not depth:
                return source[brace + 1:end]
    raise AssertionError("Production block no longer has a complete body")


class SceneCanvasUnwindControls(unittest.TestCase):
    def test_owned_partial_failures_and_removed_unwind_or_sentinel_negative(self):
        source = OWNER.read_text(encoding="utf-8")
        self.assertEqual(source.count("bool jce_editor_scene_render_init("), 1)
        self.assertEqual(source.count("void jce_editor_scene_render_shutdown("), 1)
        init = source[source.index("bool jce_editor_scene_render_init("):
                      source.index("void jce_editor_scene_render_shutdown(")]
        helpers = "\n".join("static void %s(void){%s}" % (name, block(
            source, "static void %s(void)" % name)) for name in (
                "sr_destroy_ui_canvas", "sr_destroy_view_resources", "sr_cleanup_failed_init"))
        names = ("bridge", "camera", "scene_renderer")
        exits = [block(init, "if (!s_sr.%s) {" % name) for name in names]
        registration_exit = block(init, "if (!sr_temporal_lifecycle_begin()) {")
        fatal_exits = dict(zip(names, exits))
        fatal_exits["lifecycle_registration"] = registration_exit
        for name, body in fatal_exits.items():
            self.assertEqual(body.count("return false;"), 1,
                             name + " must retain its one covered fatal exit")
        self.assertEqual(init.count("return false;"),
                         sum(body.count("return false;") for body in fatal_exits.values()),
                         "Every fatal init exit must have an actual-owner runtime control")
        shutdown_wire = "jce_editor_scene_render_shutdown();"
        self.assertEqual(registration_exit.count(shutdown_wire), 1,
                         "Registration refusal must dispatch complete existing shutdown exactly once")
        prefix = init[init.index("    memset(&s_sr, 0, sizeof(s_sr));"):
                      init.index("    /* ECS-UI Canvas overlay")]
        spec = importlib.util.spec_from_file_location("scene_unwind_driver",
                                                      ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST == "windows":
            env = driver.msvc_env(driver.resolve_target(None))
            compiler = ["cl", "/nologo", "/TP", "/std:c++17", "/EHsc"]
        else:
            cxx = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
            if not cxx:
                self.skipTest("No C++ compiler for exact failure-owner controls")
            env, compiler = None, [cxx, "-std=c++17"]
        prelude = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct Object { int id; };
typedef Object JceUICanvas;
typedef Object JceRenderer;
typedef Object JceAssetManager;
struct Handle { uint16_t idx; };
static struct {
 bool initialized; JceRenderer *renderer; JceUICanvas *ui_canvas;
 Object *bridge,*present,*camera;
 Handle white_tex,prog_grid,prog_color,u_grid_camera,u_grid_fade,u_light_dir,u_light_color;
 uint16_t postfx_output_tex;
} s_sr;
static struct {
 unsigned canvas_closed,target_closed,camera_closed,gpu_closed,cache_closed,cache_calls,invalid;
 unsigned owned_gpu;
 bool canvas_live,bridge_live,present_live,camera_live,cache_live;
} f;
static Object canvas_a={1},canvas_b={5},bridge={2},present={3},camera={4};
static Object borrowed_renderer={100},borrowed_assets={101};
static bool jce_gfx_texture_valid(Handle h){return h.idx!=UINT16_MAX;}
static bool jce_program_valid(Handle h){return h.idx!=UINT16_MAX;}
static bool jce_uniform_valid(Handle h){return h.idx!=UINT16_MAX;}
static void jce_ui_canvas_destroy(Object *p) {
 if(!f.canvas_live || (p!=&canvas_a && p!=&canvas_b))f.invalid++;
 f.canvas_live=false;f.canvas_closed++;
}
static void jce_offscreen_target_destroy(Object *p) {
 bool *live=p==&bridge?&f.bridge_live:p==&present?&f.present_live:NULL;
 if(!live || !*live)f.invalid++;else *live=false;
 f.target_closed++;
}
static void jce_camera_destroy(Object *p) {
 if(p!=&camera || !f.camera_live)f.invalid++;
 f.camera_live=false;f.camera_closed++;
}
static void close_gpu(Handle h,unsigned bit,uint16_t id) {
 if(h.idx!=id || !(f.owned_gpu&bit))f.invalid++;
 f.owned_gpu&=~bit;f.gpu_closed++;
}
static void jce_gfx_texture_destroy(Handle h){close_gpu(h,1u,0);}
static void jce_program_destroy(Handle h){close_gpu(h,2u,0);}
static void jce_uniform_destroy(Handle h) {
 unsigned bit=h.idx>=1 && h.idx<=4?1u<<(h.idx+1):0;
 close_gpu(h,bit,h.idx);
}
static void jce_editor_scene_asset_cache_init(Object *assets) {
 if(assets!=&borrowed_assets || f.cache_live)f.invalid++;
 f.cache_live=true;
}
static void jce_editor_scene_asset_cache_shutdown(void) {
 f.cache_calls++;if(f.cache_live)f.cache_closed++;
 f.cache_live=false;
}
#define LOG_TAG "probe"
#define LOG_WARN(...) ((void)0)
'''
        main = r'''
static unsigned bit_count(unsigned n){unsigned count=0;while(n){count+=n&1u;n>>=1;}return count;}
static bool all_gpu_invalid(void) {
 return s_sr.white_tex.idx==UINT16_MAX && s_sr.prog_grid.idx==UINT16_MAX &&
 s_sr.u_grid_camera.idx==UINT16_MAX && s_sr.u_grid_fade.idx==UINT16_MAX &&
 s_sr.u_light_dir.idx==UINT16_MAX && s_sr.u_light_color.idx==UINT16_MAX;
}
static int run_control(int stage,bool canvas_ok,bool present_ok,unsigned mask) {
 memset(&f,0,sizeof f);begin_init();
 // prog_color belongs to the renderer, even when its valid index is zero.
 s_sr.prog_color.idx=0;s_sr.ui_canvas=canvas_ok?&canvas_a:NULL;f.canvas_live=canvas_ok;
 if(stage>=1) {
  s_sr.bridge=&bridge;f.bridge_live=true;
  s_sr.present=present_ok?&present:NULL;f.present_live=present_ok;
 }
 if(stage==2) {
  s_sr.camera=&camera;f.camera_live=true;f.owned_gpu=mask;
  if(mask&1u)s_sr.white_tex.idx=0;
  if(mask&2u)s_sr.prog_grid.idx=0;
  if(mask&4u)s_sr.u_grid_camera.idx=1;
  if(mask&8u)s_sr.u_grid_fade.idx=2;
  if(mask&16u)s_sr.u_light_dir.idx=3;
  if(mask&32u)s_sr.u_light_color.idx=4;
 }
 bool result=stage==0?failed_bridge():stage==1?failed_camera():failed_scene_renderer();
 unsigned expected_targets=stage>=1?(present_ok?2u:1u):0u;
 if(result || f.invalid || f.canvas_live || f.bridge_live || f.present_live ||
 f.camera_live || f.owned_gpu || f.cache_live || s_sr.ui_canvas || s_sr.camera ||
 s_sr.bridge || s_sr.present || !all_gpu_invalid() ||
 f.canvas_closed!=(canvas_ok?1u:0u) || f.target_closed!=expected_targets ||
 f.camera_closed!=(stage==2?1u:0u) || f.gpu_closed!=bit_count(mask) ||
 f.cache_closed!=1 || f.cache_calls!=1 || s_sr.renderer!=&borrowed_renderer ||
 s_sr.prog_color.idx!=0)return 40+stage;
 sr_cleanup_failed_init();
 if(f.invalid || f.canvas_closed!=(canvas_ok?1u:0u) ||
 f.target_closed!=expected_targets || f.camera_closed!=(stage==2?1u:0u) ||
 f.gpu_closed!=bit_count(mask) || f.cache_closed!=1)return 50+stage;
 // Retry obtains a distinct Canvas after the previous owner is fully released.
 begin_init();s_sr.ui_canvas=&canvas_b;f.canvas_live=true;
 if(failed_bridge() || f.invalid || s_sr.ui_canvas || f.canvas_live ||
 f.canvas_closed!=(canvas_ok?2u:1u) || f.cache_closed!=2)return 60+stage;
 return 0;
}
int main(void) {
 const unsigned masks[]={0u,63u,1u,2u,4u,8u,16u,32u};
 for(int stage=0;stage<3;stage++)for(int canvas=0;canvas<2;canvas++)
 for(int present_ok=0;present_ok<2;present_ok++)
 for(unsigned i=0;i<(stage==2?8u:1u);i++) {
  int code=run_control(stage,canvas!=0,present_ok!=0,masks[i]);if(code)return code;
 }
 puts("Scene partial failure ownership PASS");return 0;
}
'''
        output = ROOT / "build/tests/editor-scene-canvas-unwind"
        output.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            for mutation in ("actual", "missing_unwind", "missing_canvas_release", "missing_sentinel"):
                # A completed Windows child can still have observers holding its
                # executable open. Each control owns distinct link output; never
                # overwrite a just-executed mutation image or retry a failed link.
                directory = Path(temporary) / mutation
                directory.mkdir()
                bodies, init_prefix, owner_helpers = exits[:], prefix, helpers
                if mutation == "missing_unwind":
                    self.assertIn("sr_cleanup_failed_init();", bodies[0])
                    bodies[0] = bodies[0].replace("sr_cleanup_failed_init();", "", 1)
                elif mutation == "missing_canvas_release":
                    self.assertIn("sr_destroy_ui_canvas();", owner_helpers)
                    owner_helpers = owner_helpers.replace("sr_destroy_ui_canvas();", "", 1)
                elif mutation == "missing_sentinel":
                    self.assertIn("s_sr.u_light_dir.idx = UINT16_MAX;", init_prefix)
                    init_prefix = init_prefix.replace("s_sr.u_light_dir.idx = UINT16_MAX;", "", 1)
                functions = "\n".join("static bool failed_%s(){%s}" % (name, body)
                                       for name, body in zip(names, bodies))
                acquire = "static void begin_init(){JceRenderer *renderer=&borrowed_renderer;" \
                          "JceAssetManager *assets=&borrowed_assets;" + init_prefix + "}\n"
                file = directory / "probe.cpp"
                file.write_text(prelude + owner_helpers + acquire + functions + main, encoding="utf-8")
                executable = directory / ("probe.exe" if driver.HOST == "windows" else "probe")
                command = compiler + ([str(file), "/Fe:probe.exe", "/Fo:probe.obj"]
                    if driver.HOST == "windows" else [str(file), "-o", str(executable)])
                result = subprocess.run(command, cwd=directory, env=env,
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], cwd=directory,
                                        capture_output=True, text=True, timeout=10)
                print("scene-canvas-unwind control=%s compile_rc=0 runtime_rc=%d" %
                      (mutation, result.returncode))
                if mutation == "actual":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("Scene partial failure ownership PASS", result.stdout)
                else:
                    expected = {"missing_unwind": 40, "missing_canvas_release": 60,
                                "missing_sentinel": 40}[mutation]
                    self.assertEqual(result.returncode, expected,
                        mutation + " must reach its ordinary ownership refusal; "
                        "a crash, timeout or missing executable is not a negative-control pass")

            # This is the exact new fatal block, not a second rollback model.
            # Native TSR tests own actual full shutdown and registry cleanup.
            registration_port = r'''
static unsigned shutdown_calls,error_logs;
static void jce_editor_scene_render_shutdown(void){++shutdown_calls;}
#define LOG_ERROR(...) (++error_logs)
'''
            registration_main = r'''
int main(void) {
 f.cache_calls=17;
 if(failed_registration() || shutdown_calls!=1 || error_logs!=1 ||
    f.cache_calls!=17)return 70;
 puts("Scene registration failure shutdown dispatch PASS");return 0;
}
'''
            for mutation in ("actual_registration", "missing_shutdown"):
                directory = Path(temporary) / mutation
                directory.mkdir()
                body = registration_exit
                if mutation == "missing_shutdown":
                    body = body.replace(shutdown_wire, "", 1)
                file = directory / "probe.cpp"
                file.write_text(prelude + registration_port +
                    "static bool failed_registration(){" + body + "}\n" +
                    registration_main, encoding="utf-8")
                executable = directory / ("probe.exe" if driver.HOST == "windows" else "probe")
                command = compiler + ([str(file), "/Fe:probe.exe", "/Fo:probe.obj"]
                    if driver.HOST == "windows" else [str(file), "-o", str(executable)])
                result = subprocess.run(command, cwd=directory, env=env,
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable)], cwd=directory,
                                        capture_output=True, text=True, timeout=10)
                print("scene-canvas-unwind control=%s compile_rc=0 runtime_rc=%d" %
                      (mutation, result.returncode))
                self.assertEqual(result.returncode, 0 if mutation == "actual_registration" else 70,
                    mutation + " must complete ordinary shutdown-dispatch assertions; "
                    "a crash, timeout or compiler failure is not negative evidence")
                if mutation == "actual_registration":
                    self.assertIn("Scene registration failure shutdown dispatch PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
