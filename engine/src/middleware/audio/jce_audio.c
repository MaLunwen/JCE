/*
 * jce_audio.c  miniaudio audio implementation.
 *
 * Loads WAV and OGG from PAK into PCM buffers, plays them via the
 * miniaudio engine.  Manages a pool of sounds and voices for
 * concurrent playback.
 */

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_m4a_decode.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/resource/jce_pak_loader.h>
#include <jce/resource/jce_asset_format.h>

#include "jce_miniaudio_opus_backend.h"

#ifndef JCE_NO_AUDIO

#include "os/core/jce_memory.h"
#include "resource/jce_asset_reader.h"

#include <miniaudio.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JCE_MAX_SOUNDS  64
#define JCE_MAX_VOICES  64
#define JCE_MAX_BUSES   16
#define JCE_BUS_NAME_MAX 32

/* ── Freeverb (public-domain Schroeder reverb) ─────────────────────────
 *
 * Jezar's Freeverb topology: 8 parallel low-pass comb filters summed into
 * 4 series all-pass filters per channel.  We vendor a compact, dependency-
 * free implementation and wrap it as a custom miniaudio node so authored
 * reverb zones drive a real DSP tail.  Tuning constants are the classic
 * Freeverb values (scaled per sample rate).  Public domain — no license. */

#define JCE_FV_NUM_COMBS    8
#define JCE_FV_NUM_ALLPASS  4
#define JCE_FV_MAX_CH       2

/* Comb/all-pass delay lengths (in frames @ 44100 Hz), Freeverb defaults.
 * The second channel adds a small stereo spread offset. */
static const int g_fv_comb_len[JCE_FV_NUM_COMBS] =
    { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int g_fv_allpass_len[JCE_FV_NUM_ALLPASS] =
    { 556, 441, 341, 225 };
#define JCE_FV_STEREO_SPREAD 23

typedef struct {
    float *buf;
    int    size;
    int    pos;
    float  feedback;
    float  filterstore; /* one-pole LP state for damping */
    float  damp1, damp2;
} FvComb;

typedef struct {
    float *buf;
    int    size;
    int    pos;
    float  feedback;
} FvAllpass;

typedef struct {
    FvComb    comb[JCE_FV_MAX_CH][JCE_FV_NUM_COMBS];
    FvAllpass allpass[JCE_FV_MAX_CH][JCE_FV_NUM_ALLPASS];
    int       channels;
    int       sample_rate;
    float     wet;       /* wet output gain */
    float     dry;       /* dry passthrough gain */
    float     roomsize;  /* comb feedback (0..~0.98) */
    float     damp;      /* comb LP damping (0..1) */
    bool      allocated;
} Freeverb;

/* Custom miniaudio node embedding a Freeverb. */
typedef struct {
    ma_node_base base;
    Freeverb     fv;
} ReverbNode;

/* Each sound owns a block of decoded PCM (s16) data. */
typedef struct {
    void      *pcm_data;      /* JCE_MALLOC'd, s16 PCM */
    ma_uint64  frame_count;
    ma_uint32  channels;
    ma_uint32  sample_rate;
} SoundSlot;

/* A named output bus backed by a ma_sound_group node. */
typedef struct {
    bool           used;
    char           name[JCE_BUS_NAME_MAX];
    ma_sound_group group;     /* node: voices attach here, group -> reverb/endpoint */
} BusSlot;

/* Each voice is an independent playback instance. */
typedef struct {
    ma_audio_buffer buffer;   /* owns a read cursor over the SoundSlot PCM */
    ma_sound        sound;    /* attached to the engine */
    bool            inited;
    int             sound_slot;
    uint32_t        generation; /* bumped on teardown; packed into JceVoice to reject stale handles */
    uint64_t        play_seq;   /* allocation order, for oldest-voice stealing when the pool is full */
    ma_lpf_node     lpf;        /* occlusion muffle filter, inserted sound→lpf→endpoint */
    bool            lpf_ok;
    float           lpf_cutoff; /* current cutoff Hz; avoids reinit churn when unchanged */
    /* Streaming voices: custom data source instead of ma_audio_buffer. */
    bool                  is_stream;
    ma_data_source_base   stream_ds;
    JceAudioStreamPullFn  stream_on_read;
    void                 *stream_ud;
    ma_uint32             stream_channels;
    ma_uint32             stream_samplerate;
    ma_uint64             stream_cursor; /* frames pulled so far */
    int                   bus;        /* index into JceAudio.buses, or -1 (direct) */
} VoiceSlot;

struct JceAudio {
    ma_engine   engine;
    bool        engine_inited;

    SoundSlot   sounds[JCE_MAX_SOUNDS];
    bool        sound_used[JCE_MAX_SOUNDS];

    VoiceSlot   voices[JCE_MAX_VOICES];
    uint64_t    play_counter;   /* monotonic; stamped into voice.play_seq on alloc */

    /* Named mixer buses (flat under Master).  Group nodes route into the
     * reverb node when present, otherwise to the engine endpoint. */
    BusSlot     buses[JCE_MAX_BUSES];

    /* Global reverb DSP (Freeverb).  Created lazily on first jce_audio_set_reverb
     * with a positive wet mix.  When live, all bus groups attach to it and it
     * feeds the endpoint; otherwise groups attach straight to the endpoint. */
    ReverbNode  reverb;
    bool        reverb_inited;
    float       reverb_wet;      /* cached send level for dry/wet ramp */
};

/* ── Freeverb DSP ──────────────────────────────────────────────────── */

static void fv_comb_set(FvComb *c, float feedback, float damp)
{
    c->feedback = feedback;
    c->damp1    = damp;
    c->damp2    = 1.0f - damp;
}

static inline float fv_comb_process(FvComb *c, float in)
{
    float out = c->buf[c->pos];
    c->filterstore = out * c->damp2 + c->filterstore * c->damp1;
    c->buf[c->pos] = in + c->filterstore * c->feedback;
    if (++c->pos >= c->size) c->pos = 0;
    return out;
}

static inline float fv_allpass_process(FvAllpass *a, float in)
{
    float bufout = a->buf[a->pos];
    float out    = -in + bufout;
    a->buf[a->pos] = in + bufout * a->feedback;
    if (++a->pos >= a->size) a->pos = 0;
    return out;
}

/* Allocate the comb/allpass delay lines for `channels` at `sample_rate`. */
static bool fv_alloc(Freeverb *fv, int channels, int sample_rate)
{
    if (channels < 1) channels = 1;
    if (channels > JCE_FV_MAX_CH) channels = JCE_FV_MAX_CH;
    fv->channels    = channels;
    fv->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    float sr_scale  = (float)fv->sample_rate / 44100.0f;

    for (int ch = 0; ch < channels; ++ch) {
        int spread = ch * JCE_FV_STEREO_SPREAD;
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i) {
            int len = (int)((float)(g_fv_comb_len[i] + spread) * sr_scale);
            if (len < 1) len = 1;
            FvComb *c = &fv->comb[ch][i];
            c->buf = (float *)JCE_CALLOC((size_t)len, sizeof(float));
            if (!c->buf) return false;
            c->size = len; c->pos = 0; c->filterstore = 0.0f;
        }
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i) {
            int len = (int)((float)(g_fv_allpass_len[i] + spread) * sr_scale);
            if (len < 1) len = 1;
            FvAllpass *a = &fv->allpass[ch][i];
            a->buf = (float *)JCE_CALLOC((size_t)len, sizeof(float));
            if (!a->buf) return false;
            a->size = len; a->pos = 0; a->feedback = 0.5f;
        }
    }
    fv->allocated = true;
    return true;
}

