/* Complete shipping caps owner with typed CPU-only backend query ports. */
#include "unity.h"
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_render_pipeline.h>
#include <jce/os/core/jce_log.h>
#include <bgfx/c99/bgfx.h>
#include <SDL3/SDL_cpuinfo.h>
#include "renderer/jce_renderer_caps_internal.h"
#include "renderer/jce_renderer_web_api_internal.h"
#include <string.h>

#ifndef JCE_TEST_API_TIER
#error "API admission fixture requires an explicit compiled policy"
#endif
#undef JCE_GRAPHICS_API_TIER_VALUE
#undef JCE_BGFX_OPENGL_VERSION
#undef JCE_BGFX_OPENGLES_VERSION
#define JCE_GRAPHICS_API_TIER_VALUE JCE_TEST_API_TIER
#if JCE_TEST_API_TIER == 0
#define JCE_BGFX_OPENGL_VERSION 31
#define JCE_BGFX_OPENGLES_VERSION 30
#elif JCE_TEST_API_TIER == 1
#define JCE_BGFX_OPENGL_VERSION 43
#define JCE_BGFX_OPENGLES_VERSION 31
#elif JCE_TEST_API_TIER == 2
#define JCE_BGFX_OPENGL_VERSION 46
#define JCE_BGFX_OPENGLES_VERSION 32
#elif JCE_TEST_API_TIER == 3
#define JCE_BGFX_OPENGL_VERSION 21
#define JCE_BGFX_OPENGLES_VERSION 20
#endif
#ifdef JCE_TEST_WRONG_OPENGL_VERSION
#undef JCE_BGFX_OPENGL_VERSION
#define JCE_BGFX_OPENGL_VERSION JCE_TEST_WRONG_OPENGL_VERSION
#endif

/* The fixture's configured tier is the bgfx package ordinal. */
static const JceGraphicsApiTier expected_build_tier =
    (JceGraphicsApiTier)(JCE_TEST_API_TIER == 3 ? 4 : JCE_TEST_API_TIER);
static JceGraphicsApiTier initial_build_tier;

static bgfx_caps_t fixture_caps;
static bool fixture_caps_available;
static bgfx_renderer_type_t fixture_backend;
static unsigned fixture_warnings;
static const bgfx_caps_t *caps_port(void)
{ return fixture_caps_available ? &fixture_caps : NULL; }
static bgfx_renderer_type_t backend_port(void) { return fixture_backend; }
static const char *name_port(bgfx_renderer_type_t backend)
{ (void)backend; return "OpenGL compiled-floor-label"; }
static uint8_t supported_port(uint8_t max, bgfx_renderer_type_t *out)
{ if (max && out) { out[0] = fixture_backend; return 1; } return 0; }
static int SDLCALL cores_port(void) { return 1; }
static int SDLCALL ram_port(void) { return 512; }
static void pipeline_port(void) {}
static void web_cache_reset_port(void) {}
static void JCE_CALL log_port(JceLogLevel level, const char *tag,
    const char *file, int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line; (void)format;
    if (level == JCE_LOG_LEVEL_WARN) ++fixture_warnings;
}
#define bgfx_get_caps caps_port
#define bgfx_get_renderer_type backend_port
#define bgfx_get_renderer_name name_port
#define bgfx_get_supported_renderers supported_port
#define SDL_GetNumLogicalCPUCores cores_port
#define SDL_GetSystemRAM ram_port
#define jce_render_pipeline_notify_tier_changed pipeline_port
#define jce_webgl_context_cache_reset_internal web_cache_reset_port
#define jce_log_write log_port
#ifndef JCE_TEST_API_CAPS_OWNER
#define JCE_TEST_API_CAPS_OWNER "renderer/jce_renderer_caps.c"
#endif
#include JCE_TEST_API_CAPS_OWNER
#undef bgfx_get_caps
#undef bgfx_get_renderer_type
#undef bgfx_get_renderer_name
#undef bgfx_get_supported_renderers
#undef SDL_GetNumLogicalCPUCores
#undef SDL_GetSystemRAM
#undef jce_render_pipeline_notify_tier_changed
#undef jce_webgl_context_cache_reset_internal
#undef jce_log_write

