/* Actual startup policy and full apply owner with typed CPU ports.
 * These controls do not initialise a graphics backend or measure memory. */
#include "unity.h"
#include "renderer/jce_renderer_budget_internal.h"
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_trace.h>
#include <string.h>

typedef struct bgfx_init_s {
    void *allocator;
    struct {
        uint32_t maxTransientVbSize;
        uint32_t maxTransientIbSize;
        uint32_t minUniformBufferSize;
        uint16_t maxEncoders;
    } limits;
} bgfx_init_t;

static struct {
    int ram_mb, cores;
    JceMachineClass machine_class;
    const char *low_memory, *vertex_mb, *index_mb;
    unsigned allocator_calls, ram_calls, core_calls, class_calls, env_calls;
    unsigned warnings, counters;
    unsigned char allocator_token;
    const char *counter_names[4];
    int64_t counter_values[4];
    const char *last_warning;
} port;

static void *counted_allocator(void)
{
    ++port.allocator_calls;
    return &port.allocator_token;
}
static int counted_ram(void) { ++port.ram_calls; return port.ram_mb; }
static int counted_cores(void) { ++port.core_calls; return port.cores; }
static JceMachineClass counted_class(void)
{
    ++port.class_calls;
    return port.machine_class;
}
static const char *counted_env(const char *name)
{
    ++port.env_calls;
    if (!strcmp(name, "JCE_LOW_MEM")) return port.low_memory;
    if (!strcmp(name, "JCE_TRANSIENT_VB_MB")) return port.vertex_mb;
    if (!strcmp(name, "JCE_TRANSIENT_IB_MB")) return port.index_mb;
    TEST_FAIL_MESSAGE("Actual startup owner queried an undeclared environment port");
    return NULL;
}
static void counted_warning(const char *tag, const char *format, ...)
{
    TEST_ASSERT_EQUAL_STRING("jce_renderer", tag);
    ++port.warnings;
    port.last_warning = format;
}
static void counted_counter(JceTraceCategory category, const char *name, int64_t value)
{
    TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, category);
    TEST_ASSERT_LESS_THAN_UINT(4, port.counters);
    port.counter_names[port.counters] = name;
    port.counter_values[port.counters++] = value;
}

#define LOG_TAG "jce_renderer"
#define LOG_WARN counted_warning
#define jce_bgfx_allocator counted_allocator
#define SDL_GetSystemRAM counted_ram
#define SDL_GetNumLogicalCPUCores counted_cores
#define jce_config_machine_class counted_class
#define getenv counted_env
#define jce_trace_counter counted_counter
#include JCE_RENDERER_BUDGET_APPLY_OWNER
#undef jce_trace_counter
#undef getenv
#undef jce_config_machine_class
#undef SDL_GetNumLogicalCPUCores
#undef SDL_GetSystemRAM
#undef jce_bgfx_allocator
#undef LOG_WARN
#undef LOG_TAG

void setUp(void)
{
    memset(&port, 0, sizeof port);
    port.ram_mb = 8192;
    port.cores = 32;
    port.machine_class = JCE_MACHINE_CLASS_AUTO;
}
void tearDown(void) {}

static bgfx_init_t seeded_init(void)
{
    bgfx_init_t init;
    memset(&init, 0, sizeof init);
    /* The missing-wire control retains the former 1 MiB initial limit. */
    init.limits.minUniformBufferSize = 1048576u;
    return init;
}
static void assert_actual_limits(const bgfx_init_t *init, bool low_memory)
{
    const JceRendererPoolBudget expected = jce_renderer_pool_budget(
        JCE_PLATFORM_WEB != 0, low_memory, port.cores,
        jce_renderer_pool_mb(port.vertex_mb), jce_renderer_pool_mb(port.index_mb));
    TEST_ASSERT_EQUAL_PTR(&port.allocator_token, init->allocator);
    TEST_ASSERT_EQUAL_UINT32(expected.vertex_bytes, init->limits.maxTransientVbSize);
    TEST_ASSERT_EQUAL_UINT32(expected.index_bytes, init->limits.maxTransientIbSize);
    TEST_ASSERT_EQUAL_UINT16(expected.encoders, init->limits.maxEncoders);
    TEST_ASSERT_EQUAL_UINT32(128u * 1024u, init->limits.minUniformBufferSize);
    TEST_ASSERT_EQUAL_UINT(1, port.allocator_calls);
    TEST_ASSERT_EQUAL_UINT(1, port.ram_calls);
    TEST_ASSERT_EQUAL_UINT(1, port.core_calls);
    TEST_ASSERT_EQUAL_UINT(1, port.class_calls);
    TEST_ASSERT_EQUAL_UINT(3, port.env_calls);
}

