#include "renderer/jce_model_texture_pool.h"
#include "os/core/jce_memory.h"
#include <unity.h>

struct JceTextureCpu { uint16_t idx; };
static uint32_t decodes, uploads, cpu_frees, gpu_frees;
static int last_mode;
static bool decode_fails, upload_fails;

void setUp(void)
{
    decodes = uploads = cpu_frees = gpu_frees = 0;
    last_mode = 0;
    decode_fails = upload_fails = false;
}
void tearDown(void) {}

static JceTextureCpu *decode(void *user, int mode)
{
    (void)user;
    ++decodes;
    last_mode = mode;
    if (decode_fails) return NULL;
    JceTextureCpu *cpu = JCE_NEW(JceTextureCpu);
    TEST_ASSERT_NOT_NULL(cpu);
    cpu->idx = (uint16_t)decodes;
    return cpu;
}

void jce_texture_cpu_free(JceTextureCpu *cpu)
{
    if (cpu) { ++cpu_frees; JCE_FREE(cpu); }
}

JceTexture jce_texture_upload_cpu(JceTextureCpu *cpu)
{
    ++uploads;
    JceTexture gpu = upload_fails ? JCE_TEXTURE_INVALID : (JceTexture){cpu->idx};
    jce_texture_cpu_free(cpu);
    return gpu;
}

void jce_texture_destroy(JceTexture texture)
{
    TEST_ASSERT_TRUE(jce_texture_valid(texture));
    ++gpu_frees;
}

void jce_texture_get_size(JceTexture texture, uint32_t *width, uint32_t *height)
{
    TEST_ASSERT_TRUE(jce_texture_valid(texture));
    *width = 64; *height = 32;
}

static void shared_image_decodes_uploads_and_releases_once(void)
{
    JceModelTexturePool *pool = jce_model_texture_pool_create();
    TEST_ASSERT_NOT_NULL(pool);
    uint32_t first = jce_model_texture_pool_acquire(pool, 7, JCE_TEX_WRAP, decode, NULL);
    uint32_t second = jce_model_texture_pool_acquire(pool, 7, JCE_TEX_WRAP, decode, NULL);
    TEST_ASSERT_EQUAL_UINT32(first, second);
    TEST_ASSERT_EQUAL_UINT32(1, decodes);
    JceTexture a = jce_model_texture_pool_upload(pool, first);
    JceTexture b = jce_model_texture_pool_upload(pool, second);
    TEST_ASSERT_EQUAL_UINT16(a.idx, b.idx);
    TEST_ASSERT_EQUAL_UINT32(1, uploads);
    TEST_ASSERT_EQUAL_UINT32(1, cpu_frees);
    TEST_ASSERT_EQUAL_UINT64(64u * 32u * 4u, jce_model_texture_pool_gpu_bytes(pool));
    jce_model_texture_pool_destroy(pool);
    TEST_ASSERT_EQUAL_UINT32(1, gpu_frees);
    TEST_ASSERT_EQUAL_UINT32(1, cpu_frees);
}

static void color_space_and_sampler_mode_are_distinct_resources(void)
{
    JceModelTexturePool *pool = jce_model_texture_pool_create();
    uint32_t linear = jce_model_texture_pool_acquire(pool, 1, JCE_TEX_WRAP, decode, NULL);
    uint32_t srgb = jce_model_texture_pool_acquire(pool, 1,
        JCE_TEX_WRAP | JCE_TEX_SRGB, decode, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_TEX_WRAP | JCE_TEX_SRGB, last_mode);
    uint32_t clamp = jce_model_texture_pool_acquire(pool, 1, JCE_TEX_CLAMP, decode, NULL);
    TEST_ASSERT_NOT_EQUAL(linear, srgb);
    TEST_ASSERT_NOT_EQUAL(linear, clamp);
    TEST_ASSERT_EQUAL_UINT32(3, decodes);
    jce_model_texture_pool_upload(pool, linear);
    jce_model_texture_pool_upload(pool, srgb);
    TEST_ASSERT_EQUAL_UINT64(2u * 64u * 32u * 4u, jce_model_texture_pool_gpu_bytes(pool));
    jce_model_texture_pool_destroy(pool);
    TEST_ASSERT_EQUAL_UINT32(2, gpu_frees);
    TEST_ASSERT_EQUAL_UINT32(3, cpu_frees);
}