void setUp(void)
{
    memset(&fixture_caps, 0, sizeof(fixture_caps));
    fixture_caps_available = true;
    fixture_backend = BGFX_RENDERER_TYPE_OPENGL;
    fixture_warnings = 0;
    jce_renderer_caps_api_reset();
}
void tearDown(void) { jce_renderer_caps_api_reset(); }
static void version_is(JceGraphicsApiVersion version, uint16_t major, uint16_t minor)
{
    TEST_ASSERT_EQUAL_UINT16(major, version.major);
    TEST_ASSERT_EQUAL_UINT16(minor, version.minor);
    TEST_ASSERT_EQUAL_UINT16(0, version.patch);
}
static void append_only_tier_identity_and_minima(void)
{
    TEST_ASSERT_EQUAL_INT(0, JCE_GRAPHICS_API_TIER_STABLE);
    TEST_ASSERT_EQUAL_INT(1, JCE_GRAPHICS_API_TIER_MODERN);
    TEST_ASSERT_EQUAL_INT(2, JCE_GRAPHICS_API_TIER_CURRENT);
    TEST_ASSERT_EQUAL_INT(3, JCE_GRAPHICS_API_TIER_COUNT);
    TEST_ASSERT_EQUAL_INT(4, JCE_GRAPHICS_API_TIER_COMPATIBILITY);
    TEST_ASSERT_EQUAL_INT(expected_build_tier, initial_build_tier);
    TEST_ASSERT_EQUAL_INT(expected_build_tier, jce_renderer_get_api_info().build_tier);
    TEST_ASSERT_EQUAL_STRING("unknown",
        jce_graphics_api_tier_name(JCE_GRAPHICS_API_TIER_COUNT));
    TEST_ASSERT_EQUAL_STRING("compatibility",
        jce_graphics_api_tier_name(JCE_GRAPHICS_API_TIER_COMPATIBILITY));
    TEST_ASSERT_EQUAL_STRING("unknown", jce_graphics_api_tier_name((JceGraphicsApiTier)-1));
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGL,
        JCE_GRAPHICS_API_TIER_STABLE), 3, 1);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGL,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY), 2, 1);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGLES,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY), 2, 0);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_VULKAN,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY), 1, 0);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGL,
        JCE_GRAPHICS_API_TIER_COUNT), 0, 0);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGLES,
        JCE_GRAPHICS_API_TIER_COUNT), 0, 0);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_VULKAN,
        JCE_GRAPHICS_API_TIER_COUNT), 0, 0);
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_D3D11,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY), 0, 0);
}
static void each_build_rejects_below_and_accepts_its_exact_floor(void)
{
    const JceRendererBackend backends[] = {
        JCE_BACKEND_OPENGL, JCE_BACKEND_OPENGLES, JCE_BACKEND_VULKAN
    };
    const bgfx_renderer_type_t native_backends[] = {
        BGFX_RENDERER_TYPE_OPENGL, BGFX_RENDERER_TYPE_OPENGLES, BGFX_RENDERER_TYPE_VULKAN
    };
    for (unsigned i = 0; i < sizeof(backends) / sizeof(backends[0]); ++i) {
        JceGraphicsApiVersion minimum = jce_renderer_api_tier_minimum(backends[i],
            expected_build_tier);
        char below[48], exact[48];
        snprintf(exact, sizeof(exact), "%u.%u.0 fixture", minimum.major, minimum.minor);
        if (minimum.minor) {
            snprintf(below, sizeof(below), "%u.%u.65535 fixture",
                minimum.major, minimum.minor - 1u);
        } else {
            snprintf(below, sizeof(below), "%u.65535.65535 fixture", minimum.major - 1u);
        }
        fixture_backend = native_backends[i];
        jce_renderer_caps_api_reset();
        TEST_ASSERT_FALSE(jce_renderer_caps_api_publish_probe(backends[i], below, NULL));
        jce_renderer_caps_api_begin_attempt(backends[i]);
        TEST_ASSERT_FALSE(jce_renderer_caps_api_accept_active_backend());
        TEST_ASSERT_TRUE(jce_renderer_get_api_info().runtime_version_verified);
        TEST_ASSERT_TRUE(jce_renderer_caps_api_publish_probe(backends[i], exact, NULL));
        jce_renderer_caps_api_begin_attempt(backends[i]);
        TEST_ASSERT_TRUE(jce_renderer_caps_api_accept_active_backend());
        version_is(jce_renderer_get_api_info().minimum_version, minimum.major, minimum.minor);
    }
}
static void compatibility_query_cannot_lower_the_compiled_policy(void)
{
    JceRendererApiInfo info;
    const bool compatibility = expected_build_tier == JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    version_is(jce_renderer_api_tier_minimum(JCE_BACKEND_OPENGL,
        JCE_GRAPHICS_API_TIER_COMPATIBILITY), 2, 1);
    TEST_ASSERT_EQUAL_INT(compatibility,
        jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGL, "2.1 Mesa", "1.20"));
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGL);
    TEST_ASSERT_EQUAL_INT(compatibility, jce_renderer_caps_api_accept_active_backend());
    info = jce_renderer_get_api_info();
    TEST_ASSERT_EQUAL_INT(expected_build_tier, info.build_tier);
    version_is(info.runtime_version, 2, 1);
    version_is(info.shader_language_version, 1, 2);
    TEST_ASSERT_EQUAL_STRING("OpenGL 2.1", jce_renderer_running_backend_name());
    TEST_ASSERT_FALSE(jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGL, "2.0 fixture", NULL));
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGL);
    TEST_ASSERT_FALSE(jce_renderer_caps_api_accept_active_backend());
}
static void backend_switch_cannot_carry_a_different_drivers_observation(void)
{
    TEST_ASSERT_TRUE(jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGL, "4.6 fixture", "4.60"));
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGL);
    TEST_ASSERT_TRUE(jce_renderer_get_api_info().runtime_version_verified);
    jce_renderer_caps_api_begin_attempt(JCE_BACKEND_VULKAN);
    JceRendererApiInfo info = jce_renderer_get_api_info();
    TEST_ASSERT_FALSE(info.runtime_version_verified);
    TEST_ASSERT_FALSE(info.shader_language_version_verified);
    version_is(info.runtime_version, 0, 0);
    TEST_ASSERT_EQUAL_INT(expected_build_tier, info.build_tier);
}
static void unparseable_versions_remain_explicitly_unverified(void)
{
    const char *invalid[] = {NULL, "not-a-version", "65536.0", "2"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        jce_renderer_caps_api_reset();
        jce_renderer_caps_api_begin_attempt(JCE_BACKEND_OPENGL);
        TEST_ASSERT_TRUE(jce_renderer_caps_api_publish_probe(JCE_BACKEND_OPENGL, invalid[i], NULL));
        TEST_ASSERT_FALSE(jce_renderer_get_api_info().runtime_version_verified);
        TEST_ASSERT_TRUE(jce_renderer_caps_api_accept_active_backend());
        TEST_ASSERT_FALSE(jce_renderer_get_api_info().runtime_version_verified);
    }
    TEST_ASSERT_TRUE(fixture_warnings >= 8);
}
static void explicit_vertex_fetch_answers_override_every_build_policy(void)
{
    fixture_caps.formats[BGFX_TEXTURE_FORMAT_RGBA32F] = BGFX_CAPS_FORMAT_TEXTURE_VERTEX;
    TEST_ASSERT_EQUAL_INT(JCE_GPU_VFETCH_YES,
        jce_gpu_vertex_fetch_state(JCE_GPU_VFETCH_FMT_RGBA32F));
    TEST_ASSERT_TRUE(jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_RGBA32F));
    TEST_ASSERT_EQUAL_INT(JCE_GPU_VFETCH_NO, jce_gpu_vertex_fetch_state(JCE_GPU_VFETCH_FMT_R32F));
    TEST_ASSERT_FALSE(jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_R32F));
}
static void compatibility_unanswered_vertex_fetch_is_not_a_hardware_yes(void)
{
    const bool guaranteed = expected_build_tier != JCE_GRAPHICS_API_TIER_COMPATIBILITY;
    TEST_ASSERT_EQUAL_INT(JCE_GPU_VFETCH_UNANSWERED,
        jce_gpu_vertex_fetch_state(JCE_GPU_VFETCH_FMT_RGBA32F));
    TEST_ASSERT_EQUAL_INT(guaranteed, jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_RGBA32F));
    fixture_caps_available = false;
    TEST_ASSERT_EQUAL_INT(JCE_GPU_VFETCH_UNANSWERED, jce_gpu_vertex_fetch_state(JCE_GPU_VFETCH_FMT_R32F));
    TEST_ASSERT_EQUAL_INT(guaranteed, jce_gpu_vertex_fetch_usable(JCE_GPU_VFETCH_FMT_R32F));
}
int main(void)
{
    initial_build_tier = jce_renderer_get_api_info().build_tier;
    UNITY_BEGIN();
    RUN_TEST(append_only_tier_identity_and_minima);
    RUN_TEST(each_build_rejects_below_and_accepts_its_exact_floor);
    RUN_TEST(compatibility_query_cannot_lower_the_compiled_policy);
    RUN_TEST(backend_switch_cannot_carry_a_different_drivers_observation);
    RUN_TEST(unparseable_versions_remain_explicitly_unverified);
    RUN_TEST(explicit_vertex_fetch_answers_override_every_build_policy);
    RUN_TEST(compatibility_unanswered_vertex_fetch_is_not_a_hardware_yes);
    return UNITY_END();
}
