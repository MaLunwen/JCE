/* Actual texture TU, AssetWriter/Pak/reader and core log/trace. Only GPU
 * capabilities/allocation/creation are counted ports. CPU upload bytes are
 * observed without creating a device. */
#include "unity.h"
#include <jce/renderer/jce_texture.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/resource/jce_archive_writer.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_trace.h>
#include <bgfx/c99/bgfx.h>
#include "resource/jce_asset_writer_internal.h"
#include "resource/jce_tex_compress.h"
#include <stdio.h>
#include <string.h>

static bgfx_caps_t caps;
static bgfx_memory_t memory;
static uint8_t staging[1024];
static bool absent_caps,fail_alloc,fail_create;
static unsigned alloc_calls,create_calls,destroy_calls,warning_calls;
static uint32_t created_format,created_width,created_height;
static uint64_t created_flags;
static bool created_mips, legacy_sentinel;
static char warning[768];
static JceAssetWriteResult encoded;
static void *archive_bytes;
static JcePakArchive *pak;
static JceTexture result;
static const uint64_t source_hash=UINT64_C(0xfedcba9876543210);
static const bgfx_caps_t *controlled_caps(void) {return absent_caps ? NULL : &caps;}
static bgfx_renderer_type_t controlled_backend(void) {return BGFX_RENDERER_TYPE_OPENGLES;}
static const bgfx_memory_t *controlled_alloc(uint32_t bytes)
{
    ++alloc_calls;
    if(fail_alloc)return NULL;
    TEST_ASSERT_TRUE(bytes<=sizeof staging);memory.data=staging;memory.size=bytes;
    return &memory;
}
static bgfx_texture_handle_t controlled_create(uint16_t width,uint16_t height,
    bool mips,uint16_t layers,bgfx_texture_format_t format,uint64_t flags,
    const bgfx_memory_t *data,uint64_t external)
{
    (void)external;
    TEST_ASSERT_EQUAL_UINT16(1,layers);TEST_ASSERT_EQUAL_PTR(&memory,data);
    ++create_calls;created_format=format;created_width=width;created_height=height;
    created_flags=flags;created_mips=mips;
    if(legacy_sentinel)TEST_ASSERT_EQUAL_UINT8(0x5a,data->data[0]);
    return (bgfx_texture_handle_t){fail_create ? UINT16_MAX : 0};
}
static void controlled_destroy(bgfx_texture_handle_t texture)
{TEST_ASSERT_EQUAL_UINT16(0,texture.idx);++destroy_calls;}
#define bgfx_get_caps controlled_caps
#define bgfx_get_renderer_type controlled_backend
#define bgfx_alloc controlled_alloc
#define bgfx_create_texture_2d controlled_create
#define bgfx_destroy_texture controlled_destroy
#ifndef JCE_TEST_TEXTURE_FAILURE_OWNER
#define JCE_TEST_TEXTURE_FAILURE_OWNER "renderer/jce_texture.c"
#endif
#include JCE_TEST_TEXTURE_FAILURE_OWNER

