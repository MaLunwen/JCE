"""Exact central image owner: counted decode admission, no SDL/GPU allocation.

Small native pixel/Bundle tests complement these CPU ports. The large control
contains a complete 32768-square RLE payload (32 MiB), but never allocates the
4 GiB decoded output. Negative guards must compile and fail at runtime.
"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def helper(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools/tests" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SOURCE = helper("test_canvas_transparent_draw.py", "tga_source_helpers")
RUN = helper("test_material_owned_cleanup.py", "tga_compiler_helpers")
FAILURE = helper("test_font_atlas_failure.py", "tga_negative_helpers").RuntimeFailure


def probe(control, mutation=None):
    text = (ROOT / "engine/src/renderer/jce_image.c").read_text(encoding="utf-8")
    if mutation == "attribute_alpha":
        original = "if (colour && (depth == 15u || depth == 16u) && attributes != 0u)"
        assert text.count(original) == 1
        text = text.replace(original, "if (false && colour && (depth == 15u || depth == 16u) && attributes != 0u)", 1)
    elif mutation == "palette_alpha":
        original = "if (indexed && b[7] == 32u)"
        assert text.count(original) == 1
        text = text.replace(original, "if (false && indexed && b[7] == 32u)", 1)
    elif mutation == "decoded_limit":
        # This guard is an unbraced if. Body extraction would consume a later
        # palette block and leave its else orphaned; disable only the exact
        # condition so the historical mutant remains valid production syntax.
        condition = ("if (rgba_bytes > (uint64_t)INT_MAX || rgba_pitch > (uint64_t)INT_MAX ||\n"
                     "        rgba_bytes > (uint64_t)SIZE_MAX)")
        assert text.count(condition) == 1
        text = text.replace(condition, "if (false)", 1)
    functions = "\n".join(SOURCE.production_function(text, signature) for signature in (
        "static bool img_is_gray16_png(", "static bool img_tga_reject(",
        "static bool img_tga_admitted(", "uint8_t *jce_image_load_rgba8_from_memory(",
        "void jce_image_free_rgba8("))
    return PORTS + "\n" + functions + "\n" + CONTROLS[control]


PORTS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint8_t stbi_uc;
typedef struct { const uint8_t *data; size_t size; bool open; } SDL_IOStream;
typedef struct { int format,w,h,pitch; void *pixels; } SDL_Surface;
#define SDL_PIXELFORMAT_RGBA32 1
#define LOG_TAG "controlled-image-admission"
static unsigned auto_calls,typed_calls,opens,closes,allocations,frees,errors,destroyed;
static bool auto_success;
static char last_error[512];
static uint8_t pixel[4]={255,0,0,255};
static SDL_Surface surface={SDL_PIXELFORMAT_RGBA32,1,1,4,pixel};
static SDL_IOStream streams[2];
static void log_error(const char *tag,const char *fmt,...)
{(void)tag;++errors;va_list ap;va_start(ap,fmt);vsnprintf(last_error,sizeof(last_error),fmt,ap);va_end(ap);}
#define LOG_ERROR log_error
static SDL_IOStream *SDL_IOFromConstMem(const void *data,size_t size)
{assert(opens<2);SDL_IOStream *io=&streams[opens++];io->data=data;io->size=size;io->open=true;return io;}
static SDL_Surface *IMG_Load_IO(SDL_IOStream *io,bool closeio)
{assert(closeio && io->open);io->open=false;++closes;++auto_calls;return auto_success?&surface:NULL;}
static SDL_Surface *IMG_LoadTyped_IO(SDL_IOStream *io,bool closeio,const char *type)
{
 assert(closeio && io->open && !strcmp(type,"TGA"));io->open=false;++closes;++typed_calls;
 /* Counted failure port forbids any huge decoded allocation, even mutants. */
 assert(io->size>=18);
 unsigned width=io->data[12]|((unsigned)io->data[13]<<8);
 return width>=32768?NULL:&surface;
}
static const char *SDL_GetError(void){return "counted codec failure";}
static SDL_Surface *SDL_ConvertSurface(SDL_Surface *s,int format)
{assert(s==&surface && format==SDL_PIXELFORMAT_RGBA32);return &surface;}
static void SDL_DestroySurface(SDL_Surface *s){assert(s==&surface);++destroyed;}
static stbi_uc *stbi_load_from_memory(const stbi_uc *b,int n,int *w,int *h,int *ch,int req)
{(void)b;(void)n;(void)w;(void)h;(void)ch;(void)req;return NULL;}
static void stbi_image_free(void *p){assert(!p);}
static void *engine_alloc(size_t n){assert(n<4096);++allocations;return malloc(n);}
static void engine_free(void *p){if(p){++frees;free(p);}}
#define JCE_MALLOC engine_alloc
#define JCE_FREE engine_free
static void reset(void)
{auto_calls=typed_calls=opens=closes=allocations=frees=errors=destroyed=0;auto_success=false;last_error[0]=0;}
"""

