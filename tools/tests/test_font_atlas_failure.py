'''Exact production font atlas owners with counted CPU failure ports.

This does not rasterize a real font or submit to a GPU. Positive controls must
compile and run; historical admission mutations must compile but fail runtime
ownership/publication constraints. No production algorithm is copied.
'''
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


def load_helper(filename, name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/tests' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


production_function = load_helper('test_canvas_transparent_draw.py', 'font_source_helper').production_function
execute = load_helper('test_material_owned_cleanup.py', 'font_compiler_helper').execute


class RuntimeFailure:
    '''Keep compiler success mandatory when expecting a runtime negative control.'''
    def __init__(self, test):
        self.test = test
        self.phase = 0

    def assertEqual(self, actual, expected, message):
        self.phase += 1
        if self.phase == 1:
            self.test.assertEqual(actual, expected, message)
        else:
            self.test.assertNotEqual(actual, expected, message)

    def skipTest(self, message):
        self.test.skipTest(message)


def font_probe(control, mutation=None):
    text = (ROOT / 'engine/src/renderer/jce_text.c').read_text(encoding='utf-8')
    if mutation == 'static_bitmap':
        guard = production_function(text, '            if (!bitmaps[i]) {')
        text = text.replace(guard, '', 1)
    elif mutation == 'dynamic_admission':
        guard = production_function(text, '        if (!jce_texture_valid(font->dyn_atlas)) {')
        text = text.replace(guard, '', 1)
    elif mutation == 'dynamic_update':
        guard = production_function(text, '    if (!jce_texture_update_rgba(font->dyn_atlas,')
        text = text.replace(guard,
            '(void)jce_texture_update_rgba(font->dyn_atlas, font->dyn_pixels, DYN_ATLAS_DIM, DYN_ATLAS_DIM);', 1)
    elif mutation == 'blob_compare':
        marker = ' && !memcmp(blob->data, data, size)'
        assert text.count(marker) == 1
        text = text.replace(marker, '', 1)
    elif mutation == 'blob_release':
        marker = '    font_bytes_release(font->font_data);'
        assert text.count(marker) == 1
        text = text.replace(marker, '    (void)font->font_data;', 1)
    constants = text[text.index('#define GLYPH_FIRST'):text.index('/* Per-glyph metrics')]
    layout = text[text.index('typedef struct {', text.index('/* Per-glyph metrics')):
                  text.index('/* -- The chain,')]
    dynamic_helpers = ''
    if 'FONT_DYN_FAIL_PIXELS' in text:
        start = text.index('enum {\n    FONT_DYN_FAIL_PIXELS')
        dynamic_helpers = text[start:text.index('static const GlyphInfo *font_render_dynamic_glyph(', start)]
    owners = '\n'.join(production_function(text, signature) for signature in (
        'static uint32_t next_pow2(', 'static void glyph_map_insert(',
        'static const GlyphInfo *glyph_map_find('))
    owners += '\n' + dynamic_helpers + '\n'
    owners += production_function(text, 'static const GlyphInfo *font_render_dynamic_glyph(') + '\n'
    owners += production_function(text, 'static JceTexture build_atlas(') + '\n'
    # Skip the forward declaration before the public PAK/memory wrappers.
    open_start = text.index('static JceFont *font_open_from_memory(', text.index('JceFont *jce_font_open_mem_ex('))
    owners += production_function(text[open_start:], 'static JceFont *font_open_from_memory(') + '\n'
    owners += production_function(text, 'void jce_font_close(') + '\n'
    blobs = text[text.index('typedef struct FontBytes {'):text.index('static bool ensure_ft_init(')]
    init = production_function(text, 'static bool ensure_ft_init(')
    owners += production_function(text, 'JceFont *jce_font_open_mem_ex(') + '\n'
    owners += production_function(text, 'void jce_text_shutdown(') + '\n'
    return PORTS + blobs + constants + layout + FONT_PORTS + init + owners + MAIN_OPEN + control + MAIN_CLOSE


PORTS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
typedef struct { uint16_t idx; } JceTexture;
typedef struct JceFont JceFont;
#define JCE_TEXTURE_INVALID ((JceTexture){UINT16_MAX})
#define JCE_FONT_MAX_FALLBACKS 4
#define LOG_TAG "controlled-font-owner"
typedef unsigned char FT_Byte;
typedef unsigned FT_UInt;
typedef long FT_Long;
typedef int FT_Int;
typedef int FT_Error;
typedef void *FT_Library;
typedef struct { unsigned width, rows; int pitch; unsigned char *buffer; } Bitmap;
typedef struct { Bitmap bitmap; struct { long x,y; } advance; int bitmap_left,bitmap_top; } GlyphSlot;
typedef GlyphSlot *FT_GlyphSlot;
typedef struct { struct {long height,ascender;} metrics; } Size;
typedef struct { FT_GlyphSlot glyph; Size *size; const char *family_name; bool live; unsigned pixels; const FT_Byte *data; } Face;
typedef Face *FT_Face;
typedef struct {bool live;} hb_font_t;
static unsigned malloc_calls,calloc_calls,fail_malloc_at,fail_calloc_at;
static unsigned owned_allocs, texture_creates,texture_destroys,update_calls,next_texture=1;
static unsigned dynamic_warnings,static_bitmap_errors,face_closes,hb_closes;
static bool texture_live[64],face_live,hb_live,fail_dynamic_texture,fail_update;
static void *allocations[1024];
static uint8_t gpu_pixels[512u*512u*4u];
static char last_dynamic_warning[512],last_static_error[512];
static bool fail_face, force_hash_collision, trace_on=true;
static unsigned scopes, scope_ends, trace_counters, live_faces, live_hb;
typedef struct {uint64_t id,parent;} JceTraceScope;
#define JCE_TRACE_CATEGORY_RENDER 1
#define JCE_TRACE_TASK_FAILED 2
#define JCE_TRACE_TASK_SUCCEEDED 3
static JceTraceScope jce_trace_scope_begin(const char *name){(void)name;return (JceTraceScope){trace_on?++scopes:0,0};}
static void jce_trace_scope_end(JceTraceScope scope,int state){(void)state;if(scope.id)scope_ends++;}
static void jce_trace_counter_scoped_internal(int category,const char *name,int64_t value,uint64_t id,uint64_t parent){
    (void)category;(void)name;(void)parent;assert(id&&value>=0);trace_counters++;
}
static uint64_t jce_archive_content_hash(const void *data,size_t bytes){
    if(force_hash_collision)return 7;
    uint64_t h=13;const unsigned char *p=data;for(size_t i=0;i<bytes;i++)h=h*31+p[i];return h;
}
static void *record_allocation(void *p){
    assert(p);for(unsigned i=0;i<1024;++i)if(!allocations[i]){allocations[i]=p;owned_allocs++;return p;}
    assert(!"allocation witness capacity");return NULL;
}
static void *controlled_malloc(size_t size){
    malloc_calls++;if(malloc_calls==fail_malloc_at)return NULL;return record_allocation(malloc(size));
}
static void *controlled_calloc(size_t count,size_t size){
    calloc_calls++;if(calloc_calls==fail_calloc_at)return NULL;return record_allocation(calloc(count,size));
}
static void controlled_free(void *p){
    if(!p)return;for(unsigned i=0;i<1024;++i)if(allocations[i]==p){allocations[i]=NULL;owned_allocs--;free(p);return;}
    assert(!"free without actual owner or repeated free");
}
#define JCE_MALLOC controlled_malloc
#define JCE_CALLOC controlled_calloc
#define JCE_FREE controlled_free
static void diagnostic(const char *tag,const char *fmt,...){
    (void)tag;char buffer[512];va_list args;va_start(args,fmt);vsnprintf(buffer,sizeof buffer,fmt,args);va_end(args);
    if(strstr(buffer,"phase=dynamic_")){dynamic_warnings++;strcpy(last_dynamic_warning,buffer);}
    if(strstr(buffer,"phase=static_bitmap")){static_bitmap_errors++;strcpy(last_static_error,buffer);}
}
#define LOG_WARN diagnostic
#define LOG_ERROR diagnostic
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
static bool jce_texture_valid(JceTexture texture){return texture.idx!=UINT16_MAX;}
'''
FONT_PORTS = r'''
static unsigned char bitmap[4]={0x40,0x80,0xc0,0xff};
static GlyphSlot slot={{2,2,2,bitmap},{3*64,0},0,2};
static Size size={{4*64,3*64}};
static Face face={&slot,&size,"controlled face"};
static Face faces[16]; static hb_font_t hbs[16];
static FT_Error FT_Init_FreeType(FT_Library *out){*out=(void *)&faces;return 0;}
static FT_Error FT_Done_FreeType(FT_Library lib){assert(lib==&faces&&!live_faces);return 0;}
static FT_Error FT_New_Memory_Face(FT_Library lib,const FT_Byte *data,FT_Long bytes,FT_Long index,FT_Face *out){
    (void)lib;(void)index;assert(data&&bytes>0);if(fail_face)return 1;
    for(unsigned i=0;i<16;i++)if(!faces[i].live){faces[i]=face;faces[i].live=true;faces[i].data=data;
        live_faces++;face_live=true;*out=&faces[i];return 0;}
    assert(!"face port capacity");return 1;
}
static FT_Error FT_Set_Pixel_Sizes(FT_Face f,FT_UInt x,FT_UInt y){assert(f&&f->live);(void)x;f->pixels=y;return 0;}
static FT_Error FT_Property_Set(FT_Library lib,const char *module,const char *key,const void *value){
    (void)lib;(void)module;(void)key;(void)value;return 0;
}
static FT_Error FT_Done_Face(FT_Face f){assert(f&&f->live);bool bytes_live=false;
    for(unsigned i=0;i<1024;i++)if(allocations[i]==f->data)bytes_live=true;
    assert(bytes_live);f->live=false;face_live=--live_faces!=0;face_closes++;return 0;}
static hb_font_t *hb_ft_font_create_referenced(FT_Face f){assert(f&&f->live);unsigned i=(unsigned)(f-faces);
    assert(i<16&&!hbs[i].live);hbs[i].live=true;live_hb++;hb_live=true;return &hbs[i];}
static void hb_font_destroy(hb_font_t *f){assert(f&&f->live);f->live=false;hb_live=--live_hb!=0;hb_closes++;}
static void jce_text_shape_forget(void *f){assert(f);}
static void jce_text_shape_stats(uint64_t *hits,uint64_t *misses){*hits=*misses=0;}
static void jce_text_shape_shutdown(void){}
static FT_Error text_render_glyph(FT_Face f,uint32_t cp,int spread){
    assert(f&&f->live);(void)cp;(void)spread;return 0;
}
static JceTexture jce_texture_from_rgba(const void *data,uint32_t width,uint32_t height){
    assert(data&&width&&height);texture_creates++;
    bool dynamic=width==DYN_ATLAS_DIM&&height==DYN_ATLAS_DIM;
    if(dynamic&&fail_dynamic_texture)return JCE_TEXTURE_INVALID;
    assert(next_texture<64);JceTexture t={(uint16_t)next_texture++};texture_live[t.idx]=true;
    if(dynamic)memcpy(gpu_pixels,data,sizeof gpu_pixels);return t;
}
static bool jce_texture_update_rgba(JceTexture t,const void *data,uint32_t width,uint32_t height){
    update_calls++;if(!jce_texture_valid(t)||fail_update)return false;
    assert(t.idx<64&&texture_live[t.idx]&&data&&width==DYN_ATLAS_DIM&&height==DYN_ATLAS_DIM);
    memcpy(gpu_pixels,data,sizeof gpu_pixels);return true;
}
static void jce_texture_destroy(JceTexture t){
    if(!jce_texture_valid(t))return;assert(t.idx<64&&texture_live[t.idx]);texture_live[t.idx]=false;texture_destroys++;
}
'''
MAIN_OPEN = r'''
#define REQUIRE(value) do {if(!(value)){fprintf(stderr,"owner constraint failed at %d: %s\n",__LINE__,#value);return 3;}}while(0)
static JceFont *open_font(void){
    void *input=controlled_malloc(16);assert(input);memset(input,7,16);return font_open_from_memory(input,16,"fonts/requested.ttf",14,NULL,0);
}
int main(void){
'''
MAIN_CLOSE = r'''
    REQUIRE(owned_allocs==0&&!face_live&&!hb_live);
    REQUIRE(!s_text.blobs&&!s_text.unique_bytes&&!s_text.references&&scopes==scope_ends);
    for(unsigned i=0;i<64;++i)REQUIRE(!texture_live[i]);return 0;
}
'''
STATIC = r'''
    fail_malloc_at=4; /* input + two actual bitmaps, then the third bitmap fails */
    JceFont *font=open_font();
    REQUIRE(font==NULL&&texture_creates==0&&owned_allocs==0);
    REQUIRE(face_closes==1&&hb_closes==1&&static_bitmap_errors==1);
    REQUIRE(strstr(last_static_error,"fonts/requested.ttf")&&strstr(last_static_error,"cp=U+"));
    fail_malloc_at=0;font=open_font();REQUIRE(font&&jce_texture_valid(font->atlas));
    REQUIRE(font->ascii['A'-GLYPH_FIRST].u1>font->ascii['A'-GLYPH_FIRST].u0);
    jce_font_close(font);REQUIRE(texture_destroys==1&&face_closes==2&&hb_closes==2);
'''
INIT = r'''
    for(unsigned stage=0;stage<3;++stage){
        JceFont *font=open_font();REQUIRE(font);
        unsigned baseline=owned_allocs,warnings=dynamic_warnings,creates=texture_creates;
        for(unsigned repeat=0;repeat<2;++repeat){
            if(stage<2)fail_calloc_at=calloc_calls+(stage==0?1:2);
            else fail_dynamic_texture=true;
            REQUIRE(font_render_dynamic_glyph(font,0x4e00)==NULL);
            REQUIRE(owned_allocs==baseline&&!font->dyn_full&&!font->dyn_pixels&&!font->dyn_map);
            REQUIRE(font->dyn_cap==0&&font->dyn_count==0&&!jce_texture_valid(font->dyn_atlas));
            REQUIRE(dynamic_warnings==warnings+1);
        }
        REQUIRE(strstr(last_dynamic_warning,"font=")&&strstr(last_dynamic_warning,"cp=U+00004E00"));
        REQUIRE(stage!=2||texture_creates==creates+2);
        fail_calloc_at=0;fail_dynamic_texture=false;
        const GlyphInfo *glyph=font_render_dynamic_glyph(font,0x4e00);
        REQUIRE(glyph&&glyph->dynamic&&font->dyn_count==1&&jce_texture_valid(font->dyn_atlas));
        REQUIRE(dynamic_warnings==warnings+1);jce_font_close(font);
        REQUIRE(owned_allocs==0);
    }
'''
UPDATE = r'''
    JceFont *font=open_font();REQUIRE(font);
    const GlyphInfo *old=font_render_dynamic_glyph(font,0x4e00);REQUIRE(old);
    int old_x=font->dyn_pen_x,old_y=font->dyn_pen_y,old_row=font->dyn_row_h;
    GlyphInfo old_value=*old;unsigned warnings=dynamic_warnings;
    fail_update=true;
    for(unsigned repeat=0;repeat<2;++repeat){
        REQUIRE(font_render_dynamic_glyph(font,0x4e01)==NULL);
        REQUIRE(glyph_map_find(font->dyn_map,font->dyn_cap,0x4e01)==NULL&&font->dyn_count==1);
        REQUIRE(font->dyn_pen_x==old_x&&font->dyn_pen_y==old_y&&font->dyn_row_h==old_row);
        REQUIRE(memcmp(font->dyn_pixels,gpu_pixels,sizeof gpu_pixels)==0);
        REQUIRE(memcmp(old,&old_value,sizeof old_value)==0&&dynamic_warnings==warnings+1);
    }
    REQUIRE(strstr(last_dynamic_warning,"phase=dynamic_upload")&&strstr(last_dynamic_warning,"cp=U+00004E01"));
    fail_update=false;const GlyphInfo *next=font_render_dynamic_glyph(font,0x4e01);
    REQUIRE(next&&font->dyn_count==2&&font->dyn_pen_x==old_x+3&&dynamic_warnings==warnings+1);
    REQUIRE(memcmp(font->dyn_pixels,gpu_pixels,sizeof gpu_pixels)==0);
    REQUIRE(memcmp(old,&old_value,sizeof old_value)==0);jce_font_close(font);
'''

BLOB_SHARED = r'''
    unsigned char input[16];memset(input,7,sizeof input);
    JceFont *a=jce_font_open_mem_ex(input,sizeof input,"mount-A/font.ttf",14,NULL,0);
    s_text.next_sdf_spread=8;
    JceFont *b=jce_font_open_mem_ex(input,sizeof input,"mount-B/font.ttf",28,NULL,0);
    REQUIRE(a&&b&&a!=b&&a->font_data==b->font_data);
    REQUIRE(a->ft_face!=b->ft_face&&a->hb_font!=b->hb_font&&a->atlas.idx!=b->atlas.idx);
    REQUIRE(a->ft_face->pixels==14&&b->ft_face->pixels==28&&a->sdf_spread==0&&b->sdf_spread==8);
    REQUIRE(s_text.references==2&&s_text.unique_bytes==sizeof input&&a->font_data->refs==2);
    uint64_t serial=a->font_data->serial;
    input[0]=9;
    REQUIRE(((unsigned char *)a->font_data->data)[0]==7);
    JceFont *c=jce_font_open_mem_ex(input,sizeof input,"mount-A/font.ttf",14,NULL,0);
    REQUIRE(c&&c->font_data!=a->font_data&&c->font_data->serial!=serial);
    jce_font_close(a);
    REQUIRE(s_text.references==2&&b->font_data->refs==1&&b->ft_face->live);
    jce_font_close(b);jce_font_close(c);
    REQUIRE(!s_text.blobs&&!s_text.references&&!s_text.unique_bytes);
    jce_text_shutdown();jce_text_shutdown();
    a=jce_font_open_mem_ex(input,sizeof input,"mount-A/font.ttf",14,NULL,0);
    REQUIRE(a&&a->font_data->serial>serial);jce_font_close(a);jce_text_shutdown();
    unsigned before=scopes,counters=trace_counters;trace_on=false;
    a=jce_font_open_mem_ex(input,sizeof input,"trace-off",14,NULL,0);
    REQUIRE(a);jce_font_close(a);jce_text_shutdown();
    REQUIRE(scopes==before&&trace_counters==counters);
'''
BLOB_COLLISION = r'''
    unsigned char input[16];memset(input,1,sizeof input);force_hash_collision=true;
    JceFont *a=jce_font_open_mem_ex(input,sizeof input,"same/path.ttf",14,NULL,0);
    memset(input,2,sizeof input);
    JceFont *b=jce_font_open_mem_ex(input,sizeof input,"same/path.ttf",14,NULL,0);
    REQUIRE(a&&b&&a->font_data!=b->font_data&&s_text.unique_bytes==32);
    REQUIRE(((unsigned char *)a->font_data->data)[0]==1&&((unsigned char *)b->font_data->data)[0]==2);
    jce_font_close(b);jce_font_close(a);
'''
BLOB_FAILURES = r'''
    unsigned char input[16];memset(input,7,sizeof input);
    fail_calloc_at=1;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"meta-oom",14,NULL,0)&&!owned_allocs);
    fail_calloc_at=0;fail_face=true;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"face-fail",14,NULL,0)&&!owned_allocs);
    fail_face=false;JceFont *a=jce_font_open_mem_ex(input,sizeof input,"live",14,NULL,0);REQUIRE(a);
    unsigned baseline=owned_allocs;uint64_t serial=a->font_data->serial;
    fail_calloc_at=calloc_calls+1;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"font-oom",18,NULL,0));
    REQUIRE(owned_allocs==baseline&&s_text.references==1&&a->font_data->refs==1);
    fail_calloc_at=0;fail_face=true;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"face-fail-hit",18,NULL,0));
    REQUIRE(owned_allocs==baseline&&s_text.references==1);fail_face=false;
    s_text.references=INT64_MAX;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"ref-overflow",18,NULL,0));
    s_text.references=1;REQUIRE(a->font_data->refs==1&&owned_allocs==baseline);
    s_text.next_serial=INT64_MAX;input[0]=4;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"serial-overflow",18,NULL,0));
    REQUIRE(owned_allocs==baseline&&s_text.references==1);s_text.next_serial=serial;
    s_text.unique_bytes=INT64_MAX;
    REQUIRE(!jce_font_open_mem_ex(input,sizeof input,"bytes-overflow",18,NULL,0));
    REQUIRE(owned_allocs==baseline&&s_text.references==1);s_text.unique_bytes=sizeof input;
    REQUIRE(!jce_font_open_mem_ex(NULL,sizeof input,"missing",18,NULL,0));
    REQUIRE(!jce_font_open_mem_ex(input,0,"empty",18,NULL,0));
    jce_font_close(a);
    REQUIRE(!s_text.blobs&&!s_text.references&&!s_text.unique_bytes);
