/* Exact shared codec bodies, native forwarding observations and old byte oracle. */
#include "unity.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_capture_internal.h"

static uint64_t copy_calls, strlen_calls, format_calls;
static void *counted_copy(void *dst, const void *src, size_t size)
{
    ++copy_calls; return SDL_memcpy(dst, src, size);
}
static size_t counted_strlen(const char *text)
{
    ++strlen_calls; return SDL_strlen(text);
}
static int counted_format(char *dst, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format); ++format_calls;
    int result = SDL_vsnprintf(dst, size, format, args);
    va_end(args); return result;
}
#define SDL_memcpy counted_copy
#define SDL_strlen counted_strlen
#define SDL_snprintf counted_format
#define jce_trace_chrome_encode_batch_into measured_encode_into
#include JCE_TEST_TRACE_JSON_CODEC_OWNER
#undef jce_trace_chrome_encode_batch_into
#undef SDL_snprintf
#undef SDL_strlen
#undef SDL_memcpy

bool old_reference_encode_into(const JceTraceEvent *, uint32_t, bool *, TraceJsonBuffer *);
bool current_reference_encode_into(const JceTraceEvent *, uint32_t, bool *, TraceJsonBuffer *);

void setUp(void) { copy_calls = strlen_calls = format_calls = 0; }
void tearDown(void) {}

