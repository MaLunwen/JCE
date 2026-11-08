/* Exact private shipping cookie owner, typed CPU ports, no GPU/device. */
#include "unity.h"
#include <bgfx/c99/bgfx.h>
#include <string.h>

static struct {
    unsigned allocations, copies, creates, binds, destroys;
    bool fail_alloc, fail_copy;
    unsigned fail_creates;
    uint16_t next_handle, bound_handle, destroyed_handle;
    uint16_t width[2], height[2], layers[2];
    uint32_t bytes[2];
    uint64_t flags[2];
    bgfx_memory_t allocation, copy;
    unsigned char atlas[256u * 256u * 4u * 16u], white[8];
} port;

static const bgfx_memory_t *counted_alloc(uint32_t bytes)
{
    ++port.allocations;
    TEST_ASSERT_EQUAL_UINT(sizeof(port.atlas), bytes);
    if (port.fail_alloc) return NULL;
    port.allocation.data = port.atlas;
    port.allocation.size = bytes;
    return &port.allocation;
}
static const bgfx_memory_t *counted_copy(const void *data, uint32_t bytes)
{
    ++port.copies;
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT(8, bytes);
    if (port.fail_copy) return NULL;
    memcpy(port.white, data, bytes);
    port.copy.data = port.white;
    port.copy.size = bytes;
    return &port.copy;
}
static bgfx_texture_handle_t counted_create(uint16_t width, uint16_t height,
    bool mipmaps, uint16_t layers, bgfx_texture_format_t format,
    uint64_t flags, const bgfx_memory_t *memory, uint64_t external)
{
    const unsigned i = port.creates++;
    TEST_ASSERT_LESS_THAN_UINT(2, i);
    TEST_ASSERT_NOT_NULL(memory);
    TEST_ASSERT_FALSE(mipmaps);
    TEST_ASSERT_EQUAL_INT(BGFX_TEXTURE_FORMAT_RGBA8, format);
    TEST_ASSERT_EQUAL_UINT64(0, external);
    TEST_ASSERT_EQUAL_UINT(width * height * 4u * layers, memory->size);
    for (uint32_t j = 0; j < memory->size; ++j)
        TEST_ASSERT_EQUAL_HEX8(0xff, memory->data[j]);
    port.width[i] = width; port.height[i] = height; port.layers[i] = layers;
    port.bytes[i] = memory->size; port.flags[i] = flags;
    bgfx_texture_handle_t handle = {UINT16_MAX};
    if (!(port.fail_creates & (1u << i))) handle.idx = port.next_handle++;
    return handle;
}
static void counted_destroy(bgfx_texture_handle_t texture)
{
    ++port.destroys; port.destroyed_handle = texture.idx;
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(texture));
}
static void jce_enc_set_texture(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    ++port.binds; port.bound_handle = texture.idx;
    TEST_ASSERT_EQUAL_UINT(13, stage);
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(sampler));
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(texture));
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, flags);
}
#define bgfx_alloc counted_alloc
#define bgfx_copy counted_copy
#define bgfx_create_texture_2d counted_create
#define bgfx_destroy_texture counted_destroy
#ifndef JCE_TEST_LIGHT_COOKIE_OWNER
#define JCE_TEST_LIGHT_COOKIE_OWNER "renderer/jce_light_cookie_internal.h"
#endif
#include JCE_TEST_LIGHT_COOKIE_OWNER

