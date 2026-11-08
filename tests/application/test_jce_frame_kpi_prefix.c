/* Complete shipping encoder, typed format/allocation ports, no graphics. */
#include "unity.h"
#include "application/jce_frame_kpi.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_timer.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned formats, integer_formats, reallocations, frees, live_blocks;
    unsigned fail_reallocation;
    int prefix_result;
    bool force_prefix_result;
} EncoderPort;
static EncoderPort ports[2];
static unsigned allocation_owner;

/* Declarations above precede all ports; the real CRT formatter still owns
 * floating-point precision/rounding and the real JCE allocator owns memory. */
static int format_forward(unsigned owner, char *out, size_t capacity,
                           const char *format, va_list args)
{
    EncoderPort *port = &ports[owner];
    ++port->formats;
    if (strstr(format, "%llu")) ++port->integer_formats;
    if (port->force_prefix_result && strcmp(format, "%llu,") == 0)
        return port->prefix_result;
    return vsnprintf(out, capacity, format, args);
}
static int current_format(char *out, size_t capacity, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = format_forward(0, out, capacity, format, args);
    va_end(args); return result;
}
static int old_format(char *out, size_t capacity, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = format_forward(1, out, capacity, format, args);
    va_end(args); return result;
}
static void *encoder_reallocate(void *ptr, size_t size)
{
    EncoderPort *port = &ports[allocation_owner];
    if (++port->reallocations == port->fail_reallocation) return NULL;
    void *result = jce__realloc_tracked(ptr, size);
    if (result && !ptr) ++port->live_blocks;
    return result;
}
static void encoder_free(void *ptr)
{
    EncoderPort *port = &ports[allocation_owner];
    if (ptr) { ++port->frees; --port->live_blocks; }
    jce__free_tracked(ptr);
}
#undef JCE_REALLOC
#undef JCE_FREE
#define JCE_REALLOC(ptr, size) encoder_reallocate((ptr), (size))
#define JCE_FREE(ptr) encoder_free(ptr)
#define snprintf current_format
#include JCE_TEST_FRAME_KPI_OWNER
#undef snprintf

/* The second complete owner is the exact old-per-row mutation generated from
 * the same source. Renaming private/public symbols keeps it a CPU reference,
 * not a second shipping implementation or new test-only production API. */
/* The guarded shared header is already included: retain the typed forward
 * declaration needed by the renamed legacy encode wrapper. */
bool old_jce_frame_kpi_encode_with_context(JceFrameKpiBuffer *buffer, uint64_t frame,
    double wall_ms, const JceGpuStats *stats, bool phases_eligible,
    const JceFrameKpiContext *context, size_t *out_size);
#define append_row old_append_row
#define context_uint old_context_uint
#define context_size old_context_size
#define context_suffix old_context_suffix
#define viewport_size_valid old_viewport_size_valid
#define jce_frame_kpi_viewport_valid old_jce_frame_kpi_viewport_valid
#define jce_frame_kpi_context_report old_jce_frame_kpi_context_report
#define context_suffix_v2 old_context_suffix_v2
#define context_suffix_v3 old_context_suffix_v3
#define encode_rows old_encode_rows
#define jce_frame_kpi_csv_header old_jce_frame_kpi_csv_header
#define jce_frame_kpi_encode old_jce_frame_kpi_encode
#define jce_frame_kpi_encode_with_context old_jce_frame_kpi_encode_with_context
#define jce_frame_kpi_export old_jce_frame_kpi_export
#define jce_frame_kpi_buffer_destroy old_jce_frame_kpi_buffer_destroy
#define snprintf old_format
#include JCE_TEST_FRAME_KPI_OLD_OWNER
#undef snprintf
#undef append_row
#undef context_uint
#undef context_size
#undef context_suffix
#undef viewport_size_valid
#undef jce_frame_kpi_viewport_valid
#undef jce_frame_kpi_context_report
#undef context_suffix_v2
#undef context_suffix_v3
#undef encode_rows
#undef jce_frame_kpi_csv_header
#undef jce_frame_kpi_encode
#undef jce_frame_kpi_encode_with_context
#undef jce_frame_kpi_export
#undef jce_frame_kpi_buffer_destroy
#undef JCE_REALLOC
#undef JCE_FREE