static JceTraceEvent event_for(unsigned type)
{
    JceTraceEvent event = {0};
    event.type = (JceTraceEventType)type; event.category = JCE_TRACE_CATEGORY_RENDER;
    event.timestamp_ns = UINT64_MAX; event.thread_id = UINT64_MAX;
    event.id = UINT64_MAX; event.parent_id = UINT64_MAX;
    event.value_u64 = UINT64_MAX; event.value_u32[0] = UINT32_MAX;
    event.value_u32[1] = UINT32_MAX; return event;
}
static void equal_batch(const JceTraceEvent *events, uint32_t count, bool first, size_t limit)
{
    TraceJsonBuffer actual = {0}, old = {0};
    bool old_first = first;
    actual.limit = old.limit = limit;
    bool ok = measured_encode_into(events, count, &first, &actual);
    bool was_ok = old_reference_encode_into(events, count, &old_first, &old);
    TEST_ASSERT_EQUAL(was_ok, ok); TEST_ASSERT_EQUAL(old_first, first);
    TEST_ASSERT_EQUAL_UINT64(old.size, actual.size);
    if (ok || actual.data) {
        TEST_ASSERT_NOT_NULL(actual.data); TEST_ASSERT_NOT_NULL(old.data);
        TEST_ASSERT_EQUAL_MEMORY(old.data, actual.data, old.size + 1u);
    }
    JCE_FREE(actual.data); JCE_FREE(old.data);
}
static void test_every_nonzero_name_byte_preserves_original_wire_bytes(void)
{
    for (unsigned type = JCE_TRACE_EVENT_THREAD_REGISTER; type <= JCE_TRACE_EVENT_COUNTER; ++type) {
        for (unsigned value = 1; value <= 255; ++value) {
            JceTraceEvent event = event_for(type);
            event.name[0] = (char)value;
            equal_batch(&event, 1, true, 0); equal_batch(&event, 1, false, 0);
        }
    }
}
static void test_max_names_empty_and_first_nul_preserve_original_bytes(void)
{
    JceTraceEvent events[10];
    for (unsigned i = 0; i < 10; ++i) {
        events[i] = event_for(i); // Includes the existing default event kind.
        memset(events[i].name, (int)('A' + i), JCE_TRACE_NAME_CAP - 1u);
    }
    equal_batch(events, 10, true, 0);
    for (unsigned i = 0; i < 10; ++i) {
        for (unsigned j = 0; j < JCE_TRACE_NAME_CAP - 1u; ++j)
            events[i].name[j] = (char)(1u + (j * 7u + i) % 255u);
    }
    equal_batch(events, 10, true, 0);
    for (unsigned i = 0; i < 10; ++i) {
        events[i].name[0] = 0;
        memset(events[i].name + 1, 1, sizeof(events[i].name) - 1u);
    }
    equal_batch(events, 10, true, 0);
}
static void test_limits_and_invalid_tail_keep_complete_batch_transaction(void)
{
    JceTraceEvent events[3];
    for (unsigned i = 0; i < 3; ++i) {
        events[i] = event_for(JCE_TRACE_EVENT_TASK_BEGIN + i);
        strcpy(events[i].name, "ordinary/\"quoted\\name\n");
    }
    for (size_t limit = 1; limit <= 1024; ++limit)
        equal_batch(events, 3, true, limit);
    memset(events[2].name, 'x', sizeof(events[2].name));
    equal_batch(events, 3, true, 0);
    TraceJsonBuffer actual = {0}; bool first = true;
    TEST_ASSERT_TRUE(measured_encode_into(events, 1, &first, &actual));
    const size_t previous = actual.size;
    char saved[1024]; TEST_ASSERT_TRUE(previous + 1u <= sizeof(saved));
    memcpy(saved, actual.data, previous + 1u);
    TEST_ASSERT_FALSE(measured_encode_into(events, 3, &first, &actual));
    TEST_ASSERT_FALSE(first); TEST_ASSERT_EQUAL_UINT64(previous, actual.size);
    TEST_ASSERT_EQUAL_MEMORY(saved, actual.data, previous + 1u);
    JCE_FREE(actual.data);
}
static void test_reused_buffer_and_invalid_arguments_remain_unchanged(void)
{
    TraceJsonBuffer actual = {0}, old = {0}; bool first = true, old_first = true;
    JceTraceEvent event = event_for(JCE_TRACE_EVENT_COUNTER);
    strcpy(event.name, "reused-name");
    for (unsigned i = 0; i < 128; ++i) {
        TEST_ASSERT_TRUE(measured_encode_into(&event, 1, &first, &actual));
        TEST_ASSERT_TRUE(old_reference_encode_into(&event, 1, &old_first, &old));
        TEST_ASSERT_EQUAL_UINT64(old.size, actual.size);
        TEST_ASSERT_EQUAL_MEMORY(old.data, actual.data, old.size + 1u);
    }
    TEST_ASSERT_FALSE(measured_encode_into(NULL, 1, &first, &actual));
    TEST_ASSERT_FALSE(measured_encode_into(&event, 1, NULL, &actual));
    TEST_ASSERT_FALSE(measured_encode_into(&event, 1, &first, NULL));
    TraceJsonBuffer bad = {NULL, 1, 0, 0};
    TEST_ASSERT_FALSE(measured_encode_into(&event, 1, &first, &bad));
    bad.size = 0; bad.capacity = 1;
    TEST_ASSERT_FALSE(measured_encode_into(&event, 1, &first, &bad));
    JCE_FREE(actual.data); JCE_FREE(old.data);
}
static void test_plain_names_use_bounded_span_copy_and_no_literal_formatting(void)
{
    JceTraceEvent witness = event_for(JCE_TRACE_EVENT_COUNTER);
    memset(witness.name, 'x', JCE_TRACE_NAME_CAP - 1u);
    TraceJsonBuffer witness_json = {0}; bool witness_first = true;
    copy_calls = strlen_calls = format_calls = 0;
    TEST_ASSERT_TRUE(measured_encode_into(&witness, 1, &witness_first, &witness_json));
    printf("CODEC_CALLS name_bytes=47 copy=%llu strlen=%llu format=%llu\n",
        (unsigned long long)copy_calls, (unsigned long long)strlen_calls,
        (unsigned long long)format_calls);
    JCE_FREE(witness_json.data);
    for (unsigned length = 1; length < JCE_TRACE_NAME_CAP; ++length) {
        JceTraceEvent event = event_for(JCE_TRACE_EVENT_COUNTER);
        memset(event.name, 'x', length);
        TraceJsonBuffer actual = {0}; bool first = true;
        copy_calls = strlen_calls = format_calls = 0;
        TEST_ASSERT_TRUE(measured_encode_into(&event, 1, &first, &actual));
        // These count explicit real codec calls, not hidden CRT allocator work.
        TEST_ASSERT_EQUAL_UINT64(1, format_calls);
        TEST_ASSERT_EQUAL_UINT64(6, copy_calls);
        TEST_ASSERT_EQUAL_UINT64(4, strlen_calls);
        JCE_FREE(actual.data);
    }
}

static void test_explicit_length_append_refuses_overflow_before_touching_source(void)
{
    TraceJsonBuffer json = {0};
    TEST_ASSERT_FALSE(trace_json_append_n(&json, "", SIZE_MAX));
    TEST_ASSERT_NULL(json.data); TEST_ASSERT_EQUAL_UINT64(0, json.size);
    char sentinel = 's';
    TraceJsonBuffer overflow = {&sentinel, SIZE_MAX - 1u, SIZE_MAX, 0};
    TEST_ASSERT_FALSE(trace_json_append_n(&overflow, "xx", 2));
    TEST_ASSERT_EQUAL_CHAR('s', sentinel);
    TEST_ASSERT_EQUAL_UINT64(SIZE_MAX - 1u, overflow.size);
    TEST_ASSERT_TRUE(trace_json_append_n(&json, "", 0));
    TEST_ASSERT_EQUAL_UINT64(0, json.size); TEST_ASSERT_EQUAL_CHAR(0, json.data[0]);
    JCE_FREE(json.data);
}