static void observe_log(const JceLogRecord *record,void *user)
{
    (void)user;
    if(strcmp(record->tag,"jce_texture")!=0 ||
       strstr(record->message,"texture request rejected")==NULL)return;
    ++warning_calls;snprintf(warning,sizeof warning,"%s",record->message);
}
void setUp(void)
{
    memset(&caps,0,sizeof caps);caps.formats[BGFX_TEXTURE_FORMAT_RGBA8]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    caps.limits.maxTextureSize=8192;
    absent_caps=fail_alloc=fail_create=legacy_sentinel=false;
    created_format=created_width=created_height=0;created_flags=0;created_mips=false;
    memset(staging,0xcd,sizeof staging);
    jce_texture_set_colour_space(1);
    alloc_calls=create_calls=destroy_calls=warning_calls=0;
    warning[0]=0;encoded=(JceAssetWriteResult){0};archive_bytes=NULL;pak=NULL;
    result=JCE_TEXTURE_INVALID;
    jce_trace_set_enabled(false);jce_trace_reset();jce_log_set_sink(observe_log,NULL);
}
void tearDown(void)
{
    jce_texture_destroy(result);jce_pak_close(pak);jce_free(archive_bytes);jce_free(encoded.data);
    jce_log_set_sink(NULL,NULL);jce_trace_set_enabled(false);
}
static JceAssetTexInfo info_for(uint32_t format)
{JceAssetTexInfo info={4,4,format,1,0,0};return info;}
static void encode_texture_payload(const JceAssetTexInfo *info,uint32_t type,
    size_t info_bytes,const void *pixels,size_t pixel_bytes,bool compressed)
{
    JceAssetWriteChunk chunks[2]={{JCEASSET_CHUNK_TEX_INFO,info,info_bytes},
        {JCEASSET_CHUNK_TEX_PIXELS,pixels,pixel_bytes}};
    encoded=jce_asset_writer_build(type,source_hash,chunks,2,compressed ? 3 : 0);
    TEST_ASSERT_TRUE_MESSAGE(encoded.success,encoded.error);
}
static void make_encoded(uint32_t format,uint32_t type,size_t info_bytes,bool compressed)
{
    JceAssetTexInfo info=info_for(format);uint8_t pixels[64];memset(pixels,0x5a,sizeof pixels);
    const size_t pixel_bytes=jce_tex_cooked_pixel_size(4,4,(int)format,1);
    legacy_sentinel=true;
    encode_texture_payload(&info,type,info_bytes,pixels,pixel_bytes,compressed);
}
static JceTextureCpu *decode_memory(void)
{return jce_texture_decode_cpu_mem(encoded.data,encoded.size,JCE_TEX_CLAMP);}
static JceTextureCpu *decode_pak(void)
{
    JceArchiveWriter *writer=jce_archive_writer_create(NULL);TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_TRUE(jce_archive_writer_add(writer,"ui/widget.png",encoded.data,encoded.size));
    size_t bytes=0;TEST_ASSERT_TRUE(jce_archive_writer_finish(writer,&archive_bytes,&bytes));
    jce_archive_writer_destroy(writer);pak=jce_pak_open(archive_bytes,bytes);TEST_ASSERT_NOT_NULL(pak);
    return jce_texture_decode_cpu(pak,"ui/widget.png",JCE_TEX_CLAMP);
}
static void expect_stage(const char *name)
{TEST_ASSERT_EQUAL_UINT32(1,warning_calls);TEST_ASSERT_NOT_NULL(strstr(warning,name));}
static void test_portable_rgba8_real_cooked_chunks_upload_valid_zero_handle(void)
{
    make_encoded(JCEASSET_TEXFMT_RGBA8,JCEASSET_TYPE_TEXTURE,sizeof(JceAssetTexInfo),true);
    JceTextureCpu *cpu=decode_pak();TEST_ASSERT_NOT_NULL(cpu);
    result=jce_texture_upload_cpu(cpu);TEST_ASSERT_TRUE(jce_texture_valid(result));
    TEST_ASSERT_EQUAL_UINT16(0,result.idx);TEST_ASSERT_EQUAL_UINT32(1,create_calls);
    TEST_ASSERT_EQUAL_UINT32(BGFX_TEXTURE_FORMAT_RGBA8,created_format);
    TEST_ASSERT_EQUAL_UINT32(4,created_width);TEST_ASSERT_EQUAL_UINT32(4,created_height);
    TEST_ASSERT_EQUAL_UINT32(0,warning_calls);
}
static void test_astc_cap_refusal_keeps_pak_asset_identity_and_does_not_allocate(void)
{
    make_encoded(JCEASSET_TEXFMT_ASTC_4x4,JCEASSET_TYPE_TEXTURE,sizeof(JceAssetTexInfo),true);
    JceTextureCpu *cpu=decode_pak();TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(cpu)));
    expect_stage("stage=upload_caps");TEST_ASSERT_NOT_NULL(strstr(warning,"cooked_format=5"));
    TEST_ASSERT_NOT_NULL(strstr(warning,"cap_observed=1 format_caps=0000"));
    TEST_ASSERT_NOT_NULL(strstr(warning,"source_hash=fedcba9876543210"));
    char hash[40];snprintf(hash,sizeof hash,"path_hash=%08x",(unsigned)jce_fnv1a32_str("ui/widget.png"));
    TEST_ASSERT_NOT_NULL(strstr(warning,hash));
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);TEST_ASSERT_EQUAL_UINT32(0,create_calls);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(NULL)));
    TEST_ASSERT_EQUAL_UINT32(1,warning_calls); /* No object/request, no polling noise. */
}
static void test_explicit_supported_astc_is_preserved_without_rgba_reinterpretation(void)
{
    caps.formats[BGFX_TEXTURE_FORMAT_ASTC4X4]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    make_encoded(JCEASSET_TEXFMT_ASTC_4x4,JCEASSET_TYPE_TEXTURE,sizeof(JceAssetTexInfo),false);
    JceTextureCpu *cpu=decode_memory();TEST_ASSERT_NOT_NULL(cpu);result=jce_texture_upload_cpu(cpu);
    TEST_ASSERT_TRUE(jce_texture_valid(result));TEST_ASSERT_EQUAL_UINT32(BGFX_TEXTURE_FORMAT_ASTC4X4,created_format);
    TEST_ASSERT_EQUAL_UINT32(16,memory.size);TEST_ASSERT_EQUAL_UINT32(0,warning_calls);
}
static void test_absent_caps_are_unavailable_and_not_reported_as_observed_zero(void)
{
    absent_caps=true;make_encoded(0,0,sizeof(JceAssetTexInfo),false);
    JceTextureCpu *cpu=decode_memory();TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(cpu)));
    expect_stage("stage=upload_caps");TEST_ASSERT_NOT_NULL(strstr(warning,"backend_raw=-1 cap_observed=0"));
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
}
static void test_bad_header_refuses_before_gpu_and_metadata_copy(void)
{
    make_encoded(0,0,sizeof(JceAssetTexInfo),false);
    ((JceAssetFileHeader *)encoded.data)->version=99;
    TEST_ASSERT_NULL(decode_memory());expect_stage("stage=cooked_header");
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
}
static void test_wrong_asset_kind_and_short_info_are_decoder_refusals(void)
{
    make_encoded(0,JCEASSET_TYPE_SOUND,8,false);
    TEST_ASSERT_NULL(decode_memory());expect_stage("stage=cooked_chunks");
    ((JceAssetFileHeader *)encoded.data)->asset_type=JCEASSET_TYPE_TEXTURE;
    warning_calls=0;TEST_ASSERT_NULL(decode_memory());expect_stage("stage=cooked_info");
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
}
static void test_bad_pixel_frame_has_specific_decoder_identity(void)
{
    make_encoded(0,0,sizeof(JceAssetTexInfo),false);
    JceAssetView view;TEST_ASSERT_TRUE(jce_asset_open(&view,encoded.data,encoded.size));
    JceAssetChunkEntry *pixels=(JceAssetChunkEntry *)jce_asset_find_chunk(&view,JCEASSET_CHUNK_TEX_PIXELS);
    TEST_ASSERT_NOT_NULL(pixels);
    pixels->compression=JCEASSET_COMPRESS_ZSTD; /* Existing bytes are not a ZSTD frame. */
    TEST_ASSERT_NULL(decode_memory());expect_stage("stage=cooked_pixels");
    TEST_ASSERT_NOT_NULL(strstr(warning,"source_hash=fedcba9876543210"));
    pixels->original_size=0;warning_calls=0;
    TEST_ASSERT_NULL(decode_memory());expect_stage("stage=cooked_pixels");
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
}
static void test_layout_and_unknown_format_refuse_before_backend_allocation(void)
{
    uint8_t pixels[64];memset(pixels,0x5a,sizeof pixels);JceAssetTexInfo info=info_for(0);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_from_cooked(&info,pixels,63,JCE_TEX_CLAMP)));
    expect_stage("stage=upload_layout");TEST_ASSERT_NOT_NULL(strstr(warning,"expected=64"));
    info.format=999;warning_calls=0;
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_from_cooked(&info,pixels,64,JCE_TEX_CLAMP)));
    expect_stage("stage=upload_format");TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
}
static void test_upload_allocation_refusal_is_not_a_backend_create(void)
{
    fail_alloc=true;make_encoded(0,0,sizeof(JceAssetTexInfo),false);
    JceTextureCpu *cpu=decode_memory();TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(cpu)));expect_stage("stage=upload_alloc");
    TEST_ASSERT_EQUAL_UINT32(1,alloc_calls);TEST_ASSERT_EQUAL_UINT32(0,create_calls);
}
static void test_backend_create_refusal_has_distinct_stage_and_source(void)
{
    fail_create=true;make_encoded(0,0,sizeof(JceAssetTexInfo),false);
    JceTextureCpu *cpu=decode_memory();TEST_ASSERT_NOT_NULL(cpu);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(cpu)));expect_stage("stage=upload_create");
    TEST_ASSERT_NOT_NULL(strstr(warning,"source_hash=fedcba9876543210"));
    TEST_ASSERT_EQUAL_UINT32(1,alloc_calls);TEST_ASSERT_EQUAL_UINT32(1,create_calls);
}
static void test_trace_keeps_integer_identity_stage_caps_and_failed_closure(void)
{
    make_encoded(5,0,sizeof(JceAssetTexInfo),false);
    JceTextureCpu *cpu=decode_pak();TEST_ASSERT_NOT_NULL(cpu);
    jce_trace_reset();jce_trace_set_enabled(true);
    TEST_ASSERT_FALSE(jce_texture_valid(jce_texture_upload_cpu(cpu)));
    JceTraceCursor cursor;JceTraceEvent events[64];jce_trace_cursor_init(&cursor);
    uint32_t count=jce_trace_read(&cursor,events,64);unsigned seen=0;bool end=false;
    for(uint32_t i=0;i<count;++i) {
        if(events[i].type==JCE_TRACE_EVENT_COUNTER) {
            if(strcmp(events[i].name,"texture.reject.source_hash_hi")==0) {TEST_ASSERT_EQUAL_UINT64(UINT32_C(0xfedcba98),events[i].value_u64);++seen;}
            if(strcmp(events[i].name,"texture.reject.source_hash_lo")==0) {TEST_ASSERT_EQUAL_UINT64(UINT32_C(0x76543210),events[i].value_u64);++seen;}
            if(strcmp(events[i].name,"texture.reject.stage")==0) {TEST_ASSERT_EQUAL_UINT64(JCE_TEXTURE_FAIL_CAPS,events[i].value_u64);++seen;}
            if(strcmp(events[i].name,"texture.reject.cap_observed")==0) {TEST_ASSERT_EQUAL_UINT64(1,events[i].value_u64);++seen;}
        }
        if(events[i].type==JCE_TRACE_EVENT_TASK_END && strcmp(events[i].name,"texture.request.reject")==0) {
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED,events[i].value_u32[0]);end=true;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4,seen);TEST_ASSERT_TRUE(end);
    JceTraceStats stats;jce_trace_get_stats(&stats);TEST_ASSERT_EQUAL_UINT32(0,stats.active_work_items);
}
/* BC3 color endpoint RGB565 0xfaa2 and alpha selectors 0, 1, 0... */
static const uint8_t bc3_block[16] = {
    61,149,0x08,0,0,0,0,0, 0xa2,0xfa,0,0,0,0,0,0
};
/* Distinct BC5 R and G endpoints. All selectors are zero. */
static const uint8_t bc5_block[16] = {
    29,0,0,0,0,0,0,0, 197,0,0,0,0,0,0,0
};
/* 5x3 has red and blue blocks side by side. The 2x1 and 1x1 tails are green
 * and yellow. Distinct alpha values also detect wrong mip offsets. */