static void fv_free(Freeverb *fv)
{
    for (int ch = 0; ch < JCE_FV_MAX_CH; ++ch) {
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i) {
            JCE_FREE(fv->comb[ch][i].buf);
            fv->comb[ch][i].buf = NULL;
        }
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i) {
            JCE_FREE(fv->allpass[ch][i].buf);
            fv->allpass[ch][i].buf = NULL;
        }
    }
    fv->allocated = false;
}

/* Push tuning (wet/dry/roomsize/damp) into the comb feedback coefficients. */
static void fv_set_params(Freeverb *fv, float wet, float dry,
                          float roomsize, float damp)
{
    if (wet < 0.0f) wet = 0.0f;
    if (dry < 0.0f) dry = 0.0f;
    if (roomsize < 0.0f) roomsize = 0.0f;
    if (roomsize > 0.98f) roomsize = 0.98f;
    if (damp < 0.0f) damp = 0.0f;
    if (damp > 1.0f) damp = 1.0f;
    fv->wet = wet; fv->dry = dry; fv->roomsize = roomsize; fv->damp = damp;
    for (int ch = 0; ch < fv->channels; ++ch)
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i)
            fv_comb_set(&fv->comb[ch][i], roomsize, damp);
}

/* Process one channel's block in place: out = dry*in + wet*reverb(in). */
static void fv_process_channel(Freeverb *fv, int ch,
                               const float *in, float *out, ma_uint32 n)
{
    const float gain = 0.015f; /* Freeverb fixed input gain */
    for (ma_uint32 s = 0; s < n; ++s) {
        float x = in[s] * gain;
        float acc = 0.0f;
        for (int i = 0; i < JCE_FV_NUM_COMBS; ++i)
            acc += fv_comb_process(&fv->comb[ch][i], x);
        for (int i = 0; i < JCE_FV_NUM_ALLPASS; ++i)
            acc = fv_allpass_process(&fv->allpass[ch][i], acc);
        out[s] = in[s] * fv->dry + acc * fv->wet;
    }
}

/* ── Custom ma_node wrapping Freeverb ──────────────────────────────── */

static void reverb_node_process(ma_node *node,
                                const float **frames_in, ma_uint32 *frame_count_in,
                                float **frames_out, ma_uint32 *frame_count_out)
{
    ReverbNode *rn = (ReverbNode *)node;
    Freeverb   *fv = &rn->fv;
    ma_uint32   n  = *frame_count_out;
    int         ch = fv->allocated ? fv->channels
                   : (int)ma_node_get_output_channels(node, 0);

    if (frame_count_in) {
        ma_uint32 in_n = frame_count_in[0];
        if (in_n < n) n = in_n;
    }
    if (!fv->allocated || ch < 1) {
        /* Passthrough if not ready. */
        if (n > 0 && ch >= 1)
            memcpy(frames_out[0], frames_in[0],
                   (size_t)n * (size_t)ch * sizeof(float));
        *frame_count_out = n;
        return;
    }

    /* Interleaved stereo (engine runs f32 interleaved per bus). De-interleave,
     * run per channel, re-interleave. */
    const float *in  = frames_in[0];
    float       *out = frames_out[0];
    static float scratch_in[JCE_FV_MAX_CH][4096];
    static float scratch_out[JCE_FV_MAX_CH][4096];
    ma_uint32 done = 0;
    while (done < n) {
        ma_uint32 blk = n - done;
        if (blk > 4096) blk = 4096;
        for (ma_uint32 s = 0; s < blk; ++s)
            for (int c = 0; c < ch; ++c)
                scratch_in[c][s] = in[(done + s) * ch + c];
        for (int c = 0; c < ch; ++c)
            fv_process_channel(fv, c, scratch_in[c], scratch_out[c], blk);
        for (ma_uint32 s = 0; s < blk; ++s)
            for (int c = 0; c < ch; ++c)
                out[(done + s) * ch + c] = scratch_out[c][s];
        done += blk;
    }
    *frame_count_out = n;
}

static ma_node_vtable g_reverb_vtable = {
    reverb_node_process,
    NULL,   /* onGetRequiredInputFrameCount */
    1,      /* input bus count  */
    1,      /* output bus count */
    0       /* flags */
};

/* Create the global reverb node and re-route every live bus group through it.
 * On any failure the groups keep their endpoint attachment (dry-only). */
static bool audio_init_reverb(JceAudio *audio)
{
    if (audio->reverb_inited) return true;
    ma_engine *e   = &audio->engine;
    ma_uint32 chan = ma_engine_get_channels(e);
    ma_uint32 sr   = ma_engine_get_sample_rate(e);

    /* Freeverb handles mono/stereo only.  Surround endpoints stay dry. */
    if (chan < 1 || chan > JCE_FV_MAX_CH) {
        LOG_WARN("jce_audio", "reverb: %u-channel endpoint unsupported, staying dry", chan);
        return false;
    }

    if (!fv_alloc(&audio->reverb.fv, (int)chan, (int)sr)) {
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: freeverb alloc failed");
        return false;
    }
    fv_set_params(&audio->reverb.fv, 0.0f, 1.0f, 0.5f, 0.5f);

    ma_node_config cfg = ma_node_config_init();
    cfg.vtable          = &g_reverb_vtable;
    cfg.pInputChannels  = &chan;
    cfg.pOutputChannels = &chan;
    if (ma_node_init(ma_engine_get_node_graph(e), &cfg, NULL,
                     &audio->reverb.base) != MA_SUCCESS) {
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: ma_node_init failed");
        return false;
    }
    if (ma_node_attach_output_bus(&audio->reverb.base, 0,
                                  ma_engine_get_endpoint(e), 0) != MA_SUCCESS) {
        ma_node_uninit(&audio->reverb.base, NULL);
        fv_free(&audio->reverb.fv);
        LOG_WARN("jce_audio", "reverb: attach to endpoint failed");
        return false;
    }
    audio->reverb_inited = true;
    audio->reverb_wet    = 0.0f;

    /* Route any already-created bus groups through the reverb node. */
    for (int i = 0; i < JCE_MAX_BUSES; ++i) {
        if (audio->buses[i].used)
            ma_node_attach_output_bus(&audio->buses[i].group, 0,
                                      &audio->reverb.base, 0);
    }
    /* Re-route direct voices (no bus) that were attached to the endpoint
     * before the reverb node existed, so the global tail covers them too. */
    for (int i = 0; i < JCE_MAX_VOICES; ++i) {
        VoiceSlot *v = &audio->voices[i];
        if (!v->inited || v->bus >= 0) continue;
        ma_node *src = v->lpf_ok ? (ma_node *)&v->lpf : (ma_node *)&v->sound;
        ma_node_attach_output_bus(src, 0, &audio->reverb.base, 0);
    }
    LOG_SUCCESS("jce_audio", "reverb node (freeverb) initialized");
    return true;
}