static void equal_integer_value(uint64_t value)
{
    for (unsigned type = JCE_TRACE_EVENT_THREAD_REGISTER; type <= JCE_TRACE_EVENT_COUNTER; ++type) {
        JceTraceEvent event = event_for(type);
        strcpy(event.name, "integer-name/\"slash\\line\n");
        event.id = event.parent_id = event.thread_id = event.value_u64 = value;
        event.timestamp_ns = value;
        event.value_u32[0] = event.value_u32[1] = (uint32_t)value;
        equal_batch(&event, 1, true, 0); equal_batch(&event, 1, false, 0);
    }
}
static void test_integer_width_signed_and_timestamp_precision_boundaries_match_old_bytes(void)
{
    const uint64_t edges[] = {0,1,2,9,10,11,99,100,101,999,1000,1001,
        UINT32_MAX,UINT64_C(4294967296),UINT64_C(9007199254740991),
        UINT64_C(9007199254740992),UINT64_C(9007199254740993),
        INT64_MAX,UINT64_C(9223372036854775808),UINT64_C(9223372036854775809),
        UINT64_MAX-1,UINT64_MAX};
    for (size_t i = 0; i < sizeof(edges)/sizeof(edges[0]); ++i) equal_integer_value(edges[i]);
    for (uint64_t power = 1; ; power *= 10) {
        equal_integer_value(power-1); equal_integer_value(power); equal_integer_value(power+1);
        if (power > UINT64_MAX/10) break;
    }
    // Stable deterministic whole-width vectors exercise mixed fields and names.
    uint64_t state = UINT64_C(0x692f4bc15a379d83);
    for (unsigned i = 0; i < 1024; ++i) {
        JceTraceEvent event = event_for(JCE_TRACE_EVENT_TASK_SUBMIT + i % 7u);
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        event.id = state; event.parent_id = ~state; event.thread_id = state >> 3;
        event.value_u64 = state ^ UINT64_C(0x8000000000000000);
        event.timestamp_ns = state; event.value_u32[0] = (uint32_t)(state >> 11);
        memset(event.name, (i % 2) ? 'x' : 1, JCE_TRACE_NAME_CAP - 1u);
        equal_batch(&event, 1, (i % 2) != 0, 0);
    }
}

static void test_unknown_enum_inputs_keep_original_default_branches(void)
{
    const int values[] = {-1, INT32_MIN, INT32_MAX, 10, 255};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        JceTraceEvent event = event_for((unsigned)values[i]);
        event.category = (JceTraceCategory)values[i];
        strcpy(event.name, "unknown-type-category");
        equal_batch(&event, 1, true, 0);
    }
}
static void test_integer_scratch_limits_and_failed_batch_preserve_owned_output(void)
{
    char bytes[32] = "prefix"; TraceIntegerArgs args = {bytes,6,7};
    TEST_ASSERT_FALSE(trace_integer_value(&args, UINT64_MAX, false));
    TEST_ASSERT_EQUAL_UINT64(6, args.size); TEST_ASSERT_EQUAL_STRING("prefix", bytes);
    TEST_ASSERT_FALSE(trace_integer_value(&args, UINT64_C(9223372036854775808), true));
    TEST_ASSERT_EQUAL_UINT64(6, args.size); TEST_ASSERT_EQUAL_STRING("prefix", bytes);
    TEST_ASSERT_FALSE(trace_integer_literal(&args, "x", SIZE_MAX));
    TEST_ASSERT_EQUAL_UINT64(6, args.size); TEST_ASSERT_EQUAL_STRING("prefix", bytes);
    args.capacity = sizeof(bytes); args.size = 0;
    TEST_ASSERT_TRUE(trace_integer_value(&args, UINT64_MAX, false));
    TEST_ASSERT_EQUAL_STRING("18446744073709551615", bytes);
    args.size = 0; TEST_ASSERT_TRUE(trace_integer_value(&args, UINT64_C(9223372036854775808), true));
    TEST_ASSERT_EQUAL_STRING("-9223372036854775808", bytes);
    TraceIntegerArgs invalid = {&bytes[0],SIZE_MAX,SIZE_MAX};
    TEST_ASSERT_FALSE(trace_integer_value(&invalid, 1, false));
    TEST_ASSERT_FALSE(trace_integer_literal(&invalid, "", 0));
    TraceJsonBuffer json = {0}; bool first = true;
    JceTraceEvent event = event_for(JCE_TRACE_EVENT_COUNTER);
    strcpy(event.name, "retained");
    TEST_ASSERT_TRUE(measured_encode_into(&event,1,&first,&json));
    const size_t previous = json.size; char saved[1024];
    TEST_ASSERT_TRUE(previous + 1u < sizeof(saved)); memcpy(saved,json.data,previous+1u);
    char tiny[2];
    TEST_ASSERT_FALSE(trace_json_append_integer_args(&json,&event,tiny,sizeof(tiny)));
    TEST_ASSERT_EQUAL_UINT64(previous,json.size);
    TEST_ASSERT_EQUAL_MEMORY(saved,json.data,previous+1u);
    JCE_FREE(json.data);
}