static const uint8_t bc3_npot_chain[64] = {
     47,0,0,0,0,0,0,0, 0x00,0xf8,0,0,0,0,0,0,
     95,0,0,0,0,0,0,0, 0x1f,0x00,0,0,0,0,0,0,
    143,0,0,0,0,0,0,0, 0xe0,0x07,0,0,0,0,0,0,
    191,0,0,0,0,0,0,0, 0xe0,0xff,0,0,0,0,0,0
};

static void expect_block_upload(bgfx_texture_format_t format, uint32_t width,
                                uint32_t height, uint32_t mips, uint32_t bytes)
{
    TEST_ASSERT_TRUE(jce_texture_valid(result));
    TEST_ASSERT_EQUAL_UINT32(1,alloc_calls);
    TEST_ASSERT_EQUAL_UINT32(1,create_calls);
    TEST_ASSERT_EQUAL_UINT32(format,created_format);
    TEST_ASSERT_EQUAL_UINT32(format,jce_texture_get_format(result));
    TEST_ASSERT_EQUAL_UINT32(width,created_width);
    TEST_ASSERT_EQUAL_UINT32(height,created_height);
    TEST_ASSERT_EQUAL_UINT32(mips,jce_texture_get_mips(result));
    TEST_ASSERT_EQUAL_INT(mips > 1,created_mips);
    TEST_ASSERT_EQUAL_UINT32(bytes,memory.size);
    TEST_ASSERT_EQUAL_UINT32(0,warning_calls);
}

