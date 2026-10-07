"""Compile the production image/quad draw path with visible resource/draw spies."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class CanvasTransparentDrawTests(unittest.TestCase):
    def test_zero_alpha_avoids_resource_and_draw_work(self):
        spec = importlib.util.spec_from_file_location("canvas_probe_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST != "windows":
            self.skipTest("recorded compiler probe uses MSVC")
        source = (ROOT / "engine/src/middleware/scene/jce_ui_canvas.c").read_text(encoding="utf-8")
        image = source[source.index("static void uc_draw_image("):source.index("/* ── Text draw")]
        quad = source[source.index("void uc_draw_quad("):source.index("static void uc_draw_slider(")]
        color = source[source.index("uint32_t uc_color("):source.index("static void uc_lerp4(")]
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct { void *renderer; } JceUICanvas;
typedef struct { float x,y,w,h; } UCRect;
typedef struct { int unused; } JceRectXform;
typedef struct { int unused; } JceRectTransform;
typedef struct { uint16_t idx; } JceTexture;
typedef struct {
 float color[4]; char sprite_path[64]; JceRectTransform rect;
 int image_type; float slice_border[4]; bool preserve_aspect; float fill_amount;
} JceUIImageComponent;
#define JCE_UI_IMAGE_SLICED 1
#define JCE_UI_IMAGE_TILED 2
#define JCE_UI_IMAGE_FILLED 3
static int draws,loads;
static uint32_t jce_rgba(uint8_t r,uint8_t g,uint8_t b,uint8_t a)
{ return (uint32_t)r|((uint32_t)g<<8)|((uint32_t)b<<16)|((uint32_t)a<<24); }
static JceTexture uc_get_texture(JceUICanvas *u,const char *p)
{ (void)u;(void)p;++loads;return (JceTexture){4}; }
static bool jce_texture_valid(JceTexture t){return t.idx!=UINT16_MAX;}
static void jce_texture_get_size(JceTexture t,uint32_t *w,uint32_t *h)
{(void)t;*w=32;*h=32;}
static const JceRectXform *uc_elem_xform(const JceRectTransform *t,const UCRect *r,JceRectXform *x)
{(void)t;(void)r;(void)x;return NULL;}
static void jce_draw_filled_rect_view_xf(void *r,...){(void)r;++draws;}
static void jce_draw_textured_rect_view_xf(void *r,...){(void)r;++draws;}
static void uc_tex_view(void *r,...){(void)r;++draws;}
static void uc_rect_view(void *r,...){(void)r;++draws;}
''' + color + image + quad + r'''
int main(void)
{
 JceUICanvas canvas={0};UCRect rect={0,0,20,20};
 JceUIImageComponent image={0};image.color[0]=image.color[1]=image.color[2]=1;
 uc_draw_image(&canvas,1,&rect,&image,1,1);
 assert(draws==0 && loads==0);
 image.color[3]=1;uc_draw_image(&canvas,1,&rect,&image,0,1);
 assert(draws==0 && loads==0);
 image.color[3]=0.01f;uc_draw_image(&canvas,1,&rect,&image,1,1);
 assert(draws==1 && loads==1); /* visible low alpha is retained */
 image.color[3]=1;uc_draw_image(&canvas,1,&rect,&image,1,1);
 assert(draws==2 && loads==2); /* opaque child graphics remain usable */
 float transparent[4]={1,1,1,0};uc_draw_quad(&canvas,1,0,0,20,20,"sprite",transparent,1);
 assert(draws==2 && loads==2);
 float opaque[4]={1,1,1,1};uc_draw_quad(&canvas,1,0,0,20,20,"sprite",opaque,1);
 assert(draws==3 && loads==3);
 puts("transparent Canvas graphics PASS");return 0;
}
'''
        base = ROOT / "build/canvas-transparent-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "probe.c"
            file.write_text(harness, encoding="utf-8")
            result = subprocess.run(["cl", "/nologo", "/TC", "/std:c11", "/W3", str(file),
                                     "/Fe:probe.exe", "/Fo:probe.obj"], cwd=directory,
                                    env=driver.msvc_env(driver.resolve_target(None)), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(directory / "probe.exe")], cwd=directory,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Canvas graphics PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