/* The node a bus group / direct sound should feed: the reverb node if live,
 * otherwise the engine endpoint. */
static ma_node *audio_output_node(JceAudio *audio)
{
    if (audio->reverb_inited) return (ma_node *)&audio->reverb.base;
    return ma_engine_get_endpoint(&audio->engine);
}

/* Find a bus index by name (case-insensitive); -1 if none. */
static int audio_find_bus(const JceAudio *audio, const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < JCE_MAX_BUSES; ++i)
        if (audio->buses[i].used &&
            SDL_strcasecmp(audio->buses[i].name, name) == 0)
            return i;
    return -1;
}

/* -- Lifecycle ------------------------------------------------------ */

JceAudio *jce_audio_create(void)
{
    JceAudio *audio = (JceAudio *)JCE_CALLOC(1, sizeof(*audio));
    if (!audio) return NULL;

    ma_engine_config cfg = ma_engine_config_init();
    if (ma_engine_init(&cfg, &audio->engine) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_engine_init failed");
        JCE_FREE(audio);
        return NULL;
    }

    audio->engine_inited = true;

    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        audio->voices[i].sound_slot = -1;
        audio->voices[i].bus        = -1;
    }

    LOG_SUCCESS("jce_audio", "miniaudio engine initialized");
    return audio;
}

static void uninit_voice(VoiceSlot *v)
{
    if (!v->inited) return;
    ma_sound_uninit(&v->sound);
    if (v->lpf_ok) {
        ma_lpf_node_uninit(&v->lpf, NULL);
        v->lpf_ok = false;
    }
    if (v->is_stream) {
        ma_data_source_uninit(&v->stream_ds);
        v->is_stream = false;
        v->stream_on_read = NULL;
        v->stream_ud = NULL;
    } else {
        ma_audio_buffer_uninit(&v->buffer);
    }
    v->inited = false;
    v->sound_slot = -1;
    v->bus = -1;
    v->generation++;   /* invalidate any outstanding JceVoice handle to this slot */
}

void jce_audio_destroy(JceAudio *audio)
{
    if (!audio) return;

    /* Uninit all voices first (they reference engine). */
    for (int i = 0; i < JCE_MAX_VOICES; i++)
        uninit_voice(&audio->voices[i]);

    /* Bus groups (nodes) — uninit before the reverb node/engine they feed. */
    for (int i = 0; i < JCE_MAX_BUSES; i++) {
        if (audio->buses[i].used) {
            ma_sound_group_uninit(&audio->buses[i].group);
            audio->buses[i].used = false;
        }
    }

    /* Global reverb node + its delay lines. */
    if (audio->reverb_inited) {
        ma_node_uninit(&audio->reverb.base, NULL);
        audio->reverb_inited = false;
    }
    fv_free(&audio->reverb.fv);

    /* Free all sound PCM data. */
    for (int i = 0; i < JCE_MAX_SOUNDS; i++) {
        if (audio->sound_used[i]) {
            JCE_FREE(audio->sounds[i].pcm_data);
            audio->sounds[i].pcm_data = NULL;
            audio->sound_used[i] = false;
        }
    }

    if (audio->engine_inited)
        ma_engine_uninit(&audio->engine);

    JCE_FREE(audio);
    LOG_INFO("jce_audio", "audio system destroyed");
}

/* -- Sound loading -------------------------------------------------- */

static int alloc_buffer_slot(const JceAudio *audio)
{
    for (int i = 0; i < JCE_MAX_SOUNDS; i++) {
        if (!audio->sound_used[i])
            return i;
    }
    return -1;
}

/* Check if path ends with a given suffix (case-insensitive). */
static bool has_ext(const char *path, const char *ext)
{
    size_t plen = strlen(path);
    size_t elen = strlen(ext);
    if (plen < elen) return false;
    return SDL_strcasecmp(path + plen - elen, ext) == 0;
}

/* Decode WAV or OGG from memory using miniaudio's built-in decoders.
   Output is always s16 PCM. */