void setUp(void) { memset(&port, 0, sizeof(port)); }
void tearDown(void) {}
static bgfx_caps_t capable(void)
{
    bgfx_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.supported = BGFX_CAPS_TEXTURE_2D_ARRAY | BGFX_CAPS_TEXTURE_BLIT;
    caps.limits.maxTextureLayers = JCE_COOKIE_ATLAS_CAPACITY;
    return caps;
}
static void bind(JceLightCookieTexture *owner)
{
    bgfx_uniform_handle_t sampler = {0};
    jce_light_cookie_texture_bind(owner, sampler);
}
static void unavailable(JceLightCookieTexture *owner)
{
    TEST_ASSERT_FALSE(BGFX_HANDLE_IS_VALID(owner->texture));
    TEST_ASSERT_FALSE(owner->atlas_enabled);
    bind(owner); jce_light_cookie_texture_destroy(owner);
    TEST_ASSERT_EQUAL_UINT(0, port.binds);
    TEST_ASSERT_EQUAL_UINT(0, port.destroys);
}
static void no_array_and_null_caps_never_allocate_create_or_bind(void)
{
    bgfx_caps_t caps = capable(); caps.supported = 0;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    unavailable(&owner);
    owner = jce_light_cookie_texture_create(NULL, false, false); unavailable(&owner);
    caps = capable(); caps.limits.maxTextureLayers = 1;
    owner = jce_light_cookie_texture_create(&caps, false, false); unavailable(&owner);
    TEST_ASSERT_EQUAL_UINT(0, port.allocations + port.copies + port.creates);
}
static void compatibility_refuses_even_a_capable_backend(void)
{
    bgfx_caps_t caps = capable();
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, true, false);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_COMPATIBILITY, owner.reason);
    unavailable(&owner);
    TEST_ASSERT_EQUAL_UINT(0, port.allocations + port.copies + port.creates);
}
static void forced_off_uses_two_initialized_white_array_layers(void)
{
    bgfx_caps_t caps = capable();
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, true);
    TEST_ASSERT_FALSE(owner.atlas_enabled);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_FORCED_OFF, owner.reason);
    TEST_ASSERT_EQUAL_UINT(0, port.allocations);
    TEST_ASSERT_EQUAL_UINT(1, port.copies); TEST_ASSERT_EQUAL_UINT(1, port.creates);
    TEST_ASSERT_EQUAL_UINT(1, port.width[0]); TEST_ASSERT_EQUAL_UINT(1, port.height[0]);
    TEST_ASSERT_EQUAL_UINT(2, port.layers[0]); TEST_ASSERT_EQUAL_UINT(8, port.bytes[0]);
    TEST_ASSERT_EQUAL_UINT64(BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, port.flags[0]);
    bind(&owner); TEST_ASSERT_EQUAL_UINT(1, port.binds);
    jce_light_cookie_texture_destroy(&owner);
}
static void full_atlas_retains_dimensions_layers_and_white_initialization(void)
{
    bgfx_caps_t caps = capable();
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_TRUE(owner.atlas_enabled); TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_READY, owner.reason);
    TEST_ASSERT_EQUAL_UINT(1, port.allocations); TEST_ASSERT_EQUAL_UINT(0, port.copies);
    TEST_ASSERT_EQUAL_UINT(1, port.creates);
    TEST_ASSERT_EQUAL_UINT(JCE_COOKIE_ATLAS_DIM, port.width[0]);
    TEST_ASSERT_EQUAL_UINT(JCE_COOKIE_ATLAS_DIM, port.height[0]);
    TEST_ASSERT_EQUAL_UINT(JCE_COOKIE_ATLAS_CAPACITY, port.layers[0]);
    TEST_ASSERT_EQUAL_UINT64(BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP |
        BGFX_SAMPLER_V_CLAMP, port.flags[0]);
    bind(&owner); jce_light_cookie_texture_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.binds); TEST_ASSERT_EQUAL_UINT(1, port.destroys);
}
static void atlas_allocation_failure_uses_typed_white_fallback(void)
{
    bgfx_caps_t caps = capable(); port.fail_alloc = true;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_ATLAS_ALLOC_FAILED, owner.reason);
    TEST_ASSERT_FALSE(owner.atlas_enabled); TEST_ASSERT_EQUAL_UINT(1, port.allocations);
    TEST_ASSERT_EQUAL_UINT(1, port.copies); TEST_ASSERT_EQUAL_UINT(1, port.creates);
    TEST_ASSERT_EQUAL_UINT(2, port.layers[0]);
    bind(&owner); jce_light_cookie_texture_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.binds); TEST_ASSERT_EQUAL_UINT(1, port.destroys);
}
static void atlas_creation_failure_falls_back_without_owning_invalid_handle(void)
{
    bgfx_caps_t caps = capable(); port.fail_creates = 1;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_ATLAS_CREATE_FAILED, owner.reason);
    TEST_ASSERT_EQUAL_UINT(2, port.creates); TEST_ASSERT_EQUAL_UINT(2, port.layers[1]);
    bind(&owner); jce_light_cookie_texture_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.binds); TEST_ASSERT_EQUAL_UINT(1, port.destroys);
}
static void white_oom_never_calls_creator_with_null_or_binds_garbage(void)
{
    bgfx_caps_t caps = capable(); port.fail_alloc = true; port.fail_copy = true;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_WHITE_ALLOC_FAILED, owner.reason);
    unavailable(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.allocations); TEST_ASSERT_EQUAL_UINT(1, port.copies);
    TEST_ASSERT_EQUAL_UINT(0, port.creates);
}
static void failed_white_create_never_binds_or_destroys_invalid_handle(void)
{
    bgfx_caps_t caps = capable(); port.fail_creates = 1;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, true);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_WHITE_CREATE_FAILED, owner.reason);
    unavailable(&owner); TEST_ASSERT_EQUAL_UINT(1, port.creates);
}
static void valid_zero_handle_and_sampler_are_owned_bound_and_destroyed_once(void)
{
    bgfx_caps_t caps = capable();
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, true);
    TEST_ASSERT_EQUAL_UINT(0, owner.texture.idx);
    bind(&owner); TEST_ASSERT_EQUAL_UINT(0, port.bound_handle);
    jce_light_cookie_texture_destroy(&owner); jce_light_cookie_texture_destroy(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.destroys); TEST_ASSERT_EQUAL_UINT(0, port.destroyed_handle);
    bind(&owner); TEST_ASSERT_EQUAL_UINT(1, port.binds);
}
static void limited_layers_or_missing_blit_use_legal_placeholder(void)
{
    bgfx_caps_t caps = capable(); caps.limits.maxTextureLayers = 2;
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_EQUAL_INT(JCE_LIGHT_COOKIE_ATLAS_UNAVAILABLE, owner.reason);
    TEST_ASSERT_EQUAL_UINT(2, port.layers[0]); jce_light_cookie_texture_destroy(&owner);
    setUp(); caps = capable(); caps.supported &= ~BGFX_CAPS_TEXTURE_BLIT;
    owner = jce_light_cookie_texture_create(&caps, false, false);
    TEST_ASSERT_EQUAL_UINT(0, port.allocations); TEST_ASSERT_EQUAL_UINT(2, port.layers[0]);
    jce_light_cookie_texture_destroy(&owner);
}
static void invalid_sampler_and_null_owner_do_not_bind(void)
{
    bgfx_caps_t caps = capable(); bgfx_uniform_handle_t sampler = {UINT16_MAX};
    JceLightCookieTexture owner = jce_light_cookie_texture_create(&caps, false, true);
    jce_light_cookie_texture_bind(&owner, sampler);
    jce_light_cookie_texture_bind(NULL, sampler); jce_light_cookie_texture_destroy(NULL);
    TEST_ASSERT_EQUAL_UINT(0, port.binds); jce_light_cookie_texture_destroy(&owner);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(no_array_and_null_caps_never_allocate_create_or_bind);
    RUN_TEST(compatibility_refuses_even_a_capable_backend);
    RUN_TEST(forced_off_uses_two_initialized_white_array_layers);
    RUN_TEST(full_atlas_retains_dimensions_layers_and_white_initialization);
    RUN_TEST(atlas_allocation_failure_uses_typed_white_fallback);
    RUN_TEST(atlas_creation_failure_falls_back_without_owning_invalid_handle);
    RUN_TEST(white_oom_never_calls_creator_with_null_or_binds_garbage);
    RUN_TEST(failed_white_create_never_binds_or_destroys_invalid_handle);
    RUN_TEST(valid_zero_handle_and_sampler_are_owned_bound_and_destroyed_once);
    RUN_TEST(limited_layers_or_missing_blit_use_legal_placeholder);
    RUN_TEST(invalid_sampler_and_null_owner_do_not_bind);
    return UNITY_END();
}
