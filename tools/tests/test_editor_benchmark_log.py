"""Actual Editor workload admission node survives DIST with ordinary negative control."""
from pathlib import Path
import sys
import unittest
ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path: sys.path.insert(0, str(ROOT))
from tools.tests.test_host_font_selection import execute_stock
from tools.tests.test_canvas_transparent_draw import production_function

PORTS = r"""
#define JCE_DIST 1
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/middleware/scene/jce_scene_stress.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#define LOG_TAG "editor_state"
static unsigned calls=0, spawned=256, rebuilt=0, motions=0;
static bool permit=true, generate=true;
static char message[160];
static unsigned s_bench_root=0, s_bench_spawned=0;
static int s_bench_kind=-1;
struct State { JceScene *scene; } s = {reinterpret_cast<JceScene*>(1)};
struct HistorySuspendScope {};
static void jce_state_benchmark_clear() { s_bench_root=s_bench_spawned=0; }
static void rebuild_entity_order_from_ecs() { ++rebuilt; }
extern "C" bool jce_scene_stress_spawn(JceScene*, const JceSceneStressDesc*, JceSceneStressResult *r) {
    if (!generate) return false; r->root=9; r->spawned=spawned; return true;
}
extern "C" void jce_scene_stress_motion(JceScene*, JceEntity, bool) { ++motions; }
extern "C" void jce_perf_phase_set_enabled(int) {}
extern "C" void JCE_CALL jce_log_write(JceLogLevel level,const char *tag,const char *file,int line,const char *fmt,...) {
    if (!permit) return;
    if (level!=JCE_LOG_LEVEL_INFO || std::strcmp(tag,LOG_TAG) || !file || line<=0) { calls=10000; return; }
    ++calls; va_list args;va_start(args,fmt);std::vsnprintf(message,sizeof message,fmt,args);va_end(args);
}
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr,"benchmark log failed %d: %s\n",__LINE__,#value); return 4; } } while(false)
"""
MAIN = r"""
int main() {
    LOG_INFO("internal","DIST internal chatter remains removed");REQUIRE(calls==0);
    jce_state_benchmark_spawn(0,256);
    REQUIRE(calls==1 && std::strcmp(message,"benchmark-spawn: kind=0 requested=256 spawned=256")==0);
    REQUIRE(rebuilt==1 && s_bench_spawned==256);
    spawned=128;jce_state_benchmark_spawn(0,256);
    REQUIRE(calls==2 && std::strstr(message,"requested=256 spawned=128"));
    generate=false;jce_state_benchmark_spawn(0,256);
    REQUIRE(calls==3 && std::strstr(message,"requested=256 spawned=0"));
    generate=true;spawned=64;jce_state_benchmark_spawn(3,64);REQUIRE(calls==4 && motions==1);
    jce_state_benchmark_spawn(0,0);REQUIRE(calls==4);
    s.scene=nullptr;jce_state_benchmark_spawn(0,256);REQUIRE(calls==4);
    s.scene=reinterpret_cast<JceScene*>(1);permit=false;jce_state_benchmark_spawn(0,256);REQUIRE(calls==4);
    return 0;
}
"""

def owner():
    return production_function((ROOT/'editor/src/core/jce_editor_state.cpp').read_text(encoding='utf-8'), 'void jce_state_benchmark_spawn(')

class EditorBenchmarkLogTests(unittest.TestCase):
    def test_actual_dist_spawn_reports_actual_count_and_runtime_filter(self):
        execute_stock(self, PORTS+owner()+MAIN)
    def test_old_macro_loss_is_ordinary_runtime_failure(self):
        body=owner()
        current='jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,'
        self.assertEqual(body.count(current),1)
        old=body.replace(current,'LOG_INFO(LOG_TAG,')
        execute_stock(self,PORTS+old+MAIN,negative=True,negative_exit=4,negative_marker='benchmark log failed ')

if __name__=='__main__': unittest.main()
