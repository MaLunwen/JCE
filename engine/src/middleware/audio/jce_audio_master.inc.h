/* Included after JceAudio: the sole device, graph registration and tap owner. */
#include "jce_audio_master_gate.h"
#include "jce_audio_master_lifecycle_internal.h"
#define JCE_AUDIO_MAX_ENGINES 8
#define JCE_AUDIO_MASTER_RATE 48000u
#define JCE_AUDIO_MASTER_CH 2u

typedef struct MasterSlot {
    SDL_AtomicInt live;
    JceAudio *ptr;
} MasterSlot;

static struct {
    bool device_inited;
    bool device_failed;
    bool session_closed;
    bool retiring;
    ma_device device;
    MasterSlot slots[JCE_AUDIO_MAX_ENGINES];
    JceAudioMasterGate gate;
    SDL_AtomicInt tap_live;
    JceAudioMasterTapFn tap_fn;
    void *tap_ud;
} s_master;

static bool master_control_allowed(void)
{
    /* Existing public controls remain caller-serialized, not globally bound
     * to Engine main. Refuse same-thread Web callbacks before target mutation. */
    return !jce_audio_master_gate_self(&s_master.gate);
}

static bool master_wait_quiescent(void)
{
    /* Legacy release is synchronous: an arbitrary timeout cannot free a target
     * still borrowed by user PCM/tap code. Engine's private preflight is bounded. */
    return jce_audio_master_gate_wait(&s_master.gate, UINT32_MAX);
}

static void master_device_cb(ma_device *dev, void *out, const void *in,
                             ma_uint32 frames)
{
    (void)dev;
    (void)in;
    float *dst = (float *)out;
    memset(dst, 0, (size_t)frames * JCE_AUDIO_MASTER_CH * sizeof(float));
    if (!jce_audio_master_gate_enter(&s_master.gate)) return;
    enum { CHUNK = 1024 };
    static float tmp[CHUNK * JCE_AUDIO_MASTER_CH];
    ma_uint32 done = 0;
    while (done < frames) {
        ma_uint32 n = frames - done;
        if (n > CHUNK) n = CHUNK;
        float *acc = dst + (size_t)done * JCE_AUDIO_MASTER_CH;
        for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
            if (!SDL_GetAtomicInt(&s_master.slots[i].live)) continue;
            JceAudio *a = s_master.slots[i].ptr;
            if (!a) continue;
            ma_uint64 read = 0;
            ma_engine_read_pcm_frames(&a->engine, tmp, n, &read);
            const ma_uint32 cnt = (ma_uint32)read * JCE_AUDIO_MASTER_CH;
            for (ma_uint32 s = 0; s < cnt; ++s) acc[s] += tmp[s];
        }
        done += n;
    }
    if (SDL_GetAtomicInt(&s_master.tap_live) && s_master.tap_fn)
        s_master.tap_fn(s_master.tap_ud, dst, frames,
                        JCE_AUDIO_MASTER_RATE, JCE_AUDIO_MASTER_CH);
    jce_audio_master_gate_leave(&s_master.gate);
}

static bool master_device_ensure(void)
{
    if (!master_control_allowed() || s_master.session_closed || s_master.retiring)
        return false;
    if (s_master.device_inited) return true;
    if (s_master.device_failed) return false;
    if (!jce_audio_master_gate_create(&s_master.gate)) {
        s_master.device_failed = true;
        LOG_ERROR("jce_audio", "master callback admission allocation failed; engines remain silent until the next session");
        return false;
    }
    ma_device_config dc = ma_device_config_init(ma_device_type_playback);
    dc.playback.format = ma_format_f32;
    dc.playback.channels = JCE_AUDIO_MASTER_CH;
    dc.sampleRate = JCE_AUDIO_MASTER_RATE;
    dc.dataCallback = master_device_cb;
    if (ma_device_init(NULL, &dc, &s_master.device) != MA_SUCCESS) {
        s_master.device_failed = true;
        LOG_WARN("jce_audio", "master mix device init failed; engines remain silent");
        return false;
    }
    if (ma_device_start(&s_master.device) != MA_SUCCESS) {
        ma_device_uninit(&s_master.device);
        s_master.device_failed = true;
        LOG_WARN("jce_audio", "master mix device start failed");
        return false;
    }
    s_master.device_inited = true;
    LOG_SUCCESS("jce_audio", "master mix device started (48kHz/2ch)");
    return true;
}

static bool master_register(JceAudio *audio)
{
    if (!master_control_allowed() || s_master.session_closed || s_master.retiring)
        return false;
    for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
        if (SDL_GetAtomicInt(&s_master.slots[i].live) || s_master.slots[i].ptr)
            continue;
        s_master.slots[i].ptr = audio;
        SDL_SetAtomicInt(&s_master.slots[i].live, 1);
        return true;
    }
    return false;
}

static bool master_unregister(JceAudio *audio)
{
    if (!master_control_allowed()) return false;
    for (int i = 0; i < JCE_AUDIO_MAX_ENGINES; ++i) {
        if (s_master.slots[i].ptr != audio) continue;
        SDL_SetAtomicInt(&s_master.slots[i].live, 0);
        if (!master_wait_quiescent()) return false;
        s_master.slots[i].ptr = NULL;
        return true;
    }
    return true;
}

bool jce_audio_master_tap_set(JceAudioMasterTapFn fn, void *ud)
{
    if (!master_control_allowed()) return false;
    if (fn && (s_master.session_closed || s_master.retiring)) return false;
    if (fn && !master_device_ensure()) return false;
    /* Replacement retires the previous pair before either field is changed. */
    SDL_SetAtomicInt(&s_master.tap_live, 0);
    if (!master_wait_quiescent()) return false;
    s_master.tap_fn = fn;
    s_master.tap_ud = fn ? ud : NULL;
    if (fn) SDL_SetAtomicInt(&s_master.tap_live, 1);
    return true;
}

#include "jce_audio_master_lifecycle.inc.h"
