"""Actual shadow admission owners and entry guards with counted CPU ports.

No backend, pixels or physical memory improvement is certified by these probes.
The native shadow-target fixture separately observes actual Noop owner counts.
"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]

def load(file, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

SOURCE = load("test_canvas_transparent_draw.py", "shadow_demand_source")
RUNNER = load("test_material_owned_cleanup.py", "shadow_demand_runner")

class NegativeRuntime:
    def __init__(self, test):
        self.test, self.phase = test, 0
    def assertEqual(self, actual, expected, message):
        self.phase += 1
        if self.phase == 1:
            self.test.assertEqual(actual, expected, message)
        else:
            self.test.assertNotEqual(actual, expected, message)
    def skipTest(self, message):
        self.test.skipTest(message)


def directional_probe(negative=False):
    text = (ROOT / "engine/src/middleware/scene/jce_sr_shadow.c").read_text(encoding="utf-8")
    helper = SOURCE.production_function(text, "static bool sr_admit_directional_shadow(")
    if negative:
        marker = '    if (!sr_resolve_primary_dir_light('
        assert helper.count(marker) == 1
        helper = helper.replace(marker, '    if (use_csm) sr_ensure_csm_count(sr);\n' + marker, 1)
    # Compile the real entry up to admission, with a counted marker after it.
    # Shader/capability/math submission after this boundary is out of scope.
    begin = text.index("void sr_draw_shadow_pass(")
    end = text.index("    /* Capability-matrix U3:", begin)
    entry = text[begin:end] + "    admitted++;\n}\n"
    return DIR_PORTS + helper + "\n" + entry + DIR_CONTROL


def dynamic_probe(negative=None):
    text = (ROOT / "engine/src/middleware/scene/jce_sr_shadow_targets.c").read_text(encoding="utf-8")
    owner = "\n".join(SOURCE.production_function(text, signature) for signature in (
        "void sr_shadow_depth_target_destroy(", "bool sr_shadow_depth_target_ensure(",
        "SrShadowTargetAdmission sr_shadow_dynamic_target_admit(", "bool sr_ensure_dyn_csm_atlas("))
    if negative == "need":
        old = "|| dynamic_count == 0"
        assert owner.count(old) == 1
        owner = owner.replace(old, "|| false", 1)
    shadow = (ROOT / "engine/src/middleware/scene/jce_sr_shadow.c").read_text(encoding="utf-8")
    begin = shadow.index("    const bool dual_on = sr_ensure_dyn_csm_atlas(")
    gate = shadow[begin:shadow.index(";", begin)+1]
    if negative == "gate":
        gate = "    const bool dual_on = sr->dyn_csm_valid;"
    return DYNAMIC_PORTS + owner + "\nstatic bool actual_gate(JceSceneRenderer *sr){\n" + gate + "\nreturn dual_on;}\n" + DYNAMIC_CONTROL


DIR_PORTS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(v) do{if(!(v)){fprintf(stderr,"shadow directional %d: %s\n",__LINE__,#v);return 3;}}while(0)
typedef struct {float x,y,z;} jce_vec3;
typedef struct {uint16_t idx;} JceShaderHandle;
typedef struct {int unused;} JceScene;
typedef struct {int unused;} EntityList;
typedef struct {int unused;} JceCamera;
typedef struct {bool shadow_valid,shadow_use_csm,last_csm_valid,frame_shadow_active,frame_shadow_vp_valid,gpu_shadow_planes_valid;void *renderer,*render_queue;unsigned csm_cascade_count;} JceSceneRenderer;
static bool light,feature=true,ready=true;static unsigned allocations,resets,admitted;
static void sr_bindpose_shadow_reset(JceSceneRenderer *sr){(void)sr;resets++;}
static JceShaderHandle jce_renderer_get_program_shadow(void *r){(void)r;return (JceShaderHandle){1};}
static JceShaderHandle jce_renderer_get_program_shadow_inst(void *r){(void)r;return (JceShaderHandle){UINT16_MAX};}
static bool jce_render_pipeline_is_feature_enabled(const char *name){(void)name;return feature;}
static bool sr_resolve_primary_dir_light(JceSceneRenderer *sr,JceScene *scene,EntityList *list,bool shadow,jce_vec3 *dir,void *a,void *b){(void)sr;(void)scene;(void)list;(void)shadow;(void)dir;(void)a;(void)b;return light;}
static bool sr_ensure_csm_count(JceSceneRenderer *sr){(void)sr;allocations++;return ready;}
'''
DIR_CONTROL = r'''
int main(void){
    JceSceneRenderer sr={0};sr.shadow_valid=true;sr.csm_cascade_count=4;
    sr_draw_shadow_pass(&sr,NULL,NULL,NULL,3,16,16,100,0.5f);
    REQUIRE(!allocations&&!admitted&&resets==1);
    light=true;feature=false;
    sr_draw_shadow_pass(&sr,NULL,NULL,NULL,3,16,16,100,0.5f);
    REQUIRE(!allocations&&admitted==1);
    feature=true;ready=false;
    sr_draw_shadow_pass(&sr,NULL,NULL,NULL,3,16,16,100,0.5f);
    REQUIRE(allocations==1&&admitted==1);
    ready=true;
    sr_draw_shadow_pass(&sr,NULL,NULL,NULL,3,16,16,100,0.5f);
    REQUIRE(allocations==2&&admitted==2);
    sr.shadow_valid=false;
    sr_draw_shadow_pass(&sr,NULL,NULL,NULL,3,16,16,100,0.5f);
    REQUIRE(allocations==2&&admitted==2);
    return 0;
}
'''
DYNAMIC_PORTS = r'''
#include "middleware/scene/jce_sr_shadow_targets.h"
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(v) do{if(!(v)){fprintf(stderr,"shadow dynamic %d: %s\n",__LINE__,#v);return 3;}}while(0)
typedef struct {uint16_t idx;} Handle;
typedef struct {unsigned shadow_dyn_count;Handle dyn_csm_atlas_tex,dyn_csm_atlas_fbo;uint16_t shadow_map_size,shadow_depth_fmt,dyn_csm_atlas_size;bool dyn_csm_valid;} JceSceneRenderer;
static unsigned creates,fb_creates,frees,fb_frees;static bool fail_texture,fail_framebuffer;
static bool dual=true,percascade=true;static bool live[16];static unsigned next_handle;
static bool sr_shadow_dual_on(void){return dual;}
static bool sr_shadow_percascade_active(const JceSceneRenderer *sr){(void)sr;return percascade;}
static uint16_t make_texture(uint16_t size,uint16_t format,void *user){(void)user;if(size!=4096||format!=7)abort();creates++;if(fail_texture)return UINT16_MAX;live[next_handle]=true;return (uint16_t)next_handle++;}
static uint16_t make_fbo(uint16_t tex,void *user){(void)user;if(!live[tex])abort();fb_creates++;return fail_framebuffer?UINT16_MAX:tex;}
static void drop_texture(uint16_t tex,void *user){(void)user;if(!live[tex])abort();live[tex]=false;frees++;}
static void drop_fbo(uint16_t fb,void *user){(void)fb;(void)user;fb_frees++;}
static const JceSrDepthTargetProvider depth_provider={make_texture,make_fbo,drop_texture,drop_fbo,NULL};
'''
DYNAMIC_CONTROL = r'''
int main(void){
    JceSceneRenderer sr={0};sr.dyn_csm_atlas_tex.idx=sr.dyn_csm_atlas_fbo.idx=UINT16_MAX;sr.shadow_map_size=4096;sr.shadow_depth_fmt=7;
    REQUIRE(!actual_gate(&sr)&&!creates&&!sr.dyn_csm_valid);
    sr.shadow_dyn_count=1;dual=false;REQUIRE(!actual_gate(&sr)&&!creates);
    dual=true;percascade=false;REQUIRE(!actual_gate(&sr)&&!creates);
    percascade=true;fail_texture=true;REQUIRE(!actual_gate(&sr)&&creates==1&&!fb_creates&&!sr.dyn_csm_valid);
    fail_texture=false;fail_framebuffer=true;REQUIRE(!actual_gate(&sr)&&creates==2&&fb_creates==1&&frees==1&&!sr.dyn_csm_valid);
    fail_framebuffer=false;REQUIRE(actual_gate(&sr)&&creates==3&&fb_creates==2&&sr.dyn_csm_valid&&sr.dyn_csm_atlas_size==4096);
    uint16_t resident=sr.dyn_csm_atlas_tex.idx;sr.shadow_dyn_count=0;
    REQUIRE(!actual_gate(&sr)&&sr.dyn_csm_valid&&sr.dyn_csm_atlas_tex.idx==resident&&creates==3&&frees==1);
    sr.shadow_dyn_count=2;REQUIRE(actual_gate(&sr)&&creates==3);
    sr_shadow_depth_target_destroy(&sr.dyn_csm_atlas_tex.idx,&sr.dyn_csm_atlas_fbo.idx,&depth_provider);
    sr_shadow_depth_target_destroy(&sr.dyn_csm_atlas_tex.idx,&sr.dyn_csm_atlas_fbo.idx,&depth_provider);
    REQUIRE(frees==2&&fb_frees==1);
    return 0;
}
'''

class ShadowTargetDemandTests(unittest.TestCase):
    def test_actual_directional_entry_requires_light_then_complete_targets(self):
        RUNNER.execute(self, directional_probe())
    def test_negative_prebuilt_csm_without_light_is_detected(self):
        RUNNER.execute(NegativeRuntime(self), directional_probe(True))
    def test_actual_dynamic_entry_owner_failure_retry_and_retained_cache(self):
        RUNNER.execute(self, dynamic_probe())
    def test_negative_dynamic_without_movers_is_detected(self):
        RUNNER.execute(NegativeRuntime(self), dynamic_probe("need"))
    def test_negative_cold_validity_gate_deadlocks_admission_is_detected(self):
        RUNNER.execute(NegativeRuntime(self), dynamic_probe("gate"))

if __name__ == "__main__":
    unittest.main()
