/*
 * jce_audio_loopback.c  WASAPI loopback capture of the system output.
 */

#include <jce/middleware/audio/jce_audio_loopback.h>
#include <jce/os/core/jce_log.h>

#define LOG_TAG "jce_loopback"

#ifndef JCE_NO_AUDIO

#include <miniaudio.h>
#include <string.h>

static struct {
    bool                active;
    ma_device           device;
    JceAudioLoopbackFn  cb;
    void               *ud;
    uint32_t            sample_rate;
    uint32_t            channels;
} g;

/* Loopback is a capture device: pInput holds the captured system output. */
static void loopback_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    (void)dev; (void)out;
    if (g.cb && in && frames)
        g.cb(g.ud, (const float *)in, frames, g.sample_rate, g.channels);
}

bool jce_audio_loopback_start(JceAudioLoopbackFn cb, void *ud)
{
    if (g.active) return false;

#if defined(_WIN32)
    memset(&g, 0, sizeof(g));
    g.cb = cb; g.ud = ud;
    g.sample_rate = 48000;   /* Opus operates at 48 kHz */
    g.channels    = 2;

    ma_device_config dc = ma_device_config_init(ma_device_type_loopback);
    dc.capture.format   = ma_format_f32;
    dc.capture.channels = 2;
    dc.sampleRate       = 48000;
    dc.dataCallback     = loopback_cb;
    dc.pUserData        = NULL;

    if (ma_device_init(NULL, &dc, &g.device) != MA_SUCCESS) {
        LOG_ERROR(LOG_TAG, "loopback device init failed (no WASAPI output?)");
        return false;
    }
    /* Reflect the device's negotiated format (miniaudio converts to ours). */
    g.sample_rate = 48000;
    g.channels    = 2;
    if (ma_device_start(&g.device) != MA_SUCCESS) {
        ma_device_uninit(&g.device);
        LOG_ERROR(LOG_TAG, "loopback device start failed");
        return false;
    }
    g.active = true;
    LOG_SUCCESS(LOG_TAG, "system loopback capture started (48kHz/2ch)");
    return true;
#else
    (void)cb; (void)ud;
    LOG_WARN(LOG_TAG, "loopback capture is Windows-only (WASAPI)");
    return false;
#endif
}

void jce_audio_loopback_stop(void)
{
    if (!g.active) return;
    ma_device_uninit(&g.device);
    g.active = false;
    g.cb = NULL; g.ud = NULL;
    LOG_SUCCESS(LOG_TAG, "loopback capture stopped");
}

bool jce_audio_loopback_is_active(void) { return g.active; }

#else /* JCE_NO_AUDIO */

bool jce_audio_loopback_start(JceAudioLoopbackFn cb, void *ud) { (void)cb; (void)ud; return false; }
void jce_audio_loopback_stop(void) {}
bool jce_audio_loopback_is_active(void) { return false; }

#endif
