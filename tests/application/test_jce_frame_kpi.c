/* Complete raw export is one write; long phase names never overread a row. */
#include "application/jce_frame_kpi.h"
#include <jce/os/core/jce_perf_phase.h>
#include "unity.h"
#include <string.h>

typedef struct {
    char bytes[32768];
    size_t size;
    unsigned calls;
    bool fail;
} Sink;

static char names[48][257];

static size_t SDLCALL sink_write(void *user, const void *bytes, size_t size,
                                 SDL_IOStatus *status)
{
    Sink *sink = user;
    ++sink->calls;
    if (sink->fail || size >= sizeof(sink->bytes) - sink->size) {
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    memcpy(sink->bytes + sink->size, bytes, size);
    sink->size += size;
    sink->bytes[sink->size] = 0;
    return size;
}

static SDL_IOStream *stream_for(Sink *sink)
{
    SDL_IOStreamInterface api;
    SDL_INIT_INTERFACE(&api);
    api.write = sink_write;
    return SDL_OpenIO(&api, sink);
}

void setUp(void) { jce_perf_phase_set_enabled(1); }
void tearDown(void) {}

static void test_growth_complete_rows_and_reuse(void)
{
    for (unsigned index = 0; index < 48; ++index) {
        memset(names[index], 'x', 256);
        names[index][0] = 'A' + (char)(index % 26);
        names[index][1] = 'A' + (char)(index / 26);
        names[index][256] = 0;
        jce_perf_phase_add(names[index], index + 0.25);
    }
    jce_perf_phase_frame_tick();
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    JceGpuStats stats = {0};
    stats.valid = true;
    stats.cpu_frame_ms = 2.5;
    stats.gpu_ms = 0.125;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 42, 3.75, &stats));
    TEST_ASSERT_EQUAL_UINT(1, sink.calls);
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_wall,3.7500\n"));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_cpu,2.5000\n"));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_gpu,0.1250\n"));
    for (unsigned index = 0; index < 48; ++index)
        TEST_ASSERT_NOT_NULL(strstr(sink.bytes, names[index]));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(16384, buffer.capacity);
    char *bytes = buffer.bytes;
    size_t capacity = buffer.capacity;
    sink.size = sink.calls = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 43, 4, NULL));
    TEST_ASSERT_EQUAL_UINT(1, sink.calls);
    TEST_ASSERT_EQUAL_PTR(bytes, buffer.bytes);
    TEST_ASSERT_EQUAL_UINT(capacity, buffer.capacity);
    TEST_ASSERT_NULL(strstr(sink.bytes, "frame_cpu"));
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_NULL(buffer.bytes);
    TEST_ASSERT_EQUAL_UINT(0, buffer.capacity);
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_failed_stream_and_invalid_clock_fail_closed(void)
{
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    sink.fail = true;
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 44, 1, NULL));
    TEST_ASSERT_EQUAL_UINT(0, sink.size);
    sink.fail = false;
    sink.calls = 0;
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 45, -1, NULL));
    TEST_ASSERT_EQUAL_UINT(0, sink.calls);
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_csv_delimiters_cannot_create_forged_rows(void)
{
    jce_perf_phase_add("bad\n0,frame_wall,0", 1);
    jce_perf_phase_frame_tick();
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 46, 1, NULL));
    TEST_ASSERT_EQUAL_UINT(0, sink.calls);
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_growth_complete_rows_and_reuse);
    RUN_TEST(test_failed_stream_and_invalid_clock_fail_closed);
    RUN_TEST(test_csv_delimiters_cannot_create_forged_rows);
    return UNITY_END();
}
