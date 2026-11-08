/* Exact composite LUT owner through typed CPU ports; no graphics device. */
#include "unity.h"
#include <bgfx/c99/bgfx.h>
#include <jce/os/core/jce_log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct {
    unsigned copies, creates, uniform_creates, binds, uploads;
    unsigned texture_destroys, uniform_destroys, warnings, errors;
    unsigned fail_uniforms;
    bool fail_copy, fail_texture;
    uint16_t next_texture, bound_texture, destroyed_texture, destroyed_uniforms[4];
    float params[4];
    bgfx_memory_t memory;
    uint32_t white;
    char warning[384];
} port;
static bgfx_uniform_handle_t counted_create_uniform(const char *name,
    bgfx_uniform_type_t type, uint16_t count)
{
    const unsigned i = port.uniform_creates++;
    TEST_ASSERT_LESS_THAN_UINT(4, i);
    TEST_ASSERT_EQUAL_STRING((i & 1u) ? "s_texLUT" : "u_gradeParams", name);
    TEST_ASSERT_EQUAL_INT((i & 1u) ? BGFX_UNIFORM_TYPE_SAMPLER : BGFX_UNIFORM_TYPE_VEC4, type);
    TEST_ASSERT_EQUAL_UINT(1, count);
    bgfx_uniform_handle_t handle = {(uint16_t)i};
    if (port.fail_uniforms & (1u << i)) handle.idx = UINT16_MAX;
    return handle;
}
static const bgfx_memory_t *counted_copy(const void *data, uint32_t bytes)
{
    ++port.copies;
    TEST_ASSERT_NOT_NULL(data); TEST_ASSERT_EQUAL_UINT(4, bytes);
    if (port.fail_copy) return NULL;
    memcpy(&port.white, data, bytes); TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, port.white);
    port.memory.data = (uint8_t *)&port.white; port.memory.size = bytes;
    return &port.memory;
}
static bgfx_texture_handle_t counted_create(uint16_t width, uint16_t height,
    uint16_t depth, bool mipmaps, bgfx_texture_format_t format,
    uint64_t flags, const bgfx_memory_t *memory, uint64_t external)
{
    ++port.creates;
    TEST_ASSERT_EQUAL_UINT(1, width); TEST_ASSERT_EQUAL_UINT(1, height);
    TEST_ASSERT_EQUAL_UINT(1, depth); TEST_ASSERT_FALSE(mipmaps);
    TEST_ASSERT_EQUAL_INT(BGFX_TEXTURE_FORMAT_RGBA8, format);
    TEST_ASSERT_EQUAL_UINT64(0, flags); TEST_ASSERT_EQUAL_UINT64(0, external);
    TEST_ASSERT_NOT_NULL(memory); TEST_ASSERT_EQUAL_UINT(4, memory->size);
    TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, *(const uint32_t *)memory->data);
    bgfx_texture_handle_t handle = {UINT16_MAX};
    if (!port.fail_texture) handle.idx = port.next_texture++;
    return handle;
}
static void counted_texture_destroy(bgfx_texture_handle_t texture)
{
    ++port.texture_destroys; port.destroyed_texture = texture.idx;
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(texture));
}
static void counted_uniform_destroy(bgfx_uniform_handle_t uniform)
{
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(uniform));
    TEST_ASSERT_LESS_THAN_UINT(4, port.uniform_destroys);
    port.destroyed_uniforms[port.uniform_destroys++] = uniform.idx;
}
static void counted_uniform(bgfx_uniform_handle_t uniform, const void *value, uint16_t count)
{
    ++port.uploads; TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(uniform));
    TEST_ASSERT_EQUAL_UINT(1, count); memcpy(port.params, value, sizeof(port.params));
}
static void counted_bind(uint8_t stage, bgfx_uniform_handle_t sampler,
    bgfx_texture_handle_t texture, uint32_t flags)
{
    ++port.binds; port.bound_texture = texture.idx;
    TEST_ASSERT_EQUAL_UINT(2, stage); TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(sampler));
    TEST_ASSERT_TRUE(BGFX_HANDLE_IS_VALID(texture)); TEST_ASSERT_EQUAL_HEX32(UINT32_MAX, flags);
}
static void counted_log(JceLogLevel level, const char *tag, const char *file,
    int line, const char *format, ...)
{
    (void)file; (void)line; TEST_ASSERT_EQUAL_STRING("postfx", tag);
    if (level == JCE_LOG_LEVEL_WARN) {
        ++port.warnings; va_list args; va_start(args, format);
        vsnprintf(port.warning, sizeof(port.warning), format, args); va_end(args);
    } else if (level == JCE_LOG_LEVEL_ERROR) ++port.errors;
    else TEST_FAIL_MESSAGE("unexpected grade log level");
}
#define bgfx_create_uniform counted_create_uniform
#define bgfx_copy counted_copy
#define bgfx_create_texture_3d counted_create
#define bgfx_destroy_texture counted_texture_destroy
#define bgfx_destroy_uniform counted_uniform_destroy
#define bgfx_set_uniform counted_uniform
#define bgfx_set_texture counted_bind
#define jce_log_write counted_log
#ifndef JCE_TEST_POSTFX_GRADE_OWNER
#define JCE_TEST_POSTFX_GRADE_OWNER "renderer/jce_postfx_grade_internal.h"
#endif
#include JCE_TEST_POSTFX_GRADE_OWNER