static char phase_names[56][64];
static char long_name[9001];
static void phase_snapshot(bool long_row)
{
    jce_perf_phase_set_enabled(1);
    /* Pre-register the encoder's observations before sealing the snapshot. */
    jce_perf_phase_add("kpi_encode", 0);
    jce_perf_phase_add("kpi_reserve", 0);
    jce_perf_phase_add("kpi_snapshot_encode_write", 0);
    jce_perf_phase_add("kpi_write", 0);
    for (unsigned index = 0; index < 56; ++index) {
        (void)snprintf(phase_names[index], sizeof phase_names[index],
                       "phase_%02u_complete_name", index);
        jce_perf_phase_add(phase_names[index], (double)index + .125);
    }
    if (long_row) {
        memset(long_name, 'x', sizeof long_name - 1u);
        long_name[sizeof long_name - 1u] = 0;
        jce_perf_phase_add(long_name, 2.25);
    }
    jce_perf_phase_frame_tick();
}
static JceGpuStats gpu_stats(void)
{
    JceGpuStats stats = {0};
    stats.valid = true; stats.cpu_frame_ms = 2.5; stats.gpu_ms = .125;
    return stats;
}
static JceFrameKpiContext context_value(void)
{
    JceFrameKpiContext context = {0};
    context.window_present = true;
    context.flags_begin_valid = context.flags_end_valid = true;
    context.flags_begin = 0; context.flags_end = UINT32_MAX;
    context.point_width = 1280; context.point_height = 720;
    context.drawable_width = context.native_width_begin = context.native_width_end = 2560;
    context.drawable_height = context.native_height_begin = context.native_height_end = 1440;
    context.backend_frame_begin_valid = context.backend_frame_end_valid = true;
    context.backend_frame_begin = 0; context.backend_frame_end = UINT32_MAX;
    return context;
}
static bool encode(unsigned owner, JceFrameKpiBuffer *buffer, uint64_t frame,
                    double wall, const JceGpuStats *stats, bool phases,
                    const JceFrameKpiContext *context, size_t *size)
{
    allocation_owner = owner;
    return owner ? old_jce_frame_kpi_encode_with_context(buffer, frame, wall, stats, phases, context, size)
                 : jce_frame_kpi_encode_with_context(buffer, frame, wall, stats, phases, context, size);
}
static void destroy(unsigned owner, JceFrameKpiBuffer *buffer)
{
    allocation_owner = owner;
    if (owner) old_jce_frame_kpi_buffer_destroy(buffer);
    else jce_frame_kpi_buffer_destroy(buffer);
}
void setUp(void)
{
    memset(ports, 0, sizeof ports); allocation_owner = 0;
    jce_trace_set_enabled(false); jce_perf_phase_set_enabled(0);
}
void tearDown(void) { jce_perf_phase_set_enabled(0); }