CONTROLS = {
    "small": r"""
int main(void)
{
 const uint8_t raw24[]={0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,24,0x20,0,0,255};
 const uint8_t true32[]={0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,32,0x28,0,0,255,40};
 const uint8_t opaque_palette[]={0,1,1,0,0,1,0,32,0,0,0,0,1,0,1,0,8,0x20,0,0,255,255,0};
 const uint8_t *data[]={raw24,true32,opaque_palette};
 const size_t sizes[]={sizeof(raw24),sizeof(true32),sizeof(opaque_palette)};
 for(unsigned i=0;i<3;++i){
  reset();int w=-1,h=-2;uint8_t *p=jce_image_load_rgba8_from_memory(data[i],sizes[i],&w,&h);
  assert(p && w==1 && h==1 && auto_calls==1 && typed_calls==1);
  assert(opens==2 && closes==2 && allocations==1 && destroyed==1 && errors==0);
  jce_image_free_rgba8(p);assert(frees==1);
 }
 /* Signature-auto success must retain priority and skip TGA typed loading. */
 reset();auto_success=true;
 const uint8_t png_sig[]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
 int w=0,h=0;uint8_t *p=jce_image_load_rgba8_from_memory(png_sig,sizeof(png_sig),&w,&h);
 assert(p && auto_calls==1 && typed_calls==0 && opens==1 && closes==1 && errors==0);
 jce_image_free_rgba8(p);assert(frees==1);
 puts("TGA small admission owner control PASS (counted ports)");return 0;
}
""",
    "alpha": r"""
int main(void)
{
 const uint8_t alpha16[]={0,0,2,0,0,0,0,0,0,0,0,0,1,0,1,0,16,0x21,0,0};
 const uint8_t palette32[]={0,1,1,0,0,1,0,32,0,0,0,0,1,0,1,0,8,0x20,0,0,255,40,0};
 const uint8_t *data[]={alpha16,palette32};const size_t sizes[]={sizeof(alpha16),sizeof(palette32)};
 const char *reasons[]={"15/16-bit attribute alpha unsupported","32-bit palette alpha unsupported"};
 for(unsigned i=0;i<2;++i){
  reset();int w=-1,h=-2;uint8_t *p=jce_image_load_rgba8_from_memory(data[i],sizes[i],&w,&h);
  assert(!p && w==-1 && h==-2 && auto_calls==1 && typed_calls==0);
  assert(opens==1 && closes==1 && allocations==0 && destroyed==0 && errors==1);
  assert(strstr(last_error,"stage=tga-admission") && strstr(last_error,reasons[i]));
 }
 puts("TGA unsupported alpha rejected before typed decoder PASS");return 0;
}
""",
    "huge": r"""
int main(void)
{
 const uint64_t decoded_pixels=(uint64_t)32768u*32768u;
 const size_t packets=(size_t)(decoded_pixels/128u),size=18u+packets*4u;
 uint8_t *encoded=(uint8_t *)malloc(size);assert(encoded);memset(encoded,0,18u);
 encoded[2]=10;encoded[13]=encoded[15]=128;encoded[16]=24;encoded[17]=0x20;
 for(size_t i=0;i<packets;++i){size_t at=18u+i*4u;encoded[at]=0xff;encoded[at+1]=0;encoded[at+2]=0;encoded[at+3]=255;}
 /* Complete, valid RLE packet count/payload, not a false truncation control. */
 assert(size==33554450u && decoded_pixels==1073741824ull);
 reset();int w=-1,h=-2;uint8_t *p=jce_image_load_rgba8_from_memory(encoded,size,&w,&h);
 assert(!p && w==-1 && h==-2 && typed_calls==0 && auto_calls==1);
 assert(opens==1 && closes==1 && allocations==0 && destroyed==0 && errors==1);
 assert(strstr(last_error,"stage=tga-admission") && strstr(last_error,"exceeds INT_MAX"));
 free(encoded);puts("TGA complete 4GiB decoded input rejected before typed decoder PASS");return 0;
}
""",
}


class ImageTgaAdmissionTests(unittest.TestCase):
    def test_actual_owner_small_formats_and_alpha_admission(self):
        RUN.execute(self, probe("small"))
        RUN.execute(self, probe("alpha"))

    def test_complete_large_rle_rejected_before_decoder_allocation(self):
        RUN.execute(self, probe("huge"))

    def test_ignored_attribute_alpha_mutant_compiles_but_fails(self):
        RUN.execute(FAILURE(self), probe("alpha", "attribute_alpha"))

    def test_lossy_palette_alpha_mutant_compiles_but_fails(self):
        RUN.execute(FAILURE(self), probe("alpha", "palette_alpha"))

    def test_decoded_limit_mutant_compiles_but_fails_without_large_allocation(self):
        RUN.execute(FAILURE(self), probe("huge", "decoded_limit"))


if __name__ == "__main__":
    unittest.main()
