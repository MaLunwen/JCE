#include "unity.h"
#include <jce/api_core.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    unsigned closes, calls;
    size_t maximum;
    uint64_t available;
    bool short_reads, bad_count;
} Input;

static size_t read_pattern(void *user,uint64_t offset,void *out,size_t capacity)
{
    Input *input=user;
    size_t i;
    ++input->calls;
    if (capacity>input->maximum) input->maximum=capacity;
    if (input->bad_count) return capacity+1u;
    if (input->available) {
        if (offset>=input->available) return 0u;
        if (capacity>input->available-offset) capacity=(size_t)(input->available-offset);
    }
    if (input->short_reads && capacity>13u) capacity=13u;
    for (i=0u;i<capacity;++i) ((uint8_t *)out)[i]=(uint8_t)((offset+i)%251u);
    return capacity;
}
static void close_pattern(void *user) { ++((Input *)user)->closes; }

static void test_large_offsets_are_bounded_and_shared(void)
{
    Input input={0};
    JceReadSourceDesc desc={UINT64_C(5)*1024u*1024u*1024u,read_pattern,close_pattern,&input};
    JceReadSource *source=jce_read_source_create(&desc), *retained;
    uint8_t *bytes=jce_malloc(70000u);
    uint64_t offset=UINT64_C(3)*1024u*1024u*1024u+17u;
    JceReadSourceStats stats;
    size_t i;
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(bytes);
    retained=jce_read_source_acquire(source);
    jce_read_source_close(source);
    TEST_ASSERT_EQUAL_UINT(0u,input.closes);
    TEST_ASSERT_EQUAL_UINT(70000u,jce_read_source_read_at(retained,offset,bytes,70000u));
    for (i=0u;i<70000u;++i) TEST_ASSERT_EQUAL_UINT8((offset+i)%251u,bytes[i]);
    TEST_ASSERT_TRUE(jce_read_source_get_stats(retained,&stats));
    TEST_ASSERT_EQUAL_UINT64(desc.size,stats.size);
    TEST_ASSERT_LESS_THAN_UINT64(80u*1024u,stats.owned_bytes);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(65536u,input.maximum);
    TEST_ASSERT_EQUAL_UINT(2u,input.calls);
    TEST_ASSERT_EQUAL_UINT(3u,jce_read_source_read_at(retained,desc.size-3u,bytes,100u));
    TEST_ASSERT_EQUAL_UINT(0u,jce_read_source_read_at(retained,UINT64_MAX,bytes,100u));
    jce_read_source_close(retained);
    TEST_ASSERT_EQUAL_UINT(1u,input.closes);
    jce_free(bytes);
}

static void test_short_reads_and_invalid_callback_count(void)
{
    Input input={0};
    JceReadSourceDesc desc={100u,read_pattern,close_pattern,&input};
    JceReadSource *source;
    uint8_t bytes[100];
    input.short_reads=true;
    source=jce_read_source_create(&desc);
    TEST_ASSERT_EQUAL_UINT(100u,jce_read_source_read_at(source,0u,bytes,sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT8(99u,bytes[99]);
    jce_read_source_close(source);
    input.bad_count=true;
    source=jce_read_source_create(&desc);
    TEST_ASSERT_EQUAL_UINT(0u,jce_read_source_read_at(source,0u,bytes,sizeof(bytes)));
    jce_read_source_close(source);
    TEST_ASSERT_EQUAL_UINT(2u,input.closes);
    input.bad_count=false; input.available=15u;
    source=jce_read_source_create(&desc);
    TEST_ASSERT_EQUAL_UINT(15u,jce_read_source_read_at(source,0u,bytes,sizeof(bytes)));
    TEST_ASSERT_EQUAL_UINT(0u,jce_read_source_read_at(source,15u,bytes,1u));
    jce_read_source_close(source);
}

static void test_file_truncation_and_memory_copy_contract(void)
{
    const char *path="_ut_read_source.bin";
    char original[]="immutable input", out[16]={0};
    JceReadSource *source=jce_read_source_open_memory(original,sizeof(original),true);
    original[0]='X';
    TEST_ASSERT_EQUAL_UINT(sizeof(original),jce_read_source_read_at(source,0u,out,sizeof(out)));
    TEST_ASSERT_EQUAL_CHAR('i',out[0]);
    jce_read_source_close(source);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path,original,sizeof(original)));
    source=jce_read_source_open_file(path);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_UINT(sizeof(original),jce_read_source_read_at(source,0u,out,sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0u,jce_read_source_read_at(source,sizeof(original),out,1u));
    jce_read_source_close(source);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_large_offsets_are_bounded_and_shared);
    RUN_TEST(test_short_reads_and_invalid_callback_count);
    RUN_TEST(test_file_truncation_and_memory_copy_contract);
    return UNITY_END();
}
