/* Complete VFS owner with bounded typed PhysFS/provider/allocation ports.
 * No OS files, huge allocations, graphics or architecture/ABI certificate.
 * SIZE32 models only the included owner's length arithmetic and allocator port. */
#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include <physfs.h>
#include <stdint.h>
#include <string.h>

/* Give the actual full TU a private public-symbol namespace. Dependencies,
 * including public headers and all three provider slots, retain real types. */
#define jce_fs_set_pak_provider bounds_jce_fs_set_pak_provider
#define jce_fs_register_pak_provider_internal bounds_jce_fs_register_pak_provider_internal
#define jce_fs_get_pak_provider bounds_jce_fs_get_pak_provider
#define jce_fs_mounted_pak_count bounds_jce_fs_mounted_pak_count
#define jce_fs_read bounds_jce_fs_read
#define jce_fs_size bounds_jce_fs_size
#define jce_fs_exists bounds_jce_fs_exists
#define jce_fs_write bounds_jce_fs_write
#define jce_fs_set_active bounds_jce_fs_set_active
#define jce_fs_set_active_policy bounds_jce_fs_set_active_policy
#define jce_fs_create bounds_jce_fs_create
#define jce_fs_destroy bounds_jce_fs_destroy
#define jce_fs_mount_pak bounds_jce_fs_mount_pak
#define jce_fs_mount_pak_named bounds_jce_fs_mount_pak_named
#define jce_fs_unmount_pak_named bounds_jce_fs_unmount_pak_named
#define jce_fs_mount_dir bounds_jce_fs_mount_dir
#define jce_fs_open bounds_jce_fs_open
#define jce_fs_close bounds_jce_fs_close
#define jce_fs_read_all bounds_jce_fs_read_all
#define jce_fs_read_pak_bounded bounds_jce_fs_read_pak_bounded
#define jce_fs_read_all_bounded_internal bounds_jce_fs_read_all_bounded_internal
#define jce_fs_set_write_dir bounds_jce_fs_set_write_dir
#define jce_fs_open_write bounds_jce_fs_open_write
#define jce_fs_write_all bounds_jce_fs_write_all
#include <jce/os/core/jce_filesystem.h>
#include "os/core/jce_filesystem_read_internal.h"
#include "os/core/jce_filesystem_pak_internal.h"
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"

static const unsigned char payload[4] = {'A',0,'B',0xff};
static unsigned open_calls,close_calls,read_calls,alloc_calls,data_free_calls,decode_calls;
static bool loose,missing,reject_alloc,short_read,short_decode;
static PHYSFS_sint64 reported_length;
static uint64_t reported_asset_length,last_alloc,last_read;
static int file_token,pak_token,asset_token;
static void *data_allocations[8];

static int counted_is_init(void) { return loose ? 1 : 0; }
static PHYSFS_File *counted_open(const char *path)
{
    TEST_ASSERT_EQUAL_STRING("asset.bin",path); ++open_calls;
    return missing ? NULL : (PHYSFS_File *)&file_token;
}
static PHYSFS_sint64 counted_length(PHYSFS_File *file)
{ TEST_ASSERT_EQUAL_PTR(&file_token,file);return reported_length; }
static int counted_close(PHYSFS_File *file)
{ TEST_ASSERT_EQUAL_PTR(&file_token,file);++close_calls;return 1; }
static PHYSFS_sint64 counted_read(PHYSFS_File *file,void *buffer,PHYSFS_uint64 count)
{
    TEST_ASSERT_EQUAL_PTR(&file_token,file);++read_calls;last_read=count;
    if(count>sizeof(payload)) return 0; /* bounded port: reject oversized IO without touching memory */
    size_t amount=(size_t)count;
    if(short_read && amount) --amount;
    memcpy(buffer,payload,amount);return (PHYSFS_sint64)amount;
}
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
static void *counted_alloc(uint32_t amount)
#else
static void *counted_alloc(size_t amount)
#endif
{
    ++alloc_calls;last_alloc=amount;
    /* Refuse all enormous/zero requests, including an old narrowing wrap.
     * A negative must fail an ordinary assertion, never write out of bounds. */
    if(reject_alloc || !amount || amount>64) return NULL;
    void *data=jce__malloc_tracked((size_t)amount);
    if(data) {
        unsigned i;
        memset(data,0xa5,(size_t)amount);
        for(i=0;i<8 && data_allocations[i];++i) {}
        TEST_ASSERT_LESS_THAN_UINT(8,i);data_allocations[i]=data;
    }
    return data;
}
static void counted_free(void *data)
{
    unsigned i;
    for(i=0;i<8;++i) if(data && data_allocations[i]==data) {
        data_allocations[i]=NULL;++data_free_calls;break;
    }
    jce__free_tracked(data);
}
static const void *JCE_CALL provider_find(const JcePakArchive *pak,const char *path)
{
    TEST_ASSERT_EQUAL_PTR(&pak_token,pak);TEST_ASSERT_EQUAL_STRING("asset.bin",path);
    return missing ? NULL : &asset_token;
}
static uint64_t JCE_CALL provider_size(const void *asset)
{ TEST_ASSERT_EQUAL_PTR(&asset_token,asset);return reported_asset_length; }
static size_t JCE_CALL provider_decode(const void *asset,void *buffer,size_t capacity)
{
    TEST_ASSERT_EQUAL_PTR(&asset_token,asset);++decode_calls;
    if(capacity!=sizeof(payload)) return 0; /* preserve ordinary refusal, never copy into a wrapped capacity */
    size_t amount=short_decode ? sizeof(payload)-1u : sizeof(payload);
    memcpy(buffer,payload,amount);return amount;
}
static const JceFsPakProvider provider={provider_find,provider_size,provider_decode};