static void test_web_does_not_inherit_desktop_editor_pool_sizes(void)
{
    JceRendererPoolBudget b = jce_renderer_pool_budget(true, false, 32, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(6u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(2u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(4, b.encoders);
    b = jce_renderer_pool_budget(true, true, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(6u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(2u*1048576u, b.index_bytes);
}
static void test_native_default_capacity_keeps_encoder_headroom(void)
{
    JceRendererPoolBudget b = jce_renderer_pool_budget(false, false, 32, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(8u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(2u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(16, b.encoders);
    b = jce_renderer_pool_budget(false, true, 1, 0, 0);
    TEST_ASSERT_EQUAL_UINT32(6u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(2u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(4, b.encoders);
}
static void test_native_large_workloads_keep_explicit_pool_capacity(void)
{
    JceRendererPoolBudget b = jce_renderer_pool_budget(false, false, 32,
        jce_renderer_pool_mb("32"), jce_renderer_pool_mb("8"));
    TEST_ASSERT_EQUAL_UINT32(32u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(8u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(16, b.encoders);
    b = jce_renderer_pool_budget(false, true, 1,
        jce_renderer_pool_mb("32"), jce_renderer_pool_mb("8"));
    TEST_ASSERT_EQUAL_UINT32(32u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(8u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(4, b.encoders);
}
static void test_explicit_budget_changes_capacity_not_encoder_policy(void)
{
    JceRendererPoolBudget b = jce_renderer_pool_budget(true, false, 32,
        jce_renderer_pool_mb("32"), jce_renderer_pool_mb("8"));
    TEST_ASSERT_EQUAL_UINT32(32u*1048576u, b.vertex_bytes);
    TEST_ASSERT_EQUAL_UINT32(8u*1048576u, b.index_bytes);
    TEST_ASSERT_EQUAL_UINT16(4, b.encoders);
}
static void test_invalid_budget_uses_default_without_overflow(void)
{
    const char *invalid[] = {NULL,"","0","-1","+1"," 6","6 ","6MiB","257",
                             "999999999999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_pool_mb(invalid[i]));
    TEST_ASSERT_EQUAL_UINT32(1048576u, jce_renderer_pool_mb("1"));
    TEST_ASSERT_EQUAL_UINT32(268435456u, jce_renderer_pool_mb("256"));
    TEST_ASSERT_EQUAL_UINT32(6291456u, jce_renderer_pool_mb("006"));
}

static void test_initial_uniform_capacity_preserves_encoder_concurrency(void)
{
    for (unsigned web = 0; web < 2; ++web) {
        for (unsigned low_memory = 0; low_memory < 2; ++low_memory) {
            const JceRendererPoolBudget b = jce_renderer_pool_budget(
                web != 0, low_memory != 0, 32, 32u * 1048576u, 8u * 1048576u);
            TEST_ASSERT_EQUAL_UINT32(128u * 1024u, b.uniform_bytes);
            TEST_ASSERT_EQUAL_UINT16((web || low_memory) ? 4 : 16, b.encoders);
        }
    }
    const JceRendererPoolBudget native = jce_renderer_pool_budget(false, false, 32, 0, 0);
    /* Two native bgfx Frames: logical initial capacity, not heap or RSS. */
    TEST_ASSERT_EQUAL_UINT32(4u * 1048576u, 2u * native.encoders * native.uniform_bytes);
}
static void test_actual_apply_wires_initial_uniform_limit_without_changing_pools(void)
{
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, false);
    TEST_ASSERT_EQUAL_UINT(0, port.warnings);
}
static void test_actual_apply_counters_match_all_requested_limits(void)
{
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    TEST_ASSERT_EQUAL_UINT(4, port.counters);
    const char *names[] = {"renderer.transient_vb_capacity_bytes",
        "renderer.transient_ib_capacity_bytes", "renderer.uniform_initial_encoder_frame_bytes",
        "renderer.encoder_capacity"};
    const uint32_t values[] = {init.limits.maxTransientVbSize, init.limits.maxTransientIbSize,
        init.limits.minUniformBufferSize, init.limits.maxEncoders};
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_STRING(names[i], port.counter_names[i]);
        TEST_ASSERT_EQUAL_INT64(values[i], port.counter_values[i]);
    }
    TEST_ASSERT_EQUAL_INT64(128u * 1024u, port.counter_values[2]);
}
static void test_actual_apply_machine_class_and_environment_keep_precedence(void)
{
    port.ram_mb = 512;
    port.machine_class = JCE_MACHINE_CLASS_FULL;
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, false);

    setUp();
    port.machine_class = JCE_MACHINE_CLASS_LOW;
    port.low_memory = "0";
    init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, false);

    setUp();
    port.machine_class = JCE_MACHINE_CLASS_FULL;
    port.low_memory = "1";
    init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, true);
}
static void test_actual_apply_low_memory_and_unknown_ram_use_existing_core_policy(void)
{
    port.ram_mb = 512;
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, true);

    setUp();
    port.ram_mb = 0;
    port.cores = 1;
    init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, true);

    setUp();
    port.ram_mb = 0;
    port.cores = 2;
    init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, false);
    TEST_ASSERT_EQUAL_UINT16(4, init.limits.maxEncoders);
}
static void test_actual_apply_explicit_transient_overrides_do_not_enlarge_uniforms(void)
{
    port.low_memory = "1";
    port.vertex_mb = "32";
    port.index_mb = "8";
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, true);
    TEST_ASSERT_EQUAL_UINT32(32u * 1048576u, init.limits.maxTransientVbSize);
    TEST_ASSERT_EQUAL_UINT32(8u * 1048576u, init.limits.maxTransientIbSize);
    TEST_ASSERT_EQUAL_UINT(0, port.warnings);
}
static void test_actual_apply_invalid_transient_overrides_warn_and_keep_default_uniforms(void)
{
    port.vertex_mb = "257";
    port.index_mb = "999999999999999999999999999999999";
    bgfx_init_t init = seeded_init();
    apply_transient_limits(&init);
    assert_actual_limits(&init, false);
    TEST_ASSERT_EQUAL_UINT(2, port.warnings);
    TEST_ASSERT_EQUAL_STRING("JCE_TRANSIENT_IB_MB must be 1..256; using default", port.last_warning);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_web_does_not_inherit_desktop_editor_pool_sizes);
    RUN_TEST(test_native_default_capacity_keeps_encoder_headroom);
    RUN_TEST(test_native_large_workloads_keep_explicit_pool_capacity);
    RUN_TEST(test_explicit_budget_changes_capacity_not_encoder_policy);
    RUN_TEST(test_invalid_budget_uses_default_without_overflow);
    RUN_TEST(test_initial_uniform_capacity_preserves_encoder_concurrency);
    RUN_TEST(test_actual_apply_wires_initial_uniform_limit_without_changing_pools);
    RUN_TEST(test_actual_apply_counters_match_all_requested_limits);
    RUN_TEST(test_actual_apply_machine_class_and_environment_keep_precedence);
    RUN_TEST(test_actual_apply_low_memory_and_unknown_ram_use_existing_core_policy);
    RUN_TEST(test_actual_apply_explicit_transient_overrides_do_not_enlarge_uniforms);
    RUN_TEST(test_actual_apply_invalid_transient_overrides_warn_and_keep_default_uniforms);
    return UNITY_END();
}
