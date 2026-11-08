/* Executes the complete production resource owner against typed bgfx ports.
 * No graphics device is created; these are lifetime/upload controls only. */
#include "jce_water_gradient_gpu.h"
#include <jce/os/core/jce_math.h>
#include "unity.h"
#include <string.h>

struct JceWaterFft { int n; float x[64], z[64]; };
struct JceWaterField { JceWaterFft *first, *second; uint64_t revision; };
static bgfx_caps_t caps;
static bool valid_format, fail_create;
static unsigned creates, destroys, updates, allocations, fail_allocation;
static bool live[64];
static bgfx_memory_t memory;
static uint16_t storage[512];
static float last_r, last_g;

const bgfx_caps_t *bgfx_get_caps(void) { return &caps; }
bool bgfx_is_texture_valid(uint16_t depth, bool cube, uint16_t layers,
    bgfx_texture_format_t format, uint64_t flags)
{ (void)depth; (void)cube; (void)layers; (void)format; (void)flags; return valid_format; }
bgfx_texture_handle_t bgfx_create_texture_2d(uint16_t width, uint16_t height,
    bool mips, uint16_t layers, bgfx_texture_format_t format, uint64_t flags,
    const bgfx_memory_t *data, uint64_t external)
{
    (void)layers; (void)flags; (void)data; (void)external;
    TEST_ASSERT_EQUAL_UINT16(width, height); TEST_ASSERT_TRUE(mips);
    TEST_ASSERT_EQUAL_INT(BGFX_TEXTURE_FORMAT_RGBA16F, format);
    bgfx_texture_handle_t handle = { UINT16_MAX };
    if (!fail_create) { handle.idx = (uint16_t)++creates; live[handle.idx] = true; }
    return handle;
}
void bgfx_destroy_texture(bgfx_texture_handle_t texture)
{
    TEST_ASSERT_TRUE(texture.idx < 64 && live[texture.idx]);
    live[texture.idx] = false; destroys++;
}
const bgfx_memory_t *bgfx_alloc(uint32_t size)
{
    allocations++;
    if (fail_allocation && allocations == fail_allocation) return NULL;
    TEST_ASSERT_TRUE(size <= sizeof(storage));
    memory.data = (uint8_t *)storage; memory.size = size; return &memory;
}
void bgfx_update_texture_2d(bgfx_texture_handle_t texture, uint16_t layer,
    uint8_t mip, uint16_t x, uint16_t y, uint16_t width, uint16_t height,
    const bgfx_memory_t *data, uint16_t pitch)
{
    (void)layer; (void)mip; (void)x; (void)y;
    TEST_ASSERT_TRUE(live[texture.idx]);
    TEST_ASSERT_EQUAL_UINT16(width, height);
    TEST_ASSERT_EQUAL_UINT16(width * 8, pitch);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)width * height * 8u, data->size);
    const uint16_t *pixels = (const uint16_t *)data->data;
    last_r = jce_half_to_float(pixels[0]); last_g = jce_half_to_float(pixels[1]);
    updates++;
}
const JceWaterFft *JCE_CALL jce_water_field_fft(const JceWaterField *f) { return f->first; }
const JceWaterFft *JCE_CALL jce_water_field_fft2(const JceWaterField *f) { return f->second; }
uint64_t JCE_CALL jce_water_field_revision(const JceWaterField *f) { return f->revision; }
int JCE_CALL jce_water_fft_resolution(const JceWaterFft *f) { return f->n; }
const float *JCE_CALL jce_water_fft_slope_x_data(const JceWaterFft *f) { return f->x; }
const float *JCE_CALL jce_water_fft_slope_z_data(const JceWaterFft *f) { return f->z; }

void setUp(void)
{
    memset(&caps, 0, sizeof(caps)); memset(live, 0, sizeof(live));
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = BGFX_CAPS_FORMAT_TEXTURE_2D;
    valid_format = true; fail_create = false;
    creates = destroys = updates = allocations = fail_allocation = 0;
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(creates, destroys); }

static void test_gradient_cache_revision_identity_and_retirement(void)
{
    JceWaterFft fft = {0}; fft.n = 4;
    for (int i=0;i<16;++i) { fft.x[i]=2.0f; fft.z[i]=-3.0f; }
    JceWaterField field = { &fft, NULL, 1 };
    JceWaterGradientGpu *cache = NULL;
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_EQUAL_UINT(3, updates);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, last_r); TEST_ASSERT_EQUAL_FLOAT(-3.0f, last_g);
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_EQUAL_UINT(3, updates);
    field.revision++;
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_EQUAL_UINT(6, updates);
    JceWaterField other = field;
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &other));
    TEST_ASSERT_EQUAL_UINT(9, updates);
    TEST_ASSERT_EQUAL_UINT(1, creates);
    jce_water_gradient_gpu_destroy(cache);
}

static void test_gradient_failed_upload_is_not_ready_and_retries(void)
{
    JceWaterFft fft = {0}; fft.n = 4;
    JceWaterField field = { &fft, NULL, 1 };
    JceWaterGradientGpu *cache = NULL;
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &field));
    field.revision++;
    fail_allocation = allocations + 2;
    TEST_ASSERT_FALSE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, jce_water_gradient_gpu_texture(cache).idx);
    fail_allocation = 0;
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_TRUE(jce_water_gradient_gpu_texture(cache).idx != UINT16_MAX);
    JceWaterGradientGpu *old = cache;
    fft.n = 8; fail_create = true;
    TEST_ASSERT_FALSE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_EQUAL_PTR(old, cache);
    jce_water_gradient_gpu_destroy(cache);
}

static void test_gradient_capability_refusal_allocates_nothing(void)
{
    JceWaterFft fft = {0}; fft.n = 4;
    JceWaterField field = { &fft, NULL, 1 };
    JceWaterGradientGpu *cache = NULL;
    valid_format = false;
    TEST_ASSERT_FALSE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_NULL(cache); TEST_ASSERT_EQUAL_UINT(0, creates);
    valid_format = true; caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F] = 0;
    TEST_ASSERT_FALSE(jce_water_gradient_gpu_prepare(&cache, &field));
    TEST_ASSERT_NULL(cache); TEST_ASSERT_EQUAL_UINT(0, creates);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gradient_cache_revision_identity_and_retirement);
    RUN_TEST(test_gradient_failed_upload_is_not_ready_and_retries);
    RUN_TEST(test_gradient_capability_refusal_allocates_nothing);
    return UNITY_END();
}