void setUp(void) { memset(&port, 0, sizeof(port)); }
void tearDown(void) {}
static JcePostFXGrade create(bool compatibility, bool texture3d)
{
    JcePostFXGrade owner = jce_postfx_grade_empty();
    jce_postfx_grade_acquire(&owner, compatibility, texture3d);
    return owner;
}
static bgfx_texture_handle_t borrowed(uint16_t index)
{ bgfx_texture_handle_t texture = {index}; return texture; }
static void compatibility_default_neutral_has_no_3d_uniform_or_bind(void)
{
    JcePostFXGrade owner = create(true, true);
    for (int i = 0; i < 20; ++i) jce_postfx_grade_submit(&owner, true);
    jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(0, port.copies + port.creates + port.uniform_creates);
    TEST_ASSERT_EQUAL_UINT(0, port.binds + port.uploads + port.warnings + port.errors);
    TEST_ASSERT_EQUAL_UINT(0, port.texture_destroys + port.uniform_destroys);
}
static void compatibility_retains_authored_request_and_warns_once_per_owner(void)
{
    JcePostFXGrade owner = create(true, true);
    jce_postfx_grade_set(&owner, borrowed(55), 16, 0.75f);
    jce_postfx_grade_set(&owner, borrowed(56), 32, 0.5f);
    for (int i = 0; i < 20; ++i) jce_postfx_grade_submit(&owner, true);
    TEST_ASSERT_EQUAL_UINT(56, owner.borrowed.idx); TEST_ASSERT_EQUAL_INT(32, owner.size);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, owner.strength); TEST_ASSERT_EQUAL_UINT(1, port.warnings);
    TEST_ASSERT_NOT_NULL(strstr(port.warning, "compatibility GL/ES"));
    TEST_ASSERT_NOT_NULL(strstr(port.warning, "retained"));
    TEST_ASSERT_EQUAL_UINT(0, port.binds + port.uploads + port.copies + port.creates + port.uniform_creates);
    jce_postfx_grade_release(&owner); TEST_ASSERT_EQUAL_UINT(0, port.texture_destroys);
}
static void missing_3d_capability_is_explicit_and_preserves_borrowed_zero(void)
{
    JcePostFXGrade owner = create(false, false);
    jce_postfx_grade_set(&owner, borrowed(0), 8, 1.0f);
    jce_postfx_grade_submit(&owner, true); jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(0, owner.borrowed.idx); TEST_ASSERT_EQUAL_UINT(1, port.warnings);
    TEST_ASSERT_NOT_NULL(strstr(port.warning, "3D textures unsupported"));
    TEST_ASSERT_EQUAL_UINT(0, port.copies + port.creates + port.uniform_creates + port.binds + port.texture_destroys);
}
static void modern_default_parks_real_3d_zero_handle_and_releases_it_once(void)
{
    JcePostFXGrade owner = create(false, true);
    TEST_ASSERT_EQUAL_INT(JCE_POSTFX_GRADE_READY, owner.reason);
    TEST_ASSERT_EQUAL_UINT(0, owner.placeholder.idx); TEST_ASSERT_EQUAL_UINT(2, port.uniform_creates);
    TEST_ASSERT_EQUAL_UINT(1, port.copies); TEST_ASSERT_EQUAL_UINT(1, port.creates);
    jce_postfx_grade_submit(&owner, true);
    TEST_ASSERT_EQUAL_UINT(1, port.binds); TEST_ASSERT_EQUAL_UINT(0, port.bound_texture);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, port.params[0]); TEST_ASSERT_EQUAL_FLOAT(1.0f, port.params[2]);
    jce_postfx_grade_release(&owner); jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.texture_destroys); TEST_ASSERT_EQUAL_UINT(0, port.destroyed_texture);
    TEST_ASSERT_EQUAL_UINT(2, port.uniform_destroys); TEST_ASSERT_EQUAL_UINT(0, port.destroyed_uniforms[0]);
}
static void modern_grade_params_and_tonemap_gate_preserve_original_formula(void)
{
    JcePostFXGrade owner = create(false, true);
    jce_postfx_grade_set(&owner, borrowed(61), 32, 0.7f);
    jce_postfx_grade_submit(&owner, true);
    TEST_ASSERT_EQUAL_UINT(61, port.bound_texture); TEST_ASSERT_EQUAL_FLOAT(1.0f, port.params[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.7f, port.params[1]); TEST_ASSERT_EQUAL_FLOAT(32.0f, port.params[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, port.params[3]); jce_postfx_grade_submit(&owner, false);
    TEST_ASSERT_EQUAL_UINT(0, port.bound_texture); TEST_ASSERT_EQUAL_FLOAT(0.0f, port.params[0]);
    jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(0, port.destroyed_texture); TEST_ASSERT_EQUAL_UINT(61, owner.borrowed.idx);
    TEST_ASSERT_EQUAL_UINT(0, port.warnings + port.errors);
}
static void borrowed_zero_is_valid_and_is_never_owned_or_freed(void)
{
    port.next_texture = 7; JcePostFXGrade owner = create(false, true);
    jce_postfx_grade_set(&owner, borrowed(0), 16, 1.0f); jce_postfx_grade_submit(&owner, true);
    TEST_ASSERT_EQUAL_UINT(0, port.bound_texture); TEST_ASSERT_EQUAL_FLOAT(1.0f, port.params[0]);
    jce_postfx_grade_release(&owner); TEST_ASSERT_EQUAL_UINT(7, port.destroyed_texture);
    TEST_ASSERT_EQUAL_UINT(0, owner.borrowed.idx); TEST_ASSERT_EQUAL_INT(16, owner.size);
}
static void setter_clamp_and_nonpositive_size_fallback_remain_unchanged(void)
{
    JcePostFXGrade owner = create(false, true);
    jce_postfx_grade_set(&owner, borrowed(44), -5, -1.0f);
    TEST_ASSERT_EQUAL_INT(-5, owner.size); TEST_ASSERT_EQUAL_FLOAT(0.0f, owner.strength);
    jce_postfx_grade_set(&owner, borrowed(44), -5, 2.0f); jce_postfx_grade_submit(&owner, true);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, owner.strength); TEST_ASSERT_EQUAL_FLOAT(1.0f, port.params[2]);
    jce_postfx_grade_release(&owner);
}
static void placeholder_oom_does_not_create_null_texture_or_leak_uniforms(void)
{
    port.fail_copy = true; JcePostFXGrade owner = create(false, true);
    TEST_ASSERT_EQUAL_INT(JCE_POSTFX_GRADE_MEMORY_FAILED, owner.reason);
    jce_postfx_grade_set(&owner, borrowed(13), 8, 0.9f); jce_postfx_grade_submit(&owner, true);
    jce_postfx_grade_release(&owner); jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.copies); TEST_ASSERT_EQUAL_UINT(0, port.creates + port.binds + port.uploads);
    TEST_ASSERT_EQUAL_UINT(2, port.uniform_destroys); TEST_ASSERT_EQUAL_UINT(0, port.texture_destroys);
    TEST_ASSERT_EQUAL_UINT(1, port.errors); TEST_ASSERT_EQUAL_UINT(1, port.warnings);
}
static void invalid_placeholder_creation_is_explicit_and_never_owned(void)
{
    port.fail_texture = true; JcePostFXGrade owner = create(false, true);
    TEST_ASSERT_EQUAL_INT(JCE_POSTFX_GRADE_TEXTURE_FAILED, owner.reason);
    jce_postfx_grade_submit(&owner, true); jce_postfx_grade_release(&owner);
    TEST_ASSERT_EQUAL_UINT(1, port.creates); TEST_ASSERT_EQUAL_UINT(2, port.uniform_destroys);
    TEST_ASSERT_EQUAL_UINT(0, port.texture_destroys + port.binds + port.uploads);
    TEST_ASSERT_EQUAL_UINT(1, port.errors);
}
static void partial_uniform_creation_releases_only_valid_owned_handles(void)
{
    port.fail_uniforms = 2; JcePostFXGrade owner = create(false, true);
    TEST_ASSERT_EQUAL_INT(JCE_POSTFX_GRADE_UNIFORM_FAILED, owner.reason);
    TEST_ASSERT_EQUAL_UINT(1, port.uniform_destroys); TEST_ASSERT_EQUAL_UINT(0, port.destroyed_uniforms[0]);
    TEST_ASSERT_EQUAL_UINT(0, port.copies + port.creates + port.texture_destroys);
    jce_postfx_grade_release(&owner); TEST_ASSERT_EQUAL_UINT(1, port.uniform_destroys);
}
static void null_owners_never_reach_backend_ports(void)
{
    jce_postfx_grade_acquire(NULL, false, true); jce_postfx_grade_submit(NULL, true);
    jce_postfx_grade_set(NULL, borrowed(0), 16, 1.0f); jce_postfx_grade_release(NULL);
    TEST_ASSERT_EQUAL_UINT(0, port.copies + port.creates + port.uniform_creates + port.binds + port.uploads);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(compatibility_default_neutral_has_no_3d_uniform_or_bind);
    RUN_TEST(compatibility_retains_authored_request_and_warns_once_per_owner);
    RUN_TEST(missing_3d_capability_is_explicit_and_preserves_borrowed_zero);
    RUN_TEST(modern_default_parks_real_3d_zero_handle_and_releases_it_once);
    RUN_TEST(modern_grade_params_and_tonemap_gate_preserve_original_formula);
    RUN_TEST(borrowed_zero_is_valid_and_is_never_owned_or_freed);
    RUN_TEST(setter_clamp_and_nonpositive_size_fallback_remain_unchanged);
    RUN_TEST(placeholder_oom_does_not_create_null_texture_or_leak_uniforms);
    RUN_TEST(invalid_placeholder_creation_is_explicit_and_never_owned);
    RUN_TEST(partial_uniform_creation_releases_only_valid_owned_handles);
    RUN_TEST(null_owners_never_reach_backend_ports);
    return UNITY_END();
}