#define PHYSFS_isInit counted_is_init
#define PHYSFS_openRead counted_open
#define PHYSFS_fileLength counted_length
#define PHYSFS_close counted_close
#define PHYSFS_readBytes counted_read
#define jce__malloc_tracked counted_alloc
#define jce__free_tracked counted_free
/* No public/dependency declaration is changed by this modeled size boundary. */
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
#undef SIZE_MAX
#define SIZE_MAX UINT32_MAX
#define size_t uint32_t
#endif
#ifndef JCE_TEST_FS_READ_ALL_OWNER
#define JCE_TEST_FS_READ_ALL_OWNER "os/core/jce_filesystem.c"
#endif
#include JCE_TEST_FS_READ_ALL_OWNER
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
#undef size_t
#undef SIZE_MAX
#endif
#undef PHYSFS_isInit
#undef PHYSFS_openRead
#undef PHYSFS_fileLength
#undef PHYSFS_close
#undef PHYSFS_readBytes
#undef jce__malloc_tracked
#undef jce__free_tracked

static JceFileSystem filesystem;
void setUp(void)
{
    jce_trace_set_enabled(false);jce_trace_task_restore_id(0);jce_trace_reset();
    memset(&filesystem,0,sizeof(filesystem));memset(data_allocations,0,sizeof(data_allocations));
    open_calls=close_calls=read_calls=alloc_calls=data_free_calls=decode_calls=0;
    loose=true;missing=reject_alloc=short_read=short_decode=false;
    reported_length=(PHYSFS_sint64)sizeof(payload);reported_asset_length=sizeof(payload);
    last_alloc=last_read=0;jce_fs_set_pak_provider(NULL);
}
void tearDown(void)
{
    unsigned i;
    jce_trace_set_enabled(false);jce_trace_task_restore_id(0);
    for(i=0;i<8;++i) if(data_allocations[i]) counted_free(data_allocations[i]);
    jce_fs_set_pak_provider(NULL);
}
static void mount_provider(void)
{
    loose=false;jce_fs_set_pak_provider(&provider);
    TEST_ASSERT_TRUE(jce_fs_mount_pak(&filesystem,(JcePakArchive *)&pak_token));
}
static void test_loose_binary_success_keeps_size_and_closes_once(void)
{
    uint64_t size=91;void *data=jce_fs_read_all(&filesystem,"asset.bin",&size);
    TEST_ASSERT_NOT_NULL(data);TEST_ASSERT_EQUAL_UINT64(sizeof(payload),size);
    TEST_ASSERT_EQUAL_MEMORY(payload,data,sizeof(payload));
    TEST_ASSERT_EQUAL_UINT(1,read_calls);TEST_ASSERT_EQUAL_UINT(1,close_calls);
    TEST_ASSERT_EQUAL_UINT64(sizeof(payload),last_read);counted_free(data);
}
static void test_miss_leaves_output_zero_without_close_or_allocation(void)
{
    missing=true;uint64_t size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(0,close_calls);
    TEST_ASSERT_EQUAL_UINT(0,alloc_calls);TEST_ASSERT_EQUAL_UINT(0,read_calls);
}
static void test_empty_or_unknown_size_closes_without_allocation(void)
{
    uint64_t size=91;reported_length=0;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(1,close_calls);
    TEST_ASSERT_EQUAL_UINT(0,alloc_calls);TEST_ASSERT_EQUAL_UINT(0,read_calls);
    reported_length=-1;size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(2,close_calls);
    TEST_ASSERT_EQUAL_UINT(0,alloc_calls);
}
static void test_allocation_failure_closes_and_does_not_publish_total(void)
{
    reject_alloc=true;uint64_t size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(1,close_calls);
    TEST_ASSERT_EQUAL_UINT(1,alloc_calls);TEST_ASSERT_EQUAL_UINT(0,read_calls);
}
static void test_short_read_frees_and_closes_without_publishing_partial(void)
{
    short_read=true;uint64_t size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(1,close_calls);
    TEST_ASSERT_EQUAL_UINT(1,read_calls);TEST_ASSERT_EQUAL_UINT(1,data_free_calls);
}
static void test_optional_output_pointer_keeps_success(void)
{
    void *data=jce_fs_read_all(&filesystem,"asset.bin",NULL);
    TEST_ASSERT_NOT_NULL(data);TEST_ASSERT_EQUAL_MEMORY(payload,data,sizeof(payload));
    TEST_ASSERT_EQUAL_UINT(1,close_calls);counted_free(data);
}
static void test_pak_public_read_transfers_one_owned_decoded_buffer(void)
{
    mount_provider();uint64_t size=91;
    void *data=jce_fs_read_all(&filesystem,"asset.bin",&size);
    TEST_ASSERT_NOT_NULL(data);TEST_ASSERT_EQUAL_MEMORY(payload,data,sizeof(payload));
    TEST_ASSERT_EQUAL_UINT64(sizeof(payload),size);TEST_ASSERT_EQUAL_UINT(1,decode_calls);
    TEST_ASSERT_EQUAL_UINT(1,alloc_calls);TEST_ASSERT_EQUAL_UINT(0,data_free_calls);
    TEST_ASSERT_EQUAL_UINT(0,close_calls);counted_free(data);TEST_ASSERT_EQUAL_UINT(1,data_free_calls);
}
static void test_incomplete_pak_decode_never_publishes_short_success(void)
{
    mount_provider();short_decode=true;uint64_t size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(1,decode_calls);
    TEST_ASSERT_EQUAL_UINT(1,alloc_calls);TEST_ASSERT_EQUAL_UINT(1,data_free_calls);
}
static void test_pak_maximum_length_rejects_before_eager_allocation(void)
{
    mount_provider();uint64_t size=91;
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
    reported_asset_length=UINT32_MAX;
#else
    reported_asset_length=UINT64_MAX;
#endif
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(0,alloc_calls);
    TEST_ASSERT_EQUAL_UINT(0,decode_calls);
}
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
static void test_size32_loose_maximum_and_wrapped_lengths_reject_before_allocation(void)
{
    const uint64_t lengths[]={UINT32_MAX,(uint64_t)UINT32_MAX+1u,(uint64_t)UINT32_MAX+18u};
    unsigned i;
    for(i=0;i<3;++i) {
        reported_length=(PHYSFS_sint64)lengths[i];uint64_t size=91;
        TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
        TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(i+1u,close_calls);
        TEST_ASSERT_EQUAL_UINT(0,alloc_calls);TEST_ASSERT_EQUAL_UINT(0,read_calls);
    }
}
static void test_size32_last_representable_payload_is_admitted_before_oom_port(void)
{
    reported_length=(PHYSFS_sint64)UINT32_MAX-1;uint64_t size=91;
    TEST_ASSERT_NULL(jce_fs_read_all(&filesystem,"asset.bin",&size));
    TEST_ASSERT_EQUAL_UINT64(0,size);TEST_ASSERT_EQUAL_UINT(1,alloc_calls);
    TEST_ASSERT_EQUAL_UINT64(UINT32_MAX,last_alloc); /* payload plus bounded owner's sentinel */
    TEST_ASSERT_EQUAL_UINT(1,close_calls);TEST_ASSERT_EQUAL_UINT(0,read_calls);
}
#endif
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_loose_binary_success_keeps_size_and_closes_once);
    RUN_TEST(test_miss_leaves_output_zero_without_close_or_allocation);
    RUN_TEST(test_empty_or_unknown_size_closes_without_allocation);
    RUN_TEST(test_allocation_failure_closes_and_does_not_publish_total);
    RUN_TEST(test_short_read_frees_and_closes_without_publishing_partial);
    RUN_TEST(test_optional_output_pointer_keeps_success);
    RUN_TEST(test_pak_public_read_transfers_one_owned_decoded_buffer);
    RUN_TEST(test_incomplete_pak_decode_never_publishes_short_success);
    RUN_TEST(test_pak_maximum_length_rejects_before_eager_allocation);
#ifdef JCE_TEST_FS_READ_ALL_SIZE32
    RUN_TEST(test_size32_loose_maximum_and_wrapped_lengths_reject_before_allocation);
    RUN_TEST(test_size32_last_representable_payload_is_admitted_before_oom_port);
#endif
    return UNITY_END();
}