static void test_frame_prefix_once_preserves_integer_boundaries_and_reuse(void)
{
    const uint64_t values[] = {0, 9, 10, UINT32_MAX, UINT64_MAX};
    JceFrameKpiBuffer current = {0}, old = {0};
    JceGpuStats stats = gpu_stats();
    bool all_equal = true, converted_once = true;
    for (unsigned index = 0; index < sizeof values / sizeof values[0]; ++index) {
        size_t a = 0, b = 0; unsigned before = ports[0].integer_formats;
        bool first = encode(0, &current, values[index], -0.0, &stats, false, NULL, &a);
        bool second = encode(1, &old, values[index], -0.0, &stats, false, NULL, &b);
        bool equal = first && second && a == b && memcmp(current.bytes, old.bytes, a + 1u) == 0;
        all_equal = all_equal && equal;
        converted_once = converted_once && ports[0].integer_formats - before == 1;
    }
    destroy(0, &current); destroy(1, &old);
    TEST_ASSERT_TRUE(all_equal);
    TEST_ASSERT_TRUE_MESSAGE(converted_once,
        "frame index was converted more than once in one complete record");
    TEST_ASSERT_EQUAL_UINT(0, ports[0].live_blocks); TEST_ASSERT_EQUAL_UINT(0, ports[1].live_blocks);
}
static void test_complete_phase_context_and_long_row_match_old_bytes(void)
{
    phase_snapshot(true);
    JceFrameKpiBuffer current = {0}, old = {0};
    JceGpuStats stats = gpu_stats(); JceFrameKpiContext context = context_value();
    size_t a = 0, b = 0;
    bool first = encode(0, &current, UINT64_MAX, 1.23455, &stats, true, &context, &a);
    bool second = encode(1, &old, UINT64_MAX, 1.23455, &stats, true, &context, &b);
    bool equal = first && second && a == b && memcmp(current.bytes, old.bytes, a + 1u) == 0;
    bool full_name = first && strstr(current.bytes, long_name) != NULL;
    unsigned conversions = ports[0].integer_formats, old_conversions = ports[1].integer_formats;
    size_t capacity = current.capacity;
    destroy(0, &current); destroy(1, &old);
    TEST_ASSERT_TRUE(equal); TEST_ASSERT_TRUE(full_name);
    TEST_ASSERT_GREATER_THAN_UINT(8192, capacity);
    TEST_ASSERT_EQUAL_UINT(1, conversions);
    TEST_ASSERT_GREATER_THAN_UINT(60, old_conversions);
    TEST_ASSERT_EQUAL_UINT(0, ports[0].live_blocks); TEST_ASSERT_EQUAL_UINT(0, ports[1].live_blocks);
}
static void test_exact_budget_and_one_byte_short_reject_without_success(void)
{
    JceFrameKpiBuffer reference = {0}; size_t expected = 0;
    JceGpuStats stats = gpu_stats(); JceFrameKpiContext context = context_value();
    TEST_ASSERT_TRUE(encode(1, &reference, UINT64_MAX, .125, &stats, false, &context, &expected));
    JceFrameKpiBuffer exact = {0}, short_buffer = {0};
    exact.max_capacity = expected + 1u; short_buffer.max_capacity = expected;
    size_t a = 999, b = 999;
    bool accepted = encode(0, &exact, UINT64_MAX, .125, &stats, false, &context, &a);
    bool equal = accepted && a == expected && memcmp(exact.bytes, reference.bytes, a + 1u) == 0;
    bool refused = !encode(0, &short_buffer, UINT64_MAX, .125, &stats, false, &context, &b);
    size_t bounded = short_buffer.capacity;
    destroy(0, &exact); destroy(0, &short_buffer); destroy(1, &reference);
    TEST_ASSERT_TRUE(equal); TEST_ASSERT_TRUE(refused); TEST_ASSERT_EQUAL_UINT(0, b);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(expected, bounded);
    TEST_ASSERT_EQUAL_UINT(0, ports[0].live_blocks); TEST_ASSERT_EQUAL_UINT(0, ports[1].live_blocks);
}
static void test_prefix_formatter_refusal_retains_owned_scratch(void)
{
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    TEST_ASSERT_TRUE(encode(0, &buffer, 7, 1.0, NULL, false, NULL, &size));
    char *bytes = buffer.bytes; size_t capacity = buffer.capacity;
    const int results[] = {-1, 22};
    bool rejected[2], retained[2];
    for (unsigned index = 0; index < 2; ++index) {
        ports[0].force_prefix_result = true; ports[0].prefix_result = results[index]; size = 999;
        rejected[index] = !encode(0, &buffer, 8, 2.0, NULL, false, NULL, &size) && size == 0;
        retained[index] = bytes == buffer.bytes && capacity == buffer.capacity &&
                          strcmp(buffer.bytes, "7,frame_wall,1.0000\n") == 0;
    }
    destroy(0, &buffer);
    TEST_ASSERT_TRUE(rejected[0]); TEST_ASSERT_TRUE(rejected[1]);
    TEST_ASSERT_TRUE(retained[0]); TEST_ASSERT_TRUE(retained[1]);
    TEST_ASSERT_EQUAL_UINT(0, ports[0].live_blocks);
}
static void test_initial_and_growth_oom_reject_and_keep_single_owner(void)
{
    JceFrameKpiBuffer first = {0}; size_t size = 999;
    ports[0].fail_reallocation = 1;
    bool refused_first = !encode(0, &first, 3, 1.0, NULL, false, NULL, &size) && size == 0;
    bool no_first_block = !first.bytes && first.capacity == 0;
    destroy(0, &first); ports[0].fail_reallocation = 0;
    JceFrameKpiBuffer growth = {0};
    TEST_ASSERT_TRUE(encode(0, &growth, 4, 1.0, NULL, false, NULL, &size));
    char *before = growth.bytes; size_t capacity = growth.capacity;
    phase_snapshot(true);
    ports[0].fail_reallocation = ports[0].reallocations + 1u; size = 999;
    bool refused_growth = !encode(0, &growth, UINT64_MAX, 1.0, NULL, true, NULL, &size) && size == 0;
    bool owner_retained = before == growth.bytes && capacity == growth.capacity;
    destroy(0, &growth);
    TEST_ASSERT_TRUE(refused_first); TEST_ASSERT_TRUE(no_first_block);
    TEST_ASSERT_TRUE(refused_growth); TEST_ASSERT_TRUE(owner_retained);
    TEST_ASSERT_EQUAL_UINT(0, ports[0].live_blocks);
}

