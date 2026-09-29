#include "unity.h"
#include <jce/api_audio.h>
#include <jce/api_core.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *path="_ut_audio_file.wav";
static JceAudioFile *active_file;
void setUp(void) {}
void tearDown(void) { jce_audio_file_close(active_file); active_file=NULL; jce_fs_host_remove_file(path); }

static void put16(uint8_t *p,uint16_t value)
{ p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8); }
static void put32(uint8_t *p,uint32_t value)
{ put16(p,(uint16_t)value); put16(p+2u,(uint16_t)(value>>16)); }

static void write_fixture(void)
{
    uint8_t *wav=jce_malloc(16044u);
    unsigned i;
    TEST_ASSERT_NOT_NULL(wav);
    memset(wav,0,16044u);
    memcpy(wav,"RIFF",4u); put32(wav+4u,16036u); memcpy(wav+8u,"WAVEfmt ",8u);
    put32(wav+16u,16u); put16(wav+20u,1u); put16(wav+22u,1u);
    put32(wav+24u,8000u); put32(wav+28u,16000u); put16(wav+32u,2u); put16(wav+34u,16u);
    memcpy(wav+36u,"data",4u); put32(wav+40u,16000u);
    for (i=0u;i<8000u;++i) put16(wav+44u+i*2u,(uint16_t)((int)(i%100u)*256-12800));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path,wav,16044u));
    jce_free(wav);
}

static void test_stream_seek_eof_and_background_waveform(void)
{
    JceAudioFile *file;
    uint32_t channels=0u,rate=0u,total=0u;
    double duration=0.0;
    int16_t pcm[256];
    uint8_t peaks[JCE_AUDIO_FILE_WAVEFORM_BINS];
    float progress=0.0f;
    uint64_t start;
    write_fixture();
    file=active_file=jce_audio_file_open(path);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(jce_audio_file_format(file,&channels,&rate,&duration));
    TEST_ASSERT_EQUAL_UINT(1u,channels); TEST_ASSERT_EQUAL_UINT(8000u,rate);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001,1.0,duration);
    start=jce_time_ticks_ms();
    while (!jce_audio_file_eof(file) && jce_time_ticks_ms()-start<5000u) {
        uint32_t got=jce_audio_file_pull(file,pcm,256u);
        if (got && total==0u) TEST_ASSERT_EQUAL_INT16(-12800,pcm[0]);
        total+=got;
        if (!got) jce_thread_sleep_ms(1u);
    }
    TEST_ASSERT_EQUAL_UINT(8000u,total);
    TEST_ASSERT_TRUE(jce_audio_file_eof(file));
    TEST_ASSERT_DOUBLE_WITHIN(0.0001,1.0,jce_audio_file_time(file));
    start=jce_time_ticks_ms();
    do {
        jce_audio_file_waveform(file,peaks,sizeof(peaks),&progress);
        if (progress<1.0f) jce_thread_sleep_ms(1u);
    } while (progress<1.0f && jce_time_ticks_ms()-start<5000u);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f,1.0f,progress);
    TEST_ASSERT_GREATER_THAN_UINT8(90u,peaks[0]);
    jce_audio_file_seek(file,0.25);
    start=jce_time_ticks_ms();
    while (!jce_audio_file_pull(file,pcm,256u) && jce_time_ticks_ms()-start<5000u)
        jce_thread_sleep_ms(1u);
    TEST_ASSERT_EQUAL_INT16(-12800,pcm[0]);
    TEST_ASSERT_DOUBLE_WITHIN(0.0001,0.282,jce_audio_file_time(file));
    jce_audio_file_close(file); active_file=NULL;
    jce_async_default_pump(NULL);
}

static void test_close_cancels_pending_analysis(void)
{
    unsigned i;
    write_fixture();
    for (i=0u;i<4u;++i) {
        JceAudioFile *file=jce_audio_file_open(path);
        TEST_ASSERT_NOT_NULL(file);
        jce_audio_file_close(file);
        jce_async_default_pump(NULL);
    }
    TEST_ASSERT_NULL(jce_audio_file_open("_ut_missing_audio_file.wav"));
}

static void probe_real_file(void)
{
    const char *input=getenv("JCE_TEST_AUDIO_FILE");
    if (!input || !input[0]) return;
    JceAudioFile *file=active_file=jce_audio_file_open(input);
    uint32_t channels,rate;
    double duration;
    int16_t pcm[2048u*8u];
    uint8_t peaks[JCE_AUDIO_FILE_WAVEFORM_BINS];
    float progress=0.0f;
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_TRUE(jce_audio_file_format(file,&channels,&rate,&duration));
    printf("audio file format %uch %uHz duration=%.6f\n",channels,rate,duration);
    for (unsigned i=0u;i<3u;++i) {
        const double target=duration*(i==0u?0.75:i==1u?0.10:0.90);
        const uint64_t start=jce_time_ticks_ms();
        uint32_t got=0u;
        jce_audio_file_seek(file,target);
        while (!(got=jce_audio_file_pull(file,pcm,2048u)) && jce_time_ticks_ms()-start<10000u)
            jce_thread_sleep_ms(1u);
        TEST_ASSERT_GREATER_THAN_UINT(0u,got);
        TEST_ASSERT_DOUBLE_WITHIN(0.0001,target+(double)got/rate,jce_audio_file_time(file));
        printf("audio file seek %.3f ready=%llu ms got=%u time=%.6f\n",target,
               (unsigned long long)(jce_time_ticks_ms()-start),got,jce_audio_file_time(file));
    }
    const uint64_t start=jce_time_ticks_ms();
    do {
        jce_audio_file_waveform(file,peaks,sizeof(peaks),&progress);
        if (progress<0.05f) jce_thread_sleep_ms(5u);
    } while (progress<0.05f && jce_time_ticks_ms()-start<10000u);
    unsigned nonzero=0u;
    for (unsigned i=0u;i<JCE_AUDIO_FILE_WAVEFORM_BINS;++i) if (peaks[i]) ++nonzero;
    TEST_ASSERT_GREATER_THAN_UINT(0u,nonzero);
    printf("audio waveform progress=%.3f nonzero=%u\n",progress,nonzero);
    jce_audio_file_close(file); active_file=NULL;
}

int main(void)
{
    int result;
    UNITY_BEGIN();
    RUN_TEST(test_stream_seek_eof_and_background_waveform);
    RUN_TEST(test_close_cancels_pending_analysis);
    RUN_TEST(probe_real_file);
    result=UNITY_END();
    jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_DRAIN,5000u);
    return result;
}