static JceSound load_from_memory(JceAudio *audio, int slot,
                                  const uint8_t *data, size_t size,
                                  const char *path)
{
    /* ── M4A / AAC-in-MP4 detection ─────────────────────────────── */
    if (jce_m4a_is_mp4_container(data, size)) {
        int16_t *pcm = NULL;
        uint32_t frames = 0, ch = 0, sr = 0;
        if (jce_m4a_decode_to_pcm(data, size, &pcm, &frames, &ch, &sr)) {
            audio->sounds[slot].pcm_data    = pcm;
            audio->sounds[slot].frame_count = (ma_uint64)frames;
            audio->sounds[slot].channels    = ch;
            audio->sounds[slot].sample_rate = sr;
            audio->sound_used[slot]         = true;
            LOG_DEBUG("jce_audio", "loaded M4A '%s' (%u Hz, %uch, %u frames)",
                      path, sr, ch, frames);
            return (JceSound)(slot + 1);
        }
        LOG_WARN("jce_audio", "M4A decode failed for '%s', trying miniaudio", path);
        /* Fall through to miniaudio as last resort. */
    }

    ma_decoder_config cfg = ma_decoder_config_init(
        ma_format_s16, 0 /* auto channels */, 0 /* auto sample rate */);
    ma_decoder decoder;

    /* Wire our Opus custom backend so .opus / Ogg-Opus is handled
       transparently (miniaudio probes custom backends before
       built-ins, so an Ogg page carrying OpusHead routes here). */
    static const ma_decoding_backend_vtable *jce_custom_backends[] = {
        &g_jce_ma_opus_backend_vtable,
    };
    cfg.ppCustomBackendVTables = (ma_decoding_backend_vtable **)jce_custom_backends;
    cfg.customBackendCount     = (ma_uint32)(sizeof(jce_custom_backends)
                                            / sizeof(jce_custom_backends[0]));

    /* Hint the encoding format from magic bytes so miniaudio picks
       the correct built-in decoder (dr_mp3, dr_wav, dr_flac, stb_vorbis).
       Note: we do NOT hint Ogg-Opus as vorbis — miniaudio's custom-backend
       probe phase (which runs first) already routes OpusHead pages to
       g_jce_ma_opus_backend_vtable. Plain Ogg-Vorbis still falls through. */
    if (size >= 4 && data[0] == 'O' && data[1] == 'g'
                  && data[2] == 'g' && data[3] == 'S') {
        /* Sniff for OpusHead — if present, leave format unknown so the
           custom backend wins; otherwise hint vorbis. */
        bool is_opus = false;
        for (size_t i = 28; i + 8 <= size && i < 80; ++i) {
            if (data[i] == 'O' && memcmp(data + i, "OpusHead", 8) == 0) {
                is_opus = true; break;
            }
        }
        if (!is_opus) cfg.encodingFormat = ma_encoding_format_vorbis;
    } else if (size >= 4 && data[0] == 'f' && data[1] == 'L'
                         && data[2] == 'a' && data[3] == 'C') {
        cfg.encodingFormat = ma_encoding_format_flac;
    } else if (size >= 4 && data[0] == 'R' && data[1] == 'I'
                         && data[2] == 'F' && data[3] == 'F') {
        cfg.encodingFormat = ma_encoding_format_wav;
    } else if (size >= 3 && data[0] == 'I' && data[1] == 'D'
                         && data[2] == '3') {
        /* ID3v2 tag header — almost always an MP3 file. */
        cfg.encodingFormat = ma_encoding_format_mp3;
    } else if (size >= 2 && data[0] == 0xFF
               && (data[1] & 0xE0) == 0xE0) {
        /* MPEG audio sync word (0xFFE0+): MP3 / MP2 / MP1. */
        cfg.encodingFormat = ma_encoding_format_mp3;
    }

    ma_result res = ma_decoder_init_memory(data, size, &cfg, &decoder);

    /* If the hinted format failed, retry with auto-detection. */
    if (res != MA_SUCCESS && cfg.encodingFormat != ma_encoding_format_unknown) {
        cfg.encodingFormat = ma_encoding_format_unknown;
        res = ma_decoder_init_memory(data, size, &cfg, &decoder);
    }
    if (res != MA_SUCCESS) {
        /* Log the first bytes to help diagnose unsupported files. */
        char hdr[48] = {0};
        size_t hlen = size < 16 ? size : 16;
        for (size_t i = 0; i < hlen; ++i) {
            snprintf(hdr + i * 3, sizeof(hdr) - i * 3, "%02X ", data[i]);
        }
        LOG_ERROR("jce_audio",
            "decode failed for '%s' (ma_result=%d, size=%zu, header=[%s])",
            path, (int)res, size, hdr);
        return JCE_SOUND_INVALID;
    }

    /* Get total frame count. */
    ma_uint64 total_frames = 0;
    ma_decoder_get_length_in_pcm_frames(&decoder, &total_frames);

    ma_uint32 channels = decoder.outputChannels;
    ma_uint32 rate     = decoder.outputSampleRate;

    if (total_frames == 0) {
        /* Unknown length (streaming format) — decode in chunks. */
        size_t alloc_frames = 1024 * 256;
        size_t used_frames  = 0;
        int16_t *pcm = (int16_t *)JCE_MALLOC(alloc_frames * channels * sizeof(int16_t));
        if (!pcm) {
            ma_decoder_uninit(&decoder);
            return JCE_SOUND_INVALID;
        }

        for (;;) {
            if (used_frames + 4096 > alloc_frames) {
                alloc_frames *= 2;
                int16_t *tmp = (int16_t *)JCE_REALLOC(pcm,
                    alloc_frames * channels * sizeof(int16_t));
                if (!tmp) {
                    JCE_FREE(pcm);
                    ma_decoder_uninit(&decoder);
                    return JCE_SOUND_INVALID;
                }
                pcm = tmp;
            }
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(&decoder, pcm + used_frames * channels,
                                        4096, &read);
            if (read == 0) break;
            used_frames += (size_t)read;
        }

        total_frames = (ma_uint64)used_frames;
        audio->sounds[slot].pcm_data = pcm;
    } else {
        /* Known length — single allocation. */
        void *pcm = JCE_MALLOC((size_t)(total_frames * channels * sizeof(int16_t)));
        if (!pcm) {
            ma_decoder_uninit(&decoder);
            return JCE_SOUND_INVALID;
        }

        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(&decoder, pcm, total_frames, &frames_read);
        total_frames = frames_read;
        audio->sounds[slot].pcm_data = pcm;
    }

    ma_decoder_uninit(&decoder);

    audio->sounds[slot].frame_count = total_frames;
    audio->sounds[slot].channels    = channels;
    audio->sounds[slot].sample_rate = rate;
    audio->sound_used[slot]         = true;

    LOG_DEBUG("jce_audio", "loaded '%s' (%u Hz, %uch, %llu frames)",
              path, rate, channels, (unsigned long long)total_frames);
    return (JceSound)(slot + 1);
}

JceSound jce_audio_load_pcm(JceAudio *audio,
                             const void *pcm_data, uint32_t pcm_size,
                             uint16_t channels, uint32_t sample_rate,
                             uint16_t bits_per_sample)
{
    if (!audio || !pcm_data || pcm_size == 0) return JCE_SOUND_INVALID;

    int slot = alloc_buffer_slot(audio);
    if (slot < 0) return JCE_SOUND_INVALID;

    /* Convert 8-bit to 16-bit if needed, or just copy 16-bit. */
    ma_uint64 frame_count;
    void *pcm_copy;

    if (bits_per_sample == 8) {
        frame_count = pcm_size / channels;
        size_t out_size = (size_t)(frame_count * channels * sizeof(int16_t));
        pcm_copy = JCE_MALLOC(out_size);
        if (!pcm_copy) return JCE_SOUND_INVALID;

        /* Convert u8 → s16. */
        const uint8_t *src = (const uint8_t *)pcm_data;
        int16_t *dst = (int16_t *)pcm_copy;
        for (uint32_t i = 0; i < pcm_size; i++)
            dst[i] = (int16_t)((src[i] - 128) * 256);
    } else if (bits_per_sample == 16) {
        frame_count = pcm_size / (channels * 2);
        pcm_copy = JCE_MALLOC(pcm_size);
        if (!pcm_copy) return JCE_SOUND_INVALID;
        memcpy(pcm_copy, pcm_data, pcm_size);
    } else {
        return JCE_SOUND_INVALID;
    }

    audio->sounds[slot].pcm_data    = pcm_copy;
    audio->sounds[slot].frame_count = frame_count;
    audio->sounds[slot].channels    = channels;
    audio->sounds[slot].sample_rate = sample_rate;
    audio->sound_used[slot]         = true;

    return (JceSound)(slot + 1);
}

static JceSound jce_audio_load_inner(JceAudio *audio, const JcePakArchive *pak,
                                     const char *path)
{
    if (!audio || !pak || !path) return JCE_SOUND_INVALID;

    const JcePakAsset *asset = jce_pak_find(pak, path);
    if (!asset) {
        LOG_ERROR("jce_audio", "asset '%s' not found in PAK", path);
        return JCE_SOUND_INVALID;
    }

    /* Decompress asset from PAK. */
    void *raw = JCE_MALLOC((size_t)asset->original_size);
    if (!raw) return JCE_SOUND_INVALID;

    size_t decoded = jce_pak_decompress(asset, raw, (size_t)asset->original_size);
    if (decoded == 0) {
        LOG_ERROR("jce_audio", "decompress failed for '%s'", path);
        JCE_FREE(raw);
        return JCE_SOUND_INVALID;
    }

    /* ── Cooked path: .jceasset AUDIO_INFO + AUDIO_PCM → direct load ── */
    if (jce_asset_is_cooked(raw, decoded)) {
        JceAssetView view;
        if (!jce_asset_open(&view, raw, decoded)) {
            JCE_FREE(raw);
            return JCE_SOUND_INVALID;
        }

        const JceAssetChunkEntry *info_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_INFO);
        const JceAssetChunkEntry *pcm_c =
            jce_asset_find_chunk(&view, JCEASSET_CHUNK_AUDIO_PCM);

        if (!info_c || !pcm_c) {
            JCE_FREE(raw);
            return JCE_SOUND_INVALID;
        }

        JceAssetAudioInfo ainfo;
        if (jce_asset_chunk_data(&view, info_c,
                                  &ainfo, sizeof(ainfo)) == 0) {
            JCE_FREE(raw);
            return JCE_SOUND_INVALID;
        }

        uint32_t pcm_size = (uint32_t)pcm_c->original_size;
        void *pcm_data = JCE_MALLOC(pcm_size);
        if (!pcm_data) { JCE_FREE(raw); return JCE_SOUND_INVALID; }

        if (jce_asset_chunk_data(&view, pcm_c, pcm_data, pcm_size) == 0) {
            JCE_FREE(pcm_data);
            JCE_FREE(raw);
            return JCE_SOUND_INVALID;
        }

        JCE_FREE(raw);

        JceSound result = jce_audio_load_pcm(audio, pcm_data, pcm_size,
                                              ainfo.channels, ainfo.sample_rate,
                                              ainfo.bits_per_sample);
        JCE_FREE(pcm_data);
        return result;
    }

    /* ── Raw path: OGG/WAV → miniaudio decode ── */
    int slot = alloc_buffer_slot(audio);
    if (slot < 0) {
        LOG_WARN("jce_audio", "no free buffer slots");
        JCE_FREE(raw);
        return JCE_SOUND_INVALID;
    }

    JceSound result = load_from_memory(audio, slot,
                                        (const uint8_t *)raw, decoded, path);
    JCE_FREE(raw);
    return result;
}

