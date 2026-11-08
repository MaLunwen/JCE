/* Exact texture owner + counted backend allocation/create/destroy ports.
 * Real registry and public intent/query/remove paths; no device or GPU. */
#include "unity.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_trace.h>
#include "os/core/jce_memory.h"
#include <bgfx/c99/bgfx.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static unsigned staging_calls, creates, destroys, cpu_allocations, source_frees;
static uint16_t next_handle;
static bool fail_staging, fail_create, live[32];
static void *watched_source[3];
static unsigned watched_count;
static void *record_alloc(size_t size)
{ ++cpu_allocations; return jce__malloc_tracked(size); }
static void record_free(void *ptr)
{
    for (unsigned i=0; i<watched_count; ++i)
        if (ptr && ptr==watched_source[i]) { ++source_frees; watched_source[i]=NULL; }
    jce__free_tracked(ptr);
}
static const bgfx_memory_t *record_staging(uint32_t bytes)
{
    ++staging_calls;
    if (fail_staging) return NULL;
    bgfx_memory_t *m=(bgfx_memory_t *)jce_malloc(sizeof(*m)+bytes);
    TEST_ASSERT_NOT_NULL(m); m->data=(uint8_t *)(m+1); m->size=bytes;
    return m;
}
static bgfx_texture_handle_t record_create(uint16_t width, uint16_t height,
    bool mips, uint16_t layers, bgfx_texture_format_t format,
    uint64_t flags, const bgfx_memory_t *memory, uint64_t external)
{
    (void)flags; (void)external; ++creates;
    TEST_ASSERT_TRUE(width>0 && height>0); TEST_ASSERT_TRUE(mips);
    TEST_ASSERT_EQUAL_UINT16(1,layers); TEST_ASSERT_EQUAL_INT(BGFX_TEXTURE_FORMAT_RGBA8,format);
    TEST_ASSERT_NOT_NULL(memory); jce_free((void *)memory);
    if (fail_create) return (bgfx_texture_handle_t){UINT16_MAX};
    TEST_ASSERT_LESS_THAN_UINT16(32,next_handle);
    const uint16_t idx=next_handle++; TEST_ASSERT_FALSE(live[idx]); live[idx]=true;
    return (bgfx_texture_handle_t){idx};
}
static void record_destroy(bgfx_texture_handle_t handle)
{
    TEST_ASSERT_LESS_THAN_UINT16(32,handle.idx); TEST_ASSERT_TRUE(live[handle.idx]);
    live[handle.idx]=false; ++destroys;
}
#define bgfx_alloc record_staging
#define bgfx_create_texture_2d record_create
#define bgfx_destroy_texture record_destroy
#define jce__malloc_tracked record_alloc
#define jce__free_tracked record_free
#include "renderer/jce_texture.c"
#undef bgfx_alloc
#undef bgfx_create_texture_2d
#undef bgfx_destroy_texture
#undef jce__malloc_tracked
#undef jce__free_tracked
static const uint8_t pixels[8*8*4]={255,0,0,255};
static JceTexture create(bool source)
{
    JceTexture tex=jce_texture_from_rgba_ex(pixels,8,8,JCE_TEX_CLAMP);
    TEST_ASSERT_TRUE(jce_texture_valid(tex));
    if (source) {
        jce_texture_set_streaming_uploads(true);
        registry_opt_in_streaming(tex.idx,8,8,pixels,JCE_TEX_CLAMP);
        TexEntry *e=registry_find(tex.idx); TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_NOT_NULL(e->source_pixels); TEST_ASSERT_LESS_THAN_UINT(3,watched_count);
        watched_source[watched_count++]=e->source_pixels;
    }
    return tex;
}
static void unchanged(JceTexture tex)
{
    uint32_t w=0,h=0; jce_texture_get_size(tex,&w,&h);
    TEST_ASSERT_EQUAL_UINT32(8,w); TEST_ASSERT_EQUAL_UINT32(8,h);
    TEST_ASSERT_EQUAL_UINT32(4,jce_texture_get_mips(tex));
    TEST_ASSERT_EQUAL_UINT8(0,jce_texture_get_resident_top_mip(tex));
    TEST_ASSERT_NOT_NULL(registry_find(tex.idx)); TEST_ASSERT_TRUE(live[tex.idx]);
}
void setUp(void)
{
    TEST_ASSERT_EQUAL_INT(0,s_count);
    staging_calls=creates=destroys=cpu_allocations=source_frees=0;
    next_handle=0; watched_count=0; fail_staging=fail_create=false;
    memset(live,0,sizeof live); memset(watched_source,0,sizeof watched_source);
    s_caps_floor_cached=0; s_global_mip_bias=0; s_global_mip_bias_set=false;
    s_texture_quality_bias=0; jce_texture_set_streaming_uploads(false);
    jce_trace_set_enabled(false); jce_trace_reset();
}
void tearDown(void)
{
    while(s_count) jce_texture_destroy((JceTexture){s_registry[0].idx});
    for(unsigned i=0;i<32;++i) TEST_ASSERT_FALSE(live[i]);
    for(unsigned i=0;i<watched_count;++i) TEST_ASSERT_NULL(watched_source[i]);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0,stats.active_work_items); TEST_ASSERT_EQUAL_UINT64(0,jce_trace_task_current_id());
    jce_trace_set_enabled(false);
}
static void test_source_backed_requests_preserve_raw_borrow_and_registry(void)
{
    JceTexture tex=create(true), material_copy=tex;
    const unsigned a=cpu_allocations,c=creates,s=staging_calls;
    void *source=registry_find(tex.idx)->source_pixels;
    jce_texture_set_mip_bias(tex,1); jce_texture_request_mip_residency(tex,255);
    TEST_ASSERT_EQUAL_INT8(1,jce_texture_get_mip_bias(tex));
    TEST_ASSERT_EQUAL_UINT8(1,registry_find(tex.idx)->desired_top_mip);
    TEST_ASSERT_EQUAL_PTR(source,registry_find(tex.idx)->source_pixels);
    unchanged(material_copy);
    TEST_ASSERT_EQUAL_UINT(a,cpu_allocations); TEST_ASSERT_EQUAL_UINT(c,creates);
    TEST_ASSERT_EQUAL_UINT(s,staging_calls); TEST_ASSERT_EQUAL_UINT(0,destroys);
    TEST_ASSERT_EQUAL_UINT(0,source_frees); jce_texture_destroy(tex);
    TEST_ASSERT_EQUAL_UINT(1,source_frees); TEST_ASSERT_NULL(registry_find(tex.idx));
}
static void test_global_bias_preserves_all_registered_and_untracked_handles(void)
{
    JceTexture a=create(true), b=create(true), untracked=create(false);
    const unsigned allocations=cpu_allocations,staging=staging_calls,created=creates;
    jce_texture_set_global_mip_bias(2);
    unchanged(a); unchanged(b); unchanged(untracked);
    TEST_ASSERT_EQUAL_INT8(2,jce_texture_get_global_mip_bias());
    TEST_ASSERT_FALSE(registry_find(untracked.idx)->replacement_warned);
    TEST_ASSERT_EQUAL_UINT(allocations,cpu_allocations);
    TEST_ASSERT_EQUAL_UINT(staging,staging_calls); TEST_ASSERT_EQUAL_UINT(created,creates);
    TEST_ASSERT_EQUAL_UINT(0,destroys); TEST_ASSERT_EQUAL_UINT(0,source_frees);
}
static void test_denied_request_never_enters_staging_or_creation_failure_ports(void)
{
    JceTexture tex=create(true); const unsigned staged=staging_calls,created=creates;
    fail_staging=true; fail_create=true;
    jce_texture_set_mip_bias(tex,1); jce_texture_set_global_mip_bias(3);
    unchanged(tex); TEST_ASSERT_EQUAL_UINT(staged,staging_calls);
    TEST_ASSERT_EQUAL_UINT(created,creates); TEST_ASSERT_EQUAL_UINT(0,destroys);
}
static void test_trace_reports_cancelled_once_per_capture_and_disabled_allocates_no_ids(void)
{
    JceTexture tex=create(true);
    TEST_ASSERT_EQUAL_UINT64(1,jce_trace_next_id());
    for(unsigned i=0;i<10;++i) jce_texture_set_mip_bias(tex,1);
    TEST_ASSERT_EQUAL_UINT64(2,jce_trace_next_id());
    jce_trace_reset(); jce_trace_set_enabled(true);
    for(unsigned i=0;i<10;++i) jce_texture_set_mip_bias(tex,1);
    JceTraceCursor cursor; jce_trace_cursor_init(&cursor);
    JceTraceEvent events[32]; const uint32_t n=jce_trace_read(&cursor,events,32);
    unsigned terminal=0,requested=0,resident=0;
    for(uint32_t i=0;i<n;++i) {
        if(!strcmp(events[i].name,"texture.residency.unsupported") && events[i].type==JCE_TRACE_EVENT_TASK_END) {
            ++terminal; TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_CANCELLED,events[i].value_u32[0]);
        }
        if(!strcmp(events[i].name,"texture.residency.requested_top_mip")) {++requested; TEST_ASSERT_EQUAL_UINT64(1,events[i].value_u64);}
        if(!strcmp(events[i].name,"texture.residency.resident_top_mip")) {++resident; TEST_ASSERT_EQUAL_UINT64(0,events[i].value_u64);}
    }
    TEST_ASSERT_EQUAL_UINT(1,terminal); TEST_ASSERT_EQUAL_UINT(1,requested); TEST_ASSERT_EQUAL_UINT(1,resident);
    TEST_ASSERT_EQUAL_UINT64(0,cursor.lost_events);
    jce_trace_set_enabled(false); jce_trace_reset(); jce_trace_set_enabled(true);
    jce_texture_set_mip_bias(tex,1); jce_trace_cursor_init(&cursor);
    terminal=0; const uint32_t later=jce_trace_read(&cursor,events,32);
    for(uint32_t i=0;i<later;++i)
        if(events[i].type==JCE_TRACE_EVENT_TASK_END && !strcmp(events[i].name,"texture.residency.unsupported")) ++terminal;
    TEST_ASSERT_EQUAL_UINT(1,terminal); unchanged(tex);
}
static void test_swap_remove_and_handle_reuse_preserve_remaining_source_owner(void)
{
    JceTexture first=create(true), second=create(true);
    jce_texture_set_mip_bias(first,1); jce_texture_set_mip_bias(second,1);
    jce_texture_destroy(first); TEST_ASSERT_NULL(registry_find(first.idx));
    unchanged(second); TEST_ASSERT_EQUAL_UINT(1,source_frees);
    next_handle=first.idx; JceTexture reused=create(false);
    unchanged(reused); TEST_ASSERT_FALSE(registry_find(reused.idx)->replacement_warned);
    unchanged(second); jce_texture_destroy(second); TEST_ASSERT_EQUAL_UINT(2,source_frees);
    unchanged(reused); jce_texture_destroy(reused); TEST_ASSERT_EQUAL_INT(0,s_count);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_source_backed_requests_preserve_raw_borrow_and_registry);
    RUN_TEST(test_global_bias_preserves_all_registered_and_untracked_handles);
    RUN_TEST(test_denied_request_never_enters_staging_or_creation_failure_ports);
    RUN_TEST(test_trace_reports_cancelled_once_per_capture_and_disabled_allocates_no_ids);
    RUN_TEST(test_swap_remove_and_handle_reuse_preserve_remaining_source_owner);
    return UNITY_END();
}
