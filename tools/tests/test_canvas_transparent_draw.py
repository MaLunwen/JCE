"""Run production image/quad and exact text-owner opacity controls with counted ports."""
import importlib.util
from pathlib import Path
import subprocess
import re
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

def production_function(source, signature):
    """Take the actual function body; comments/strings cannot end its scope."""
    start = source.index(signature)
    brace = source.index("{", start)
    tokens = re.finditer(r"//[^\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|[{}]",
                         source[brace:])
    depth = 0
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:brace + token.end()]
    raise AssertionError(f"Unclosed production function: {signature}")

class CanvasTransparentDrawTests(unittest.TestCase):
    def test_zero_alpha_avoids_resource_and_draw_work(self):
        spec = importlib.util.spec_from_file_location("canvas_probe_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST != "windows":
            self.skipTest("recorded compiler probe uses MSVC")
        source = (ROOT / "engine/src/middleware/scene/jce_ui_canvas.c").read_text(encoding="utf-8")
        image = production_function(source, "static void uc_draw_image(")
        quad = production_function(source, "void uc_draw_quad(")
        color = production_function(source, "uint32_t uc_color(")
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
        base = ROOT / "build/tests/canvas-transparent-tests"
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

    def test_inherited_zero_alpha_avoids_actual_text_owner_resource_and_draw_work(self):
        spec = importlib.util.spec_from_file_location("canvas_text_probe_driver", ROOT / "tools/build/jce.py")
        driver = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(driver)
        if driver.HOST != "windows":
            self.skipTest("recorded compiler probe uses MSVC")
        harness = r"""
#include "jce_ui_canvas_text.h"
#include <jce/middleware/ui/jce_localization.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
struct JceFont { int px; };
static struct JceFont face;
static unsigned char canvas_token, renderer_token;
static unsigned font_loads, measures, submits, localizations;
JceRenderer *uc_renderer(const JceUICanvas *canvas)
{
 assert(canvas == (JceUICanvas *)&canvas_token);
 return (JceRenderer *)&renderer_token;
}
int uc_font_px_clamp(int px) { return px < 4 ? 4 : px > 256 ? 256 : px; }
JceFont *uc_get_font(JceUICanvas *canvas, const char *path, int px, bool sdf)
{
 assert(canvas == (JceUICanvas *)&canvas_token);
 (void)path; (void)sdf; ++font_loads; face.px = px; return &face;
}
int jce_font_line_height(const JceFont *font) { return font->px; }
const char *jce_loc_t(const char *key) { ++localizations; return key; }
uint32_t uc_color(const float color[4], float alpha)
{ (void)color; (void)alpha; return 0xffffffffu; }
void jce_text_measure(const JceFont *font, const char *text, float *w, float *h)
{ ++measures; *w = (float)strlen(text) * font->px; *h = (float)font->px; }
void jce_text_measure_math(const JceFont *font, const char *text, float *w, float *h)
{ jce_text_measure(font, text, w, h); }
static void submit(const JceRenderer *r, const JceFont *font)
{ assert(r == (JceRenderer *)&renderer_token && font == &face); ++submits; }
void jce_text_draw_scaled_view_xf(const JceRenderer *r, JceFont *font,
 uint16_t view, float x, float y, float scale, const char *text, uint32_t color,
 const JceRectXform *xf)
{ (void)view;(void)x;(void)y;(void)scale;(void)text;(void)color;(void)xf;submit(r,font); }
void jce_text_draw_styled_view_xf(const JceRenderer *r, JceFont *font,
 uint16_t view, float x, float y, float scale, const char *text, uint32_t color,
 const JceRectXform *xf, const JceTextStyle *style)
{ (void)style;jce_text_draw_scaled_view_xf(r,font,view,x,y,scale,text,color,xf); }
void jce_text_draw_math_view(const JceRenderer *r, JceFont *font,
 uint16_t view, float x, float y, float scale, const char *text, uint32_t color)
{ jce_text_draw_scaled_view_xf(r,font,view,x,y,scale,text,color,NULL); }
int main(void)
{
 JceUICanvas *canvas = (JceUICanvas *)&canvas_token;
 UCRect rect = {0,0,100,100};
 JceUITextComponent text = {0};
 strcpy(text.locale_key, "missing-key");
 text.color[0]=text.color[1]=text.color[2]=text.color[3]=1;
 text.font_size=14;text.line_spacing=1;
 const float invisible[] = {0.0f,-1.0f,NAN};
 for (int mode=0;mode<3;++mode) {
  text.rich_text=mode==1;text.math_text=mode==2;text.best_fit=true;
  strcpy(text.text,mode==1?"<color=#ff0000ff>A</color>":mode==2?"\\frac{A}{B}":"Visible");
  for (unsigned i=0;i<sizeof invisible/sizeof invisible[0];++i) {
   uc_draw_text(canvas,1,&rect,&text,invisible[i],1,NULL);
   assert(font_loads==0 && measures==0 && submits==0 && localizations==0);
  }
 }
 // Positive inherited alpha still consumes the real plain/rich/math branches.
 for (int mode=0;mode<3;++mode) {
  text.rich_text=mode==1;text.math_text=mode==2;text.best_fit=false;
  strcpy(text.text,mode==1?"<color=#ff0000ff>A</color>":mode==2?"\\frac{A}{B}":"Visible");
  unsigned before_loads=font_loads,before_draws=submits,before_measures=measures;
  uc_draw_text(canvas,1,&rect,&text,0.01f,1,NULL);
  assert(font_loads==before_loads+1 && submits>before_draws && measures>before_measures);
 }
 // Transparent base text does not suppress an independently visible rich span.
 text.color[3]=0;text.rich_text=true;text.math_text=false;
 strcpy(text.text,"<color=#ff0000ff>A</color>");
 unsigned before=submits;
 uc_draw_text(canvas,1,&rect,&text,1,1,NULL);
 assert(submits==before+1);
 // Nor does it suppress an authored outline/shadow on the styled path.
 text.rich_text=false;text.sdf=true;text.outline_width=1;
 text.outline_color[3]=text.shadow_color[3]=1;
 strcpy(text.text,"Styled");before=submits;
 uc_draw_text(canvas,1,&rect,&text,1,1,NULL);
 assert(submits==before+1);
 puts("transparent Canvas text owner PASS");return 0;
}
"""
        base = ROOT / "build/tests/canvas-transparent-tests"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=base) as temporary:
            directory = Path(temporary)
            file = directory / "text_probe.c"
            file.write_text(harness, encoding="utf-8")
            production = ROOT / "engine/src/middleware/scene/jce_ui_canvas_text.c"
            result = subprocess.run(["cl", "/nologo", "/TC", "/std:c11", "/W3", "/utf-8",
                                     "/I" + str(ROOT / "engine/include"),
                                     "/I" + str(production.parent), str(file), str(production),
                                     "/Fe:text_probe.exe"], cwd=directory,
                                    env=driver.msvc_env(driver.resolve_target(None)), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(directory / "text_probe.exe")], cwd=directory,
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Canvas text owner PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()