JceSound jce_audio_load(JceAudio *audio, const JcePakArchive *pak, const char *path)
{
    JCE_PROFILE_ZONE_N("Audio::Load");
    JceSound result = jce_audio_load_inner(audio, pak, path);
    JCE_PROFILE_ZONE_END;
    return result;
}

void jce_audio_unload(JceAudio *audio, JceSound snd)
{
    if (!audio || snd == JCE_SOUND_INVALID) return;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return;

    /* Stop and uninit any voice using this sound. */
    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        if (audio->voices[i].inited && audio->voices[i].sound_slot == slot)
            uninit_voice(&audio->voices[i]);
    }

    JCE_FREE(audio->sounds[slot].pcm_data);
    audio->sounds[slot].pcm_data = NULL;
    audio->sound_used[slot] = false;
}

/* -- Playback ------------------------------------------------------- */

/* JceVoice handle = (generation << 16) | (slot + 1). The +1 keeps a valid
   handle non-zero (0 == JCE_VOICE_INVALID); the generation makes a handle to
   a since-recycled slot resolve as stale instead of controlling a new sound. */
static JceVoice pack_voice(JceAudio *audio, int slot)
{
    uint32_t gen = audio->voices[slot].generation & 0xFFFFu;
    return (JceVoice)((gen << 16) | (((uint32_t)slot + 1u) & 0xFFFFu));
}

/* Decode + validate a handle to a live slot index, or -1 if stale/invalid. */
static int resolve_voice(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return -1;
    int idx = (int)((uint32_t)voice & 0xFFFFu) - 1;
    uint32_t gen = ((uint32_t)voice >> 16) & 0xFFFFu;
    if (idx < 0 || idx >= JCE_MAX_VOICES) return -1;
    if (!audio->voices[idx].inited) return -1;
    if ((audio->voices[idx].generation & 0xFFFFu) != gen) return -1;
    return idx;
}

static int alloc_voice(JceAudio *audio)
{
    int found = -1;

    /* 1: an unused slot. */
    for (int i = 0; i < JCE_MAX_VOICES && found < 0; i++)
        if (!audio->voices[i].inited) found = i;

    /* 2: reclaim a finished (no longer playing) voice. */
    if (found < 0) {
        for (int i = 0; i < JCE_MAX_VOICES && found < 0; i++) {
            if (!ma_sound_is_playing(&audio->voices[i].sound)) {
                uninit_voice(&audio->voices[i]);
                found = i;
            }
        }
    }

    /* 3: virtualization — the pool is full and everything is playing. Steal the
       OLDEST one-shot voice rather than dropping the new sound; prefer non-
       looping victims so we don't cut background music. */
    if (found < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < JCE_MAX_VOICES; i++) {
            if (audio->voices[i].inited
                && !ma_sound_is_looping(&audio->voices[i].sound)
                && audio->voices[i].play_seq < oldest) {
                oldest = audio->voices[i].play_seq;
                found = i;
            }
        }
        if (found < 0) {                  /* all voices loop → steal the oldest */
            oldest = UINT64_MAX;
            for (int i = 0; i < JCE_MAX_VOICES; i++) {
                if (audio->voices[i].inited && audio->voices[i].play_seq < oldest) {
                    oldest = audio->voices[i].play_seq;
                    found = i;
                }
            }
        }
        if (found >= 0) uninit_voice(&audio->voices[found]);
    }

    if (found >= 0)
        audio->voices[found].play_seq = ++audio->play_counter;
    return found;
}

/* The node a voice should feed into: its assigned bus group when routed,
   otherwise the global output node (reverb if live, else endpoint). */
static ma_node *voice_target_node(JceAudio *audio, const VoiceSlot *v)
{
    if (v->bus >= 0 && v->bus < JCE_MAX_BUSES && audio->buses[v->bus].used)
        return (ma_node *)&audio->buses[v->bus].group;
    return audio_output_node(audio);
}

/* Insert a per-voice low-pass node (sound → lpf → target) for occlusion
   muffling, starting at bypass (22050 Hz). On ANY failure the sound keeps its
   default endpoint attachment, so audio still plays (just without the muffle).
   `target` is the voice's bus group or the global output node. */
static void voice_attach_lpf(JceAudio *audio, VoiceSlot *v)
{
    ma_engine *e = &audio->engine;
    ma_lpf_node_config cfg = ma_lpf_node_config_init(
        ma_engine_get_channels(e), ma_engine_get_sample_rate(e), 22050.0, 2);
    if (ma_lpf_node_init(ma_engine_get_node_graph(e), &cfg, NULL, &v->lpf) != MA_SUCCESS)
        return;
    ma_node *target = voice_target_node(audio, v);
    if (ma_node_attach_output_bus(&v->lpf, 0, target, 0) != MA_SUCCESS
        || ma_node_attach_output_bus(&v->sound, 0, &v->lpf, 0) != MA_SUCCESS) {
        ma_lpf_node_uninit(&v->lpf, NULL);
        return;
    }
    v->lpf_ok     = true;
    v->lpf_cutoff = 22050.0f;
}