static void expect_native_block(uint32_t cooked, bgfx_texture_format_t format,
                                const uint8_t block[16], int sampler)
{
    const JceAssetTexInfo info=info_for(cooked);
    caps.formats[format]=BGFX_CAPS_FORMAT_TEXTURE_2D |
        BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED | BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB;
    result=jce_texture_from_cooked(&info,block,16,sampler);
    expect_block_upload(format,4,4,1,16);
    TEST_ASSERT_EQUAL_MEMORY(block,staging,16);
    TEST_ASSERT_EQUAL_INT((sampler & JCE_TEX_SRGB) != 0,
                         (created_flags & BGFX_TEXTURE_SRGB) != 0);
}
static void test_native_bc3_preserves_block_bytes_and_srgb(void)
{
    expect_native_block(JCEASSET_TEXFMT_BC3,BGFX_TEXTURE_FORMAT_BC3,
                        bc3_block,JCE_TEX_CLAMP | JCE_TEX_SRGB);
}
static void test_native_bc5_preserves_block_bytes(void)
{
    expect_native_block(JCEASSET_TEXFMT_BC5,BGFX_TEXTURE_FORMAT_BC5,
                        bc5_block,JCE_TEX_CLAMP);
}

static void test_emulated_bc3_uploads_asymmetric_rgba_and_preserves_srgb(void)
{
    const JceAssetTexInfo info=info_for(JCEASSET_TEXFMT_BC3);
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA8]|=BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB;
    encode_texture_payload(&info,JCEASSET_TYPE_TEXTURE,sizeof info,bc3_block,
                           sizeof bc3_block,true);
    JceTextureCpu *cpu=jce_texture_decode_cpu_mem(encoded.data,encoded.size,
                                                 JCE_TEX_CLAMP | JCE_TEX_SRGB);
    TEST_ASSERT_NOT_NULL(cpu);
    result=jce_texture_upload_cpu(cpu);
    expect_block_upload(BGFX_TEXTURE_FORMAT_RGBA8,4,4,1,64);
    TEST_ASSERT_TRUE((created_flags & BGFX_TEXTURE_SRGB) != 0);
    for (unsigned i=0;i<16;++i) {
        const uint8_t rgba[]={255,85,17,i == 1 ? 149 : 61};
        TEST_ASSERT_EQUAL_MEMORY(rgba,staging + i*4,sizeof rgba);
    }
}
static void test_emulated_bc5_preserves_red_and_green_channels(void)
{
    const JceAssetTexInfo info=info_for(JCEASSET_TEXFMT_BC5);
    const uint8_t rgba[]={29,197,0,255};
    caps.formats[BGFX_TEXTURE_FORMAT_BC5]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    result=jce_texture_from_cooked(&info,bc5_block,sizeof bc5_block,JCE_TEX_CLAMP);
    expect_block_upload(BGFX_TEXTURE_FORMAT_RGBA8,4,4,1,64);
    TEST_ASSERT_FALSE((created_flags & BGFX_TEXTURE_SRGB) != 0);
    for (unsigned i=0;i<16;++i)
        TEST_ASSERT_EQUAL_MEMORY(rgba,staging + i*4,sizeof rgba);
}

