'''Run actual Canvas resource/image owners with a fixed 64-entry budget.

CPU ports validate admission, fallback and one warning per generation, not GPU
pixels or real project IO. The negative still compiles and must fail at runtime.
'''
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('canvas_budget_helpers', ROOT / 'tools/tests/test_font_atlas_failure.py')
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def probe(mutation=False):
    source = (ROOT / 'engine/src/middleware/scene/jce_ui_canvas.c').read_text(encoding='utf-8')
    if mutation:
        guard = HELPERS.production_function(source, '        if (!uc->texture_budget_warned) {')
        source = source.replace(guard, '', 1)
    cache_end = source.index('} UCTexSlot;') + len('} UCTexSlot;')
    layout = source[source.rfind('typedef struct', 0, cache_end):cache_end]
    budget = next(line for line in source.splitlines() if line.startswith('#define UC_MAX_TEXTURES'))
    owners = '\n'.join(HELPERS.production_function(source, signature) for signature in (
        'static JceTexture uc_get_texture(', 'uint32_t uc_color(', 'static void uc_draw_image('))
    return PORTS + budget + '\n' + layout + CANVAS_PORTS + owners + CONTROL


PORTS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef struct { uint16_t idx; } JceTexture;
typedef struct JcePakArchive JcePakArchive;
typedef struct JceFileSystem JceFileSystem;
#define JCE_TEXTURE_INVALID ((JceTexture){UINT16_MAX})
#define JCE_TEX_CLAMP 1
#define LOG_TAG "controlled-canvas-budget"
'''
CANVAS_PORTS = r'''
typedef struct { UCTexSlot textures[UC_MAX_TEXTURES]; int texture_count; bool texture_budget_warned;
    uint32_t tex_root_gen; const JcePakArchive *pak; void *renderer; } JceUICanvas;
typedef struct {float x,y,w,h;} UCRect;
typedef struct {int unused;} JceRectTransform;
typedef struct {int unused;} JceRectXform;
typedef struct {float color[4];char sprite_path[64];JceRectTransform rect;
    int image_type;float slice_border[4];bool preserve_aspect;float fill_amount;} JceUIImageComponent;