typedef bool (*Encoder)(const JceTraceEvent *, uint32_t, bool *, TraceJsonBuffer *);
static int benchmark(void)
{
    enum { BATCH = 512, REPEATS = 64, SEGMENTS = 8 };
    JceTraceEvent events[BATCH];
    for (unsigned i = 0; i < BATCH; ++i) {
        events[i] = event_for(JCE_TRACE_EVENT_TASK_SUBMIT + i % 7u);
        strcpy(events[i].name, (i % 16u) ? "renderer.scene.color-submit" : "quote\"slash\\tab\t");
        events[i].id = 1000 + i; events[i].parent_id = 999;
        events[i].timestamp_ns = UINT64_C(1234567890123456) + i;
    }
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (!frequency) return 1;
    const unsigned order[SEGMENTS] = {0, 1, 1, 0, 0, 1, 1, 0};
    TraceJsonBuffer buffers[2] = {{0}, {0}};
    Encoder encoders[2] = {old_reference_encode_into, current_reference_encode_into};
    for (unsigned mode = 0; mode < 2; ++mode) {
        for (unsigned warm = 0; warm < 8; ++warm) {
            bool first = true; buffers[mode].size = 0;
            if (!encoders[mode](events, BATCH, &first, &buffers[mode])) return 1;
        }
    }
    printf("CODEC_BENCH frequency=%llu events_per_segment=%u order=ABBAABBA scope=codec_only_uninstrumented_no_IO\n",
        (unsigned long long)frequency, BATCH * REPEATS);
    uint64_t checksum = 0;
    for (unsigned segment = 0; segment < SEGMENTS; ++segment) {
        unsigned mode = order[segment]; uint64_t bytes = 0;
        uint64_t begin = SDL_GetPerformanceCounter();
        for (unsigned repeat = 0; repeat < REPEATS; ++repeat) {
            bool first = true; buffers[mode].size = 0;
            if (!encoders[mode](events, BATCH, &first, &buffers[mode])) return 1;
            bytes += buffers[mode].size;
            checksum += (unsigned char)buffers[mode].data[buffers[mode].size - 1u];
        }
        uint64_t end = SDL_GetPerformanceCounter();
        if (end < begin) return 1;
        printf("CODEC_SEGMENT index=%u mode=%s ticks=%llu bytes=%llu capacity=%llu\n", segment,
            mode ? "span" : "old", (unsigned long long)(end - begin),
            (unsigned long long)bytes, (unsigned long long)buffers[mode].capacity);
    }
    printf("CODEC_BENCH checksum=%llu\n", (unsigned long long)checksum);
    JCE_FREE(buffers[0].data); JCE_FREE(buffers[1].data); return 0;
}
int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_nonzero_name_byte_preserves_original_wire_bytes);
    RUN_TEST(test_max_names_empty_and_first_nul_preserve_original_bytes);
    RUN_TEST(test_limits_and_invalid_tail_keep_complete_batch_transaction);
    RUN_TEST(test_reused_buffer_and_invalid_arguments_remain_unchanged);
    RUN_TEST(test_plain_names_use_bounded_span_copy_and_no_literal_formatting);
    RUN_TEST(test_explicit_length_append_refuses_overflow_before_touching_source);
    RUN_TEST(test_integer_width_signed_and_timestamp_precision_boundaries_match_old_bytes);
    RUN_TEST(test_unknown_enum_inputs_keep_original_default_branches);
    RUN_TEST(test_integer_scratch_limits_and_failed_batch_preserve_owned_output);
    int result = UNITY_END();
    if (!result && argc == 2 && strcmp(argv[1], "--benchmark") == 0) result = benchmark();
    return result;
}