'''


class FontAtlasFailureTests(unittest.TestCase):
    def test_static_bitmap_oom_fails_actual_font_open_then_retry_succeeds(self):
        execute(self, font_probe(STATIC))

    def test_dynamic_pixels_map_and_texture_failures_retry_and_diagnose_once(self):
        execute(self, font_probe(INIT))

    def test_dynamic_upload_rejection_rolls_back_and_preserves_accepted_glyph(self):
        execute(self, font_probe(UPDATE))

    def test_negative_previous_silent_bitmap_oom_cannot_publish_font(self):
        execute(RuntimeFailure(self), font_probe(STATIC, 'static_bitmap'))

    def test_negative_unchecked_dynamic_texture_creation_cannot_publish_state(self):
        execute(RuntimeFailure(self), font_probe(INIT, 'dynamic_admission'))

    def test_negative_ignored_dynamic_update_cannot_publish_glyph(self):
        execute(RuntimeFailure(self), font_probe(UPDATE, 'dynamic_update'))

    def test_blob_shared_bytes_keep_independent_faces_and_release_last_reference(self):
        execute(self, font_probe(BLOB_SHARED))

    def test_blob_hash_collision_and_same_label_different_bytes_do_not_alias(self):
        execute(self, font_probe(BLOB_COLLISION))

    def test_blob_partial_failures_and_overflow_preserve_existing_owner(self):
        execute(self, font_probe(BLOB_FAILURES))

    def test_negative_blob_hash_without_byte_comparison_fails_at_runtime(self):
        execute(self, font_probe(BLOB_COLLISION, 'blob_compare'),
                expected_runtime_failure='owner constraint failed')

    def test_negative_font_close_without_blob_release_fails_at_runtime(self):
        execute(self, font_probe(BLOB_SHARED, 'blob_release'),
                expected_runtime_failure='owner constraint failed')


if __name__ == '__main__':
    unittest.main()