#define JCE_UI_IMAGE_SLICED 1
#define JCE_UI_IMAGE_TILED 2
#define JCE_UI_IMAGE_FILLED 3
static unsigned generation=1,loads,destroys,budget_warnings,flat_draws,texture_draws,next_texture;
static bool live[128];static char warning[512];
static void diagnostic(const char *tag,const char *fmt,...){
    (void)tag;char text[512];va_list args;va_start(args,fmt);vsnprintf(text,sizeof text,fmt,args);va_end(args);
    if(strstr(text,"reason=entry_budget")){budget_warnings++;strcpy(warning,text);}
}
#define LOG_WARN diagnostic
static bool jce_texture_valid(JceTexture t){return t.idx!=UINT16_MAX;}
static unsigned uc_resource_generation(void){return generation;}
static const char *uc_resource_asset_root(void){return "";}
static JceFileSystem *jce_fs_get_active(void){return NULL;}
static void *jce_fs_host_read_all(const char *path,uint64_t *size){(void)path;(void)size;assert(!"unexpected host IO");return NULL;}
static void *jce_fs_read_all(JceFileSystem *fs,const char *path,uint64_t *size){(void)fs;(void)path;(void)size;assert(!"unexpected VFS IO");return NULL;}
static void jce_fs_buffer_free(void *p){assert(!p);}
static void *jce_texture_decode_cpu_mem(const void *p,size_t size,int sampler){(void)p;(void)size;(void)sampler;assert(!"unexpected decode");return NULL;}
static JceTexture jce_texture_upload_cpu(void *p){(void)p;assert(!"unexpected upload");return JCE_TEXTURE_INVALID;}
static JceTexture jce_texture_load(const JcePakArchive *pak,const char *path){
    assert(pak&&path);loads++;if(strstr(path,"missing"))return JCE_TEXTURE_INVALID;
    assert(next_texture<128);JceTexture t={(uint16_t)next_texture++};live[t.idx]=true;return t;
}
static void jce_texture_destroy(JceTexture t){assert(t.idx<128&&live[t.idx]);live[t.idx]=false;destroys++;}
static void jce_strlcpy(char *dst,const char *src,size_t size){snprintf(dst,size,"%s",src);}
static void jce_texture_get_size(JceTexture t,uint32_t *width,uint32_t *height){assert(t.idx<128&&live[t.idx]);*width=*height=32;}
static uint32_t jce_rgba(uint8_t r,uint8_t g,uint8_t b,uint8_t a){return r|((uint32_t)g<<8)|((uint32_t)b<<16)|((uint32_t)a<<24);}
static const JceRectXform *uc_elem_xform(const JceRectTransform *t,const UCRect *r,JceRectXform *x){(void)t;(void)r;(void)x;return NULL;}
static void jce_draw_filled_rect_view_xf(void *renderer,...){(void)renderer;flat_draws++;}
static void jce_draw_textured_rect_view_xf(void *renderer,...){(void)renderer;texture_draws++;}
static void uc_tex_view(void *renderer,...){(void)renderer;texture_draws++;}
static void uc_rect_view(void *renderer,...){(void)renderer;flat_draws++;}
'''
CONTROL = r'''
#define REQUIRE(value) do {if(!(value)){fprintf(stderr,"Canvas constraint failed at %d: %s\n",__LINE__,#value);return 3;}}while(0)
int main(void){
    JceUICanvas canvas={0};canvas.pak=(JcePakArchive *)&canvas;
    JceTexture first=uc_get_texture(&canvas,"existing.png");REQUIRE(first.idx==0);
    char path[64];for(unsigned i=1;i<UC_MAX_TEXTURES;++i){snprintf(path,sizeof path,"missing-%u.png",i);uc_get_texture(&canvas,path);}
    REQUIRE(canvas.texture_count==UC_MAX_TEXTURES&&loads==UC_MAX_TEXTURES&&budget_warnings==0);
    JceUIImageComponent image={0};for(unsigned i=0;i<4;++i)image.color[i]=1;
    strcpy(image.sprite_path,"new-visible.png");UCRect rect={0,0,20,20};
    uc_draw_image(&canvas,1,&rect,&image,1,1);uc_draw_image(&canvas,1,&rect,&image,1,1);
    REQUIRE(flat_draws==2&&texture_draws==0&&loads==UC_MAX_TEXTURES&&budget_warnings==1);
    REQUIRE(strstr(warning,"path=new-visible.png")&&strstr(warning,"count=64 limit=64")&&strstr(warning,"generation=1")&&strstr(warning,"fallback=flat_color"));
    REQUIRE(uc_get_texture(&canvas,"existing.png").idx==first.idx&&budget_warnings==1&&loads==UC_MAX_TEXTURES);
    generation++;uc_draw_image(&canvas,1,&rect,&image,1,1);
    REQUIRE(destroys==1&&texture_draws==1&&canvas.texture_count==1&&!canvas.texture_budget_warned);
    for(unsigned i=1;i<UC_MAX_TEXTURES;++i){snprintf(path,sizeof path,"missing-next-%u.png",i);uc_get_texture(&canvas,path);}
    strcpy(image.sprite_path,"another-visible.png");uc_draw_image(&canvas,1,&rect,&image,1,1);
    REQUIRE(budget_warnings==2&&flat_draws==3&&strstr(warning,"generation=2"));
    generation++;uc_get_texture(&canvas,"flush-missing.png");REQUIRE(destroys==2);
    for(unsigned i=0;i<128;++i)REQUIRE(!live[i]);return 0;
}
'''


class CanvasTextureBudgetTests(unittest.TestCase):
    def test_full_cache_preserves_hits_falls_back_and_warns_once_per_generation(self):
        HELPERS.execute(self, probe())

    def test_negative_previous_silent_budget_rejection_is_detected(self):
        HELPERS.execute(HELPERS.RuntimeFailure(self), probe(True))


if __name__ == '__main__':
    unittest.main()