static void test_emulated_bc3_crops_block_edges_and_uploads_tail_mips(void)
{
    const JceAssetTexInfo info={5,3,JCEASSET_TEXFMT_BC3,3,0,0};
    const uint8_t red[]={255,0,0,47}, blue[]={0,0,255,95};
    const uint8_t tail[]={0,255,0,143, 0,255,0,143, 255,255,0,191};
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    result=jce_texture_from_cooked(&info,bc3_npot_chain,sizeof bc3_npot_chain,
                                   JCE_TEX_CLAMP);
    expect_block_upload(BGFX_TEXTURE_FORMAT_RGBA8,5,3,3,72);
    for (unsigned y=0;y<3;++y)
        for (unsigned x=0;x<5;++x)
            TEST_ASSERT_EQUAL_MEMORY(x == 4 ? blue : red,staging + (y*5+x)*4,4);
    TEST_ASSERT_EQUAL_MEMORY(tail,staging + 60,sizeof tail);
}

static void expect_no_backend_upload(void)
{
    TEST_ASSERT_FALSE(jce_texture_valid(result));
    TEST_ASSERT_EQUAL_UINT32(0,alloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0,create_calls);
}
static void test_emulated_bc3_requires_native_rgba8(void)
{
    const JceAssetTexInfo info=info_for(JCEASSET_TEXFMT_BC3);
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA8]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    result=jce_texture_from_cooked(&info,bc3_block,sizeof bc3_block,JCE_TEX_CLAMP);
    expect_no_backend_upload();
}
static void test_emulated_bc3_rejects_partial_mip_chain(void)
{
    const JceAssetTexInfo info={5,3,JCEASSET_TEXFMT_BC3,2,0,0};
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    /* 48 is the exact encoded size of the declared base and first tail. */
    result=jce_texture_from_cooked(&info,bc3_npot_chain,48,JCE_TEX_CLAMP);
    expect_no_backend_upload();
}
static void test_emulated_bc3_respects_device_extent_limit(void)
{
    const JceAssetTexInfo info={5,3,JCEASSET_TEXFMT_BC3,1,0,0};
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    caps.limits.maxTextureSize=4;
    result=jce_texture_from_cooked(&info,bc3_npot_chain,32,JCE_TEX_CLAMP);
    expect_no_backend_upload();
}
static void test_emulated_bc3_rejects_decoded_byte_budget_before_gpu_allocation(void)
{
    const JceAssetTexInfo info={4097,4096,JCEASSET_TEXFMT_BC3,1,0,0};
    const size_t bytes=1025u*1024u*16u;
    uint8_t *blocks=jce_malloc(bytes);
    TEST_ASSERT_NOT_NULL(blocks);
    memset(blocks,0,bytes);
    caps.formats[BGFX_TEXTURE_FORMAT_BC3]=BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED;
    result=jce_texture_from_cooked(&info,blocks,bytes,JCE_TEX_CLAMP);
    jce_free(blocks);
    expect_no_backend_upload();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_portable_rgba8_real_cooked_chunks_upload_valid_zero_handle);
    RUN_TEST(test_astc_cap_refusal_keeps_pak_asset_identity_and_does_not_allocate);
    RUN_TEST(test_explicit_supported_astc_is_preserved_without_rgba_reinterpretation);
    RUN_TEST(test_absent_caps_are_unavailable_and_not_reported_as_observed_zero);
    RUN_TEST(test_bad_header_refuses_before_gpu_and_metadata_copy);
    RUN_TEST(test_wrong_asset_kind_and_short_info_are_decoder_refusals);
    RUN_TEST(test_bad_pixel_frame_has_specific_decoder_identity);
    RUN_TEST(test_layout_and_unknown_format_refuse_before_backend_allocation);
    RUN_TEST(test_upload_allocation_refusal_is_not_a_backend_create);
    RUN_TEST(test_backend_create_refusal_has_distinct_stage_and_source);
    RUN_TEST(test_trace_keeps_integer_identity_stage_caps_and_failed_closure);
    RUN_TEST(test_native_bc3_preserves_block_bytes_and_srgb);
    RUN_TEST(test_native_bc5_preserves_block_bytes);
    RUN_TEST(test_emulated_bc3_uploads_asymmetric_rgba_and_preserves_srgb);
    RUN_TEST(test_emulated_bc5_preserves_red_and_green_channels);
    RUN_TEST(test_emulated_bc3_crops_block_edges_and_uploads_tail_mips);
    RUN_TEST(test_emulated_bc3_requires_native_rgba8);
    RUN_TEST(test_emulated_bc3_rejects_partial_mip_chain);
    RUN_TEST(test_emulated_bc3_respects_device_extent_limit);
    RUN_TEST(test_emulated_bc3_rejects_decoded_byte_budget_before_gpu_allocation);
    return UNITY_END();
}