JceVoice jce_audio_play(JceAudio *audio, JceSound snd,
                         bool loop, float volume, float pitch)
{
    JCE_PROFILE_ZONE_N("Audio::Play");
    if (!audio || snd == JCE_SOUND_INVALID) { JCE_PROFILE_ZONE_END; return JCE_VOICE_INVALID; }

    int buf_slot = (int)snd - 1;
    if (buf_slot < 0 || buf_slot >= JCE_MAX_SOUNDS
        || !audio->sound_used[buf_slot]) {
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    int vi = alloc_voice(audio);
    if (vi < 0) {
        LOG_WARN("jce_audio", "no free voices");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    SoundSlot *s = &audio->sounds[buf_slot];
    VoiceSlot *v = &audio->voices[vi];

    /* Create an audio buffer that references the sound's PCM data.
       Each voice gets its own buffer with an independent read cursor. */
    ma_audio_buffer_config buf_cfg = ma_audio_buffer_config_init(
        ma_format_s16, s->channels, s->frame_count, s->pcm_data, NULL);
    buf_cfg.sampleRate = s->sample_rate;

    if (ma_audio_buffer_init(&buf_cfg, &v->buffer) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_audio_buffer_init failed");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    if (ma_sound_init_from_data_source(&audio->engine,
            &v->buffer, 0, NULL, &v->sound) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_init_from_data_source failed");
        ma_audio_buffer_uninit(&v->buffer);
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    ma_sound_set_volume(&v->sound, volume);
    ma_sound_set_pitch(&v->sound, pitch);
    ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_start(&v->sound);

    v->inited = true;
    v->sound_slot = buf_slot;
    voice_attach_lpf(audio, v);
    JCE_PROFILE_ZONE_END;
    return pack_voice(audio, vi);
}

/* ── Streaming source ──────────────────────────────────────────── */

/* miniaudio passes &v->stream_ds (NOT a VoiceSlot*) to vtable callbacks.
 * Recover the enclosing VoiceSlot via offsetof container_of. */
static inline VoiceSlot *voice_from_ds(ma_data_source *ds) {
    return (VoiceSlot *)((char *)ds - offsetof(VoiceSlot, stream_ds));
}

static ma_result stream_ds_on_read(ma_data_source *ds,
                                    void *out, ma_uint64 frame_count,
                                    ma_uint64 *frames_read)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (!v->stream_on_read) {
        if (frames_read) *frames_read = 0;
        return MA_AT_END;
    }
    /* Cap to uint32 — miniaudio buffers are small per callback. */
    uint32_t want = frame_count > 0xffffffffull
        ? 0xffffffffu : (uint32_t)frame_count;
    /* The pull callback (jce_audio_stream_pull) always returns the full
     * requested count, padding silence on under-run/EOF. So we can just
     * forward its output directly. */
    uint32_t got = v->stream_on_read(v->stream_ud,
                                     (int16_t *)out, want);
    if (got == 0) {
        /* Defensive: pad silence here too in case a future pull impl
         * returns short. */
        memset(out, 0, (size_t)want * v->stream_channels * sizeof(int16_t));
        got = want;
    }
    v->stream_cursor += got;
    if (frames_read) *frames_read = got;
    return MA_SUCCESS;
}

static ma_result stream_ds_on_seek(ma_data_source *ds, ma_uint64 frame_index)
{
    (void)ds; (void)frame_index;
    /* Seeks are driven by the upstream JceAudioStream, not via miniaudio. */
    return MA_NOT_IMPLEMENTED;
}

static ma_result stream_ds_on_get_data_format(ma_data_source *ds,
                                              ma_format *format,
                                              ma_uint32 *channels,
                                              ma_uint32 *sample_rate,
                                              ma_channel *channel_map,
                                              size_t channel_map_cap)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (format) *format = ma_format_s16;
    if (channels) *channels = v->stream_channels;
    if (sample_rate) *sample_rate = v->stream_samplerate;
    (void)channel_map; (void)channel_map_cap;
    return MA_SUCCESS;
}

static ma_result stream_ds_on_get_cursor(ma_data_source *ds,
                                          ma_uint64 *cursor)
{
    VoiceSlot *v = voice_from_ds(ds);
    if (cursor) *cursor = v->stream_cursor;
    return MA_SUCCESS;
}

static ma_data_source_vtable g_stream_vtable = {
    stream_ds_on_read,
    stream_ds_on_seek,
    stream_ds_on_get_data_format,
    stream_ds_on_get_cursor,
    NULL, /* onGetLength: unknown for streams */
    NULL, /* onSetLooping */
    0
};

JceVoice jce_audio_play_stream(JceAudio *audio,
                                JceAudioStreamPullFn on_read, void *ud,
                                uint16_t channels, uint32_t sample_rate,
                                float volume, float pitch)
{
    JCE_PROFILE_ZONE_N("Audio::PlayStream");
    if (!audio || !on_read || channels == 0 || sample_rate == 0) {
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    int vi = alloc_voice(audio);
    if (vi < 0) {
        LOG_WARN("jce_audio", "no free voices for stream");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    VoiceSlot *v = &audio->voices[vi];

    ma_data_source_config ds_cfg = ma_data_source_config_init();
    ds_cfg.vtable = &g_stream_vtable;
    if (ma_data_source_init(&ds_cfg, &v->stream_ds) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_data_source_init failed");
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    v->is_stream         = true;
    v->stream_on_read    = on_read;
    v->stream_ud         = ud;
    v->stream_channels   = channels;
    v->stream_samplerate = sample_rate;
    v->stream_cursor     = 0;

    if (ma_sound_init_from_data_source(&audio->engine,
            &v->stream_ds, 0, NULL, &v->sound) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_init_from_data_source (stream) failed");
        ma_data_source_uninit(&v->stream_ds);
        v->is_stream = false;
        JCE_PROFILE_ZONE_END;
        return JCE_VOICE_INVALID;
    }

    ma_sound_set_volume(&v->sound, volume);
    ma_sound_set_pitch(&v->sound, pitch);
    ma_sound_set_looping(&v->sound, MA_FALSE);
    ma_sound_start(&v->sound);

    v->inited     = true;
    v->sound_slot = -1;
    voice_attach_lpf(audio, v);
    JCE_PROFILE_ZONE_END;
    return pack_voice(audio, vi);
}

void jce_audio_stop(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    uninit_voice(&audio->voices[idx]);
}

/* Occlusion muffle: set the per-voice low-pass cutoff (22050 = bypass). No-op
   if the voice has no lpf node. Skips reinit for sub-perceptual changes. */
void jce_audio_set_lowpass(JceAudio *audio, JceVoice voice, float cutoff_hz)
{
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return;
    VoiceSlot *v = &audio->voices[idx];
    if (!v->lpf_ok) return;
    if (cutoff_hz < 20.0f)    cutoff_hz = 20.0f;
    if (cutoff_hz > 22050.0f) cutoff_hz = 22050.0f;
    if (cutoff_hz > v->lpf_cutoff - 25.0f && cutoff_hz < v->lpf_cutoff + 25.0f)
        return;
    ma_lpf_config lcfg = ma_lpf_config_init(
        ma_format_f32, ma_engine_get_channels(&audio->engine),
        ma_engine_get_sample_rate(&audio->engine), (double)cutoff_hz, 2);
    if (ma_lpf_node_reinit(&lcfg, &v->lpf) == MA_SUCCESS)
        v->lpf_cutoff = cutoff_hz;
}

void jce_audio_pause(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_stop(&audio->voices[idx].sound);
}

void jce_audio_resume(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    if (!ma_sound_is_playing(&audio->voices[idx].sound))
        ma_sound_start(&audio->voices[idx].sound);
}

void jce_audio_set_volume(JceAudio *audio, JceVoice voice, float volume)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_volume(&audio->voices[idx].sound, volume);
}

void jce_audio_set_pitch(JceAudio *audio, JceVoice voice, float pitch)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_pitch(&audio->voices[idx].sound, pitch);
}

void jce_audio_set_looping(JceAudio *audio, JceVoice voice, bool loop)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_looping(&audio->voices[idx].sound, loop ? MA_TRUE : MA_FALSE);
}

bool jce_audio_is_playing(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return false;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return false;

    return ma_sound_is_playing(&audio->voices[idx].sound) != 0;
}

