/* A decoded batch from the previous seek generation must never be published. */
#include "unity.h"
#include "jce_audio_stream.h"
#include <jce/api_core.h>

static JceAudioStream *stream;
static JceSemaphore *decode_started, *decode_release, *seek_started, *seek_release;
static bool first;
static int16_t value;

void setUp(void)
{
    first=true;
    value=100;
    decode_started=jce_semaphore_create(0u);
    decode_release=jce_semaphore_create(0u);
    seek_started=jce_semaphore_create(0u);
    seek_release=jce_semaphore_create(0u);
}

void tearDown(void)
{
    jce_semaphore_signal(decode_release);
    jce_semaphore_signal(seek_release);
    jce_audio_stream_destroy(stream);
    stream=NULL;
    jce_semaphore_destroy(decode_started);
    jce_semaphore_destroy(decode_release);
    jce_semaphore_destroy(seek_started);
    jce_semaphore_destroy(seek_release);
}

static uint32_t decode(void *user,int16_t *out,uint32_t capacity)
{
    uint32_t frames=capacity<256u?capacity:256u;
    int16_t batch=value;
    (void)user;
    if (first) {
        first=false;
        jce_semaphore_signal(decode_started);
        (void)jce_semaphore_wait_timeout(decode_release,5000u);
    }
    for (uint32_t i=0u;i<frames;++i) out[i]=batch;
    return frames;
}

static void seek(void *user,double seconds)
{
    (void)user;
    (void)seconds;
    value=200;
    jce_semaphore_signal(seek_started);
    (void)jce_semaphore_wait_timeout(seek_release,5000u);
}

static void test_inflight_batch_cannot_cross_seek(void)
{
    JceAudioStreamDesc desc={0};
    int16_t out[256];
    uint32_t got=0u;
    uint64_t start;
    desc.decode_next=decode;
    desc.seek=seek;
    desc.channels=1u;
    desc.samplerate=8000u;
    desc.duration_sec=10.0;
    stream=jce_audio_stream_create(&desc);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(decode_started,5000u));
    TEST_ASSERT_FALSE(jce_audio_stream_ready(stream));
    jce_audio_stream_seek(stream,1.0);
    jce_semaphore_signal(decode_release);
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(seek_started,5000u));
    /* The seek callback is blocked: the old code has published its old batch
     * and has not yet cleared it. No scheduler timing assumption is needed. */
    TEST_ASSERT_FALSE(jce_audio_stream_ready(stream));
    TEST_ASSERT_EQUAL_UINT(0u,jce_audio_stream_pull(stream,out,256u));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001,1.0,jce_audio_stream_get_time(stream));
    jce_semaphore_signal(seek_release);
    start=jce_time_ticks_ms();
    while (!jce_audio_stream_ready(stream) && jce_time_ticks_ms()-start<5000u)
        jce_thread_sleep_ms(1u);
    TEST_ASSERT_TRUE(jce_audio_stream_ready(stream));
    got=jce_audio_stream_pull(stream,out,256u);
    TEST_ASSERT_GREATER_THAN_UINT(0u,got);
    TEST_ASSERT_EQUAL_INT16(200,out[0]);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001,1.0+(double)got/8000.0,
                             jce_audio_stream_get_time(stream));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_inflight_batch_cannot_cross_seek);
    return UNITY_END();
}