static void cancellation_after_growth_releases_each_pending_image_once(void)
{
    JceModelTexturePool *pool = jce_model_texture_pool_create();
    for (uint32_t i = 1; i <= 65; ++i) {
        uint32_t slot = jce_model_texture_pool_acquire(pool, i, 0, decode, NULL);
        TEST_ASSERT_EQUAL_UINT32(i, slot);
        TEST_ASSERT_EQUAL_UINT32(slot,
            jce_model_texture_pool_acquire(pool, i, 0, decode, NULL));
    }
    TEST_ASSERT_EQUAL_UINT32(65, decodes);
    TEST_ASSERT_EQUAL_UINT64(0, jce_model_texture_pool_gpu_bytes(pool));
    jce_model_texture_pool_destroy(pool);
    TEST_ASSERT_EQUAL_UINT32(65, cpu_frees);
    TEST_ASSERT_EQUAL_UINT32(0, uploads);
    TEST_ASSERT_EQUAL_UINT32(0, gpu_frees);
}

static void failures_are_not_retried_with_consumed_staging(void)
{
    JceModelTexturePool *pool = jce_model_texture_pool_create();
    decode_fails = true;
    uint32_t bad = jce_model_texture_pool_acquire(pool, 1, 0, decode, NULL);
    TEST_ASSERT_EQUAL_UINT32(bad, jce_model_texture_pool_acquire(pool, 1, 0, decode, NULL));
    TEST_ASSERT_EQUAL_UINT32(1, decodes);
    decode_fails = false;
    uint32_t good = jce_model_texture_pool_acquire(pool, 2, 0, decode, NULL);
    upload_fails = true;
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(pool, bad)));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(pool, good)));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(pool, good)));
    TEST_ASSERT_EQUAL_UINT32(1, uploads);
    TEST_ASSERT_EQUAL_UINT32(1, cpu_frees);
    TEST_ASSERT_EQUAL_UINT32(0, jce_model_texture_pool_acquire(pool, 3, 0, decode, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, jce_model_texture_pool_gpu_bytes(pool));
    jce_model_texture_pool_destroy(pool);
    TEST_ASSERT_EQUAL_UINT32(0, gpu_frees);
}

static void absent_and_invalid_slots_do_not_touch_resources(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, jce_model_texture_pool_acquire(NULL, 1, 0, decode, NULL));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(NULL, 1)));
    TEST_ASSERT_EQUAL_UINT64(0, jce_model_texture_pool_gpu_bytes(NULL));
    jce_model_texture_pool_destroy(NULL);
    JceModelTexturePool *pool = jce_model_texture_pool_create();
    TEST_ASSERT_EQUAL_UINT32(0, jce_model_texture_pool_acquire(pool, 0, 0, decode, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, jce_model_texture_pool_acquire(pool, 1, 0, NULL, NULL));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(pool, 0)));
    TEST_ASSERT_FALSE(jce_texture_valid(jce_model_texture_pool_upload(pool, 2)));
    TEST_ASSERT_EQUAL_UINT32(0, decodes);
    jce_model_texture_pool_destroy(pool);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(shared_image_decodes_uploads_and_releases_once);
    RUN_TEST(color_space_and_sampler_mode_are_distinct_resources);
    RUN_TEST(cancellation_after_growth_releases_each_pending_image_once);
    RUN_TEST(failures_are_not_retried_with_consumed_staging);
    RUN_TEST(absent_and_invalid_slots_do_not_touch_resources);
    return UNITY_END();
}