/* -- Global --------------------------------------------------------- */

void jce_audio_set_master_volume(JceAudio *audio, float volume)
{
    if (!audio) return;
    ma_engine_set_volume(&audio->engine, volume);
}

void jce_audio_stop_all(JceAudio *audio)
{
    if (!audio) return;
    for (int i = 0; i < JCE_MAX_VOICES; i++)
        uninit_voice(&audio->voices[i]);
}

/* -- Mixer buses ---------------------------------------------------- */

bool jce_audio_bus_create(JceAudio *audio, const char *name)
{
    if (!audio || !name || !name[0]) return false;
    if (SDL_strcasecmp(name, "Master") == 0) return true; /* implicit endpoint */
    if (audio_find_bus(audio, name) >= 0) return true;     /* already exists */

    int slot = -1;
    for (int i = 0; i < JCE_MAX_BUSES; i++)
        if (!audio->buses[i].used) { slot = i; break; }
    if (slot < 0) {
        LOG_WARN("jce_audio", "bus pool exhausted (max %d)", JCE_MAX_BUSES);
        return false;
    }

    BusSlot *b = &audio->buses[slot];
    /* Group attaches to the engine endpoint by default; we re-route it to the
     * reverb node when one is live so bus audio is reverberated. */
    if (ma_sound_group_init(&audio->engine, 0, NULL, &b->group) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_group_init failed for bus '%s'", name);
        return false;
    }
    if (audio->reverb_inited)
        ma_node_attach_output_bus(&b->group, 0, &audio->reverb.base, 0);
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->used = true;
    return true;
}

void jce_audio_bus_set_volume(JceAudio *audio, const char *name, float volume)
{
    if (!audio || !name) return;
    if (volume < 0.0f) volume = 0.0f;
    if (SDL_strcasecmp(name, "Master") == 0) {
        ma_engine_set_volume(&audio->engine, volume);
        return;
    }
    int idx = audio_find_bus(audio, name);
    if (idx < 0) return;
    ma_sound_group_set_volume(&audio->buses[idx].group, volume);
}

void jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice,
                             const char *bus_name)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0) return;
    VoiceSlot *v = &audio->voices[idx];

    int bus = audio_find_bus(audio, bus_name);  /* -1 => direct/Master */
    v->bus = bus;

    /* Reattach the voice's output edge (lpf if present, else the sound) to the
     * new target.  On failure the prior attachment stays, so audio keeps
     * flowing (just on the old bus). */
    ma_node *target = voice_target_node(audio, v);
    ma_node *src    = v->lpf_ok ? (ma_node *)&v->lpf : (ma_node *)&v->sound;
    ma_node_attach_output_bus(src, 0, target, 0);
}

/* -- Global reverb -------------------------------------------------- */

void jce_audio_set_reverb(JceAudio *audio, const JceAudioReverbParams *params)
{
    if (!audio || !params) return;

    float wet = params->wet_mix;
    float dry = params->dry_mix;
    if (wet < 0.0f) wet = 0.0f;

    /* Lazily stand up the reverb node the first time a zone asks for wet > 0.
     * Until then the mix stays fully dry with zero overhead. */
    if (wet > 0.0001f && !audio->reverb_inited) {
        if (!audio_init_reverb(audio)) return;  /* stays dry on failure */
    }
    if (!audio->reverb_inited) return;

    /* Map the generic preset onto Freeverb tuning:
     *   roomsize  <- decay_seconds (longer tail => higher comb feedback)
     *   damp      <- damping, raised when lowpass_hz is low (dark rooms)   */
    float decay    = params->decay_seconds;
    float roomsize = decay <= 0.0f ? 0.5f
                   : decay / (decay + 1.2f);          /* 0..~0.98 asymptote */
    if (roomsize > 0.98f) roomsize = 0.98f;

    float damp = params->damping;
    if (params->lowpass_hz > 0.0f && params->lowpass_hz < 22050.0f) {
        float lp = 1.0f - (params->lowpass_hz / 22050.0f);  /* darker => more */
        if (lp > damp) damp = lp;
    }
    if (damp > 1.0f) damp = 1.0f;
    if (dry > 1.0f) dry = 1.0f;

    fv_set_params(&audio->reverb.fv, wet, dry, roomsize, damp);
    audio->reverb_wet = wet;
}

JceSound jce_audio_load_memory(JceAudio *audio, const void *data,
                                uint32_t size, const char *hint_path)
{
    if (!audio || !data || size == 0) return JCE_SOUND_INVALID;
    int slot = alloc_buffer_slot(audio);
    if (slot < 0) return JCE_SOUND_INVALID;
    return load_from_memory(audio, slot,
                            (const uint8_t *)data, (size_t)size,
                            hint_path ? hint_path : "<memory>");
}

float jce_audio_get_duration(const JceAudio *audio, JceSound snd)
{
    if (!audio || snd == JCE_SOUND_INVALID) return 0.0f;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return 0.0f;
    const SoundSlot *s = &audio->sounds[slot];
    if (s->sample_rate == 0) return 0.0f;
    return (float)s->frame_count / (float)s->sample_rate;
}

float jce_audio_get_time(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return 0.0f;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return 0.0f;
    float cursor = 0.0f;
    ma_sound_get_cursor_in_seconds((ma_sound *)&audio->voices[idx].sound, &cursor);
    return cursor;
}

void jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    int slot = audio->voices[idx].sound_slot;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS) return;
    ma_uint32 sr = audio->sounds[slot].sample_rate;
    if (sr == 0) return;
    ma_uint64 frame = (ma_uint64)(time_sec * (float)sr);
    if (frame > audio->sounds[slot].frame_count)
        frame = audio->sounds[slot].frame_count;
    ma_sound_seek_to_pcm_frame(&audio->voices[idx].sound, frame);
}

const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
                                       uint32_t *out_frame_count,
                                       uint32_t *out_channels)
{
    if (!audio || snd == JCE_SOUND_INVALID) return NULL;
    int slot = (int)snd - 1;
    if (slot < 0 || slot >= JCE_MAX_SOUNDS || !audio->sound_used[slot]) return NULL;
    const SoundSlot *s = &audio->sounds[slot];
    if (!s->pcm_data) return NULL;
    if (out_frame_count) *out_frame_count = (uint32_t)s->frame_count;
    if (out_channels)    *out_channels    = s->channels;
    return (const int16_t *)s->pcm_data;
}

/* ── 3D positional audio ──────────────────────────────────────────── */

void jce_audio_set_listener(JceAudio *audio, const JceAudioListener *l)
{
    if (!audio || !l) return;
    /* miniaudio supports multiple listeners; we use index 0 only. */
    ma_engine_listener_set_position(&audio->engine, 0,
                                    l->position[0], l->position[1], l->position[2]);
    ma_engine_listener_set_direction(&audio->engine, 0,
                                     l->forward[0], l->forward[1], l->forward[2]);
    ma_engine_listener_set_world_up(&audio->engine, 0,
                                    l->up[0], l->up[1], l->up[2]);
    ma_engine_listener_set_velocity(&audio->engine, 0,
                                    l->velocity[0], l->velocity[1], l->velocity[2]);
}