typedef struct { double ms; unsigned integer_formats, formats, reallocations; bool equal; } Batch;
static Batch benchmark_batch(unsigned owner, JceFrameKpiBuffer *buffer,
    uint64_t frame, const JceGpuStats *stats, const JceFrameKpiContext *context,
    const char *expected, size_t expected_size)
{
    Batch batch = {0}; batch.equal = true;
    EncoderPort before = ports[owner];
    uint64_t begin = jce_time_perf_counter();
    for (unsigned index = 0; index < 256; ++index) {
        size_t size = 0;
        if (!encode(owner, buffer, frame, 1.23455, stats, true, context, &size) ||
            size != expected_size || memcmp(buffer->bytes, expected, size + 1u) != 0)
            batch.equal = false;
    }
    batch.ms = jce_time_perf_to_ms(begin, jce_time_perf_counter());
    batch.integer_formats = ports[owner].integer_formats - before.integer_formats;
    batch.formats = ports[owner].formats - before.formats;
    batch.reallocations = ports[owner].reallocations - before.reallocations;
    return batch;
}
static int benchmark(void)
{
    setUp(); phase_snapshot(false);
    JceFrameKpiBuffer current = {0}, old = {0};
    JceGpuStats stats = gpu_stats(); JceFrameKpiContext context = context_value();
    bool ok = true;
    printf("KPI_PREFIX_BENCHMARK model=encode-plus-exact-memcmp pairs=12 records_per_group=256 "
           "trace=0 phase=1 format_ports=1 speed_threshold=none\n");
    for (unsigned pair = 0; pair < 12; ++pair) {
        uint64_t frame = pair < 6 ? UINT64_C(1000) : UINT64_MAX;
        size_t size = 0, current_size = 0;
        if (!encode(1, &old, frame, 1.23455, &stats, true, &context, &size) ||
            !encode(0, &current, frame, 1.23455, &stats, true, &context, &current_size) ||
            size != current_size || memcmp(old.bytes, current.bytes, size + 1u) != 0) { ok = false; break; }
        /* Both references use real JCE storage. This immutable comparison
         * buffer lies outside the timed groups and contains the whole record. */
        char *expected = jce__malloc_tracked(size + 1u);
        if (!expected) { ok = false; break; }
        memcpy(expected, old.bytes, size + 1u);
        Batch a, b;
        if (pair % 2u == 0) {
            a = benchmark_batch(0, &current, frame, &stats, &context, expected, size);
            b = benchmark_batch(1, &old, frame, &stats, &context, expected, size);
        } else {
            b = benchmark_batch(1, &old, frame, &stats, &context, expected, size);
            a = benchmark_batch(0, &current, frame, &stats, &context, expected, size);
        }
        jce__free_tracked(expected);
        bool equal = a.equal && b.equal;
        bool counts = a.integer_formats == 256 && b.integer_formats > 256 &&
                      a.reallocations == 0 && b.reallocations == 0;
        ok = ok && equal && counts;
        printf("KPI_PREFIX_PAIR pair=%u order=%s records=256 frame=%llu bytes=%llu "
               "equal=%u current_ms=%.6f old_ms=%.6f current_uint64=%u old_uint64=%u "
               "current_formats=%u old_formats=%u current_realloc=%u old_realloc=%u\n",
            pair, pair % 2u ? "old,current" : "current,old", (unsigned long long)frame,
            (unsigned long long)size, (unsigned)equal, a.ms, b.ms, a.integer_formats,
            b.integer_formats, a.formats, b.formats, a.reallocations, b.reallocations);
    }
    destroy(0, &current); destroy(1, &old); tearDown();
    return ok && !ports[0].live_blocks && !ports[1].live_blocks ? 0 : 1;
}
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0) return benchmark();
    UNITY_BEGIN();
    RUN_TEST(test_frame_prefix_once_preserves_integer_boundaries_and_reuse);
    RUN_TEST(test_complete_phase_context_and_long_row_match_old_bytes);
    RUN_TEST(test_exact_budget_and_one_byte_short_reject_without_success);
    RUN_TEST(test_prefix_formatter_refusal_retains_owned_scratch);
    RUN_TEST(test_initial_and_growth_oom_reject_and_keep_single_owner);
    return UNITY_END();
}
