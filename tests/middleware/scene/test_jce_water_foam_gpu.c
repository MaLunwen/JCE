/* Executes the complete production resource owner against typed bgfx ports.
 * No graphics device is created; these are lifetime/upload controls only. */
#include "jce_water_foam_gpu.h"
#include <jce/os/core/jce_math.h>
#include "unity.h"
#include <string.h>

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
void setUp(void)
{
    memset(&caps,0,sizeof(caps));memset(live,0,sizeof(live));
    caps.formats[BGFX_TEXTURE_FORMAT_RGBA16F]=BGFX_CAPS_FORMAT_TEXTURE_2D;
    valid_format=true;fail_create=false;
    creates=destroys=updates=allocations=fail_allocation=0;
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(creates,destroys); }
static JceWaterFoamState state_for(float *a, float *b)
{
    JceWaterFoamState state={0};state.resolution=4;
    state.density[0]=a;state.density[1]=b;
    state.identity=1;state.revision=1;
    return state;
}
static void test_density_mip_identity_and_retirement(void)
{
    float a[64],b[64];for(int i=0;i<64;++i){a[i]=0.25f;b[i]=0.75f;}
    JceWaterFoamState state=state_for(a,b);
    JceWaterFoamGpu *cache=NULL;
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_UINT(3,updates);
    TEST_ASSERT_EQUAL_FLOAT(0.25f,last_r);TEST_ASSERT_EQUAL_FLOAT(0.75f,last_g);
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_UINT(3,updates);
    ++state.revision;
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_UINT(6,updates);
    ++state.identity;
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_UINT(9,updates);TEST_ASSERT_EQUAL_UINT(1,creates);
    jce_water_foam_gpu_destroy(cache);
}
static void test_partial_upload_retry_and_resize_rollback(void)
{
    float a[64]={0};JceWaterFoamState state=state_for(a,NULL);
    JceWaterFoamGpu *cache=NULL;
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    ++state.revision;fail_allocation=allocations+2;
    TEST_ASSERT_FALSE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX,jce_water_foam_gpu_texture(cache).idx);
    fail_allocation=0;
    TEST_ASSERT_TRUE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_TRUE(jce_water_foam_gpu_texture(cache).idx!=UINT16_MAX);
    JceWaterFoamGpu *old=cache;state.resolution=8;fail_create=true;
    TEST_ASSERT_FALSE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_EQUAL_PTR(old,cache);
    jce_water_foam_gpu_destroy(cache);
}
static void test_capability_and_invalid_density_refusal(void)
{
    float a[64]={0};JceWaterFoamState state=state_for(a,NULL);
    JceWaterFoamGpu *cache=NULL;valid_format=false;
    TEST_ASSERT_FALSE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_NULL(cache);TEST_ASSERT_EQUAL_UINT(0,creates);
    valid_format=true;a[7]=NAN;
    TEST_ASSERT_FALSE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_NULL(cache);TEST_ASSERT_EQUAL_UINT(0,creates);
    a[7]=1.1f;
    TEST_ASSERT_FALSE(jce_water_foam_gpu_prepare(&cache,&state));
    TEST_ASSERT_NULL(cache);TEST_ASSERT_EQUAL_UINT(0,creates);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_density_mip_identity_and_retirement);
    RUN_TEST(test_partial_upload_retry_and_resize_rollback);
    RUN_TEST(test_capability_and_invalid_density_refusal);
    return UNITY_END();
}