void jce_audio_set_doppler_factor(JceAudio *audio, float factor)
{
    if (!audio) return;
    if (factor < 0.0f) factor = 0.0f;
    /* miniaudio's Doppler is per-sound; apply to every active voice. */
    for (int i = 0; i < JCE_MAX_VOICES; ++i) {
        if (audio->voices[i].inited)
            ma_sound_set_doppler_factor(&audio->voices[i].sound, factor);
    }
}

void jce_audio_voice_set_3d(JceAudio *audio, JceVoice voice, bool spatial)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_spatialization_enabled(&audio->voices[idx].sound,
                                         spatial ? MA_TRUE : MA_FALSE);
}

void jce_audio_voice_set_position(JceAudio *audio, JceVoice voice,
                                    float x, float y, float z)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_position(&audio->voices[idx].sound, x, y, z);
}

void jce_audio_voice_set_velocity(JceAudio *audio, JceVoice voice,
                                    float vx, float vy, float vz)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;
    ma_sound_set_velocity(&audio->voices[idx].sound, vx, vy, vz);
}

void jce_audio_voice_set_attenuation(JceAudio *audio, JceVoice voice,
                                       JceAudioAttenuation model,
                                       float min_distance, float max_distance,
                                       float rolloff)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = resolve_voice(audio, voice);
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_attenuation_model ma_model = ma_attenuation_model_inverse;
    switch (model) {
        case JCE_AUDIO_ATTEN_NONE:        ma_model = ma_attenuation_model_none;        break;
        case JCE_AUDIO_ATTEN_INVERSE:     ma_model = ma_attenuation_model_inverse;     break;
        case JCE_AUDIO_ATTEN_LINEAR:      ma_model = ma_attenuation_model_linear;      break;
        case JCE_AUDIO_ATTEN_EXPONENTIAL: ma_model = ma_attenuation_model_exponential; break;
    }
    ma_sound *snd = &audio->voices[idx].sound;
    ma_sound_set_attenuation_model(snd, ma_model);
    if (min_distance > 0.0f) ma_sound_set_min_distance(snd, min_distance);
    if (max_distance > 0.0f) ma_sound_set_max_distance(snd, max_distance);
    if (rolloff      > 0.0f) ma_sound_set_rolloff(snd, rolloff);
}

#else /* JCE_NO_AUDIO */

JceAudio *jce_audio_create(void) {
    LOG_WARN("jce_audio", "audio disabled (JCE_NO_AUDIO)");
    return NULL;
}
void jce_audio_destroy(JceAudio *audio) { (void)audio; }
JceSound jce_audio_load(JceAudio *audio, JcePakArchive *pak, const char *path) {
    (void)audio; (void)pak; (void)path; return JCE_SOUND_INVALID;
}
JceSound jce_audio_load_pcm(JceAudio *audio, const void *pcm_data,
    uint32_t pcm_size, uint16_t channels, uint32_t sample_rate,
    uint16_t bits_per_sample) {
    (void)audio; (void)pcm_data; (void)pcm_size;
    (void)channels; (void)sample_rate; (void)bits_per_sample;
    return JCE_SOUND_INVALID;
}
void jce_audio_unload(JceAudio *audio, JceSound snd) { (void)audio; (void)snd; }
JceVoice jce_audio_play(JceAudio *audio, JceSound snd, bool loop, float volume, float pitch) {
    (void)audio; (void)snd; (void)loop; (void)volume; (void)pitch;
    return JCE_VOICE_INVALID;
}
void jce_audio_stop(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_pause(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_resume(JceAudio *audio, JceVoice voice) { (void)audio; (void)voice; }
void jce_audio_set_volume(JceAudio *audio, JceVoice voice,
                          float volume)
{
    (void)audio; (void)voice; (void)volume;
}
void jce_audio_set_lowpass(JceAudio *audio, JceVoice voice,
                           float cutoff_hz)
{
    (void)audio; (void)voice; (void)cutoff_hz;
}
void jce_audio_set_pitch(JceAudio *audio, JceVoice voice,
                         float pitch)
{
    (void)audio; (void)voice; (void)pitch;
}
void jce_audio_set_looping(JceAudio *audio, JceVoice voice,
                           bool loop)
{
    (void)audio; (void)voice; (void)loop;
}
bool jce_audio_is_playing(const JceAudio *audio,
                          JceVoice voice)
{
    (void)audio; (void)voice;
    return false;
}
void jce_audio_set_master_volume(JceAudio *audio, float volume) { (void)audio; (void)volume; }
void jce_audio_stop_all(JceAudio *audio) { (void)audio; }
bool jce_audio_bus_create(JceAudio *audio, const char *name) {
    (void)audio; (void)name; return false;
}
void jce_audio_bus_set_volume(JceAudio *audio, const char *name, float volume) {
    (void)audio; (void)name; (void)volume;
}
void jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice, const char *bus_name) {
    (void)audio; (void)voice; (void)bus_name;
}
void jce_audio_set_reverb(JceAudio *audio, const JceAudioReverbParams *params) {
    (void)audio; (void)params;
}
JceSound jce_audio_load_memory(JceAudio *audio, const void *data,
    uint32_t size, const char *hint_path) {
    (void)audio; (void)data; (void)size; (void)hint_path;
    return JCE_SOUND_INVALID;
}
float jce_audio_get_duration(const JceAudio *audio, JceSound snd) {
    (void)audio; (void)snd; return 0.0f;
}
float jce_audio_get_time(const JceAudio *audio, JceVoice voice) {
    (void)audio; (void)voice; return 0.0f;
}
void jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec) {
    (void)audio; (void)voice; (void)time_sec;
}
const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
    uint32_t *out_frame_count, uint32_t *out_channels) {
    (void)audio; (void)snd;
    if (out_frame_count) *out_frame_count = 0;
    if (out_channels) *out_channels = 0;
    return NULL;
}
JceVoice jce_audio_play_stream(JceAudio *audio,
                                JceAudioStreamPullFn on_read, void *ud,
                                uint16_t channels, uint32_t sample_rate,
                                float volume, float pitch) {
    (void)audio; (void)on_read; (void)ud; (void)channels;
    (void)sample_rate; (void)volume; (void)pitch;
    return JCE_VOICE_INVALID;
}
void jce_audio_set_listener(JceAudio *audio, const JceAudioListener *l) {
    (void)audio; (void)l;
}
void jce_audio_set_doppler_factor(JceAudio *audio, float factor) {
    (void)audio; (void)factor;
}
void jce_audio_voice_set_3d(JceAudio *audio, JceVoice voice, bool spatial) {
    (void)audio; (void)voice; (void)spatial;
}
void jce_audio_voice_set_position(JceAudio *audio, JceVoice voice,
                                    float x, float y, float z) {
    (void)audio; (void)voice; (void)x; (void)y; (void)z;
}
void jce_audio_voice_set_velocity(JceAudio *audio, JceVoice voice,
                                    float vx, float vy, float vz) {
    (void)audio; (void)voice; (void)vx; (void)vy; (void)vz;
}
void jce_audio_voice_set_attenuation(JceAudio *audio, JceVoice voice,
                                       JceAudioAttenuation model,
                                       float min_distance, float max_distance,
                                       float rolloff) {
    (void)audio; (void)voice; (void)model;
    (void)min_distance; (void)max_distance; (void)rolloff;
}

#endif /* JCE_NO_AUDIO */
