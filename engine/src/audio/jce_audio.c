/*
 * jce_audio.c  miniaudio audio implementation.
 *
 * Loads WAV and OGG from PAK into PCM buffers, plays them via the
 * miniaudio engine.  Manages a pool of sounds and voices for
 * concurrent playback.
 */

#include <jce/audio/jce_audio.h>
#include <jce/audio/jce_m4a_decode.h>
#include <jce/core/pak_loader.h>
#include <jce/resource/jce_asset_format.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_profiler.h>

#ifndef JCE_NO_AUDIO

#include <miniaudio.h>
#include <SDL3/SDL.h>
#include "core/jce_memory.h"
#include "resource/jce_asset_reader.h"
#include <stdlib.h>
#include <string.h>

#define JCE_MAX_SOUNDS  64
#define JCE_MAX_VOICES  32

/* Each sound owns a block of decoded PCM (s16) data. */
typedef struct {
    void      *pcm_data;      /* JCE_MALLOC'd, s16 PCM */
    ma_uint64  frame_count;
    ma_uint32  channels;
    ma_uint32  sample_rate;
} SoundSlot;

/* Each voice is an independent playback instance. */
typedef struct {
    ma_audio_buffer buffer;   /* owns a read cursor over the SoundSlot PCM */
    ma_sound        sound;    /* attached to the engine */
    bool            inited;
    int             sound_slot;
} VoiceSlot;

struct JceAudio {
    ma_engine   engine;
    bool        engine_inited;

    SoundSlot   sounds[JCE_MAX_SOUNDS];
    bool        sound_used[JCE_MAX_SOUNDS];

    VoiceSlot   voices[JCE_MAX_VOICES];
};

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

    for (int i = 0; i < JCE_MAX_VOICES; i++)
        audio->voices[i].sound_slot = -1;

    LOG_SUCCESS("jce_audio", "miniaudio engine initialized");
    return audio;
}

static void uninit_voice(VoiceSlot *v)
{
    if (!v->inited) return;
    ma_sound_uninit(&v->sound);
    ma_audio_buffer_uninit(&v->buffer);
    v->inited = false;
    v->sound_slot = -1;
}

void jce_audio_destroy(JceAudio *audio)
{
    if (!audio) return;

    /* Uninit all voices first (they reference engine). */
    for (int i = 0; i < JCE_MAX_VOICES; i++)
        uninit_voice(&audio->voices[i]);

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

    /* Hint the encoding format from magic bytes so miniaudio picks
       the correct built-in decoder (dr_mp3, dr_wav, dr_flac, stb_vorbis). */
    if (size >= 4 && data[0] == 'O' && data[1] == 'g'
                  && data[2] == 'g' && data[3] == 'S') {
        cfg.encodingFormat = ma_encoding_format_vorbis;
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

static JceSound jce_audio_load_inner(JceAudio *audio, const PakArchive *pak,
                                     const char *path)
{
    if (!audio || !pak || !path) return JCE_SOUND_INVALID;

    const PakAsset *asset = pak_find(pak, path);
    if (!asset) {
        LOG_ERROR("jce_audio", "asset '%s' not found in PAK", path);
        return JCE_SOUND_INVALID;
    }

    /* Decompress asset from PAK. */
    void *raw = JCE_MALLOC((size_t)asset->original_size);
    if (!raw) return JCE_SOUND_INVALID;

    size_t decoded = pak_decompress(asset, raw, (size_t)asset->original_size);
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

JceSound jce_audio_load(JceAudio *audio, const PakArchive *pak, const char *path)
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

static int alloc_voice(JceAudio *audio)
{
    /* First pass: find an unused slot. */
    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        if (!audio->voices[i].inited)
            return i;
    }

    /* Second pass: reclaim a finished voice. */
    for (int i = 0; i < JCE_MAX_VOICES; i++) {
        if (!ma_sound_is_playing(&audio->voices[i].sound)) {
            uninit_voice(&audio->voices[i]);
            return i;
        }
    }

    return -1;
}

JceVoice jce_audio_play(JceAudio *audio, JceSound snd,
                         bool loop, float volume, float pitch)
{
    if (!audio || snd == JCE_SOUND_INVALID) return JCE_VOICE_INVALID;

    int buf_slot = (int)snd - 1;
    if (buf_slot < 0 || buf_slot >= JCE_MAX_SOUNDS
        || !audio->sound_used[buf_slot])
        return JCE_VOICE_INVALID;

    int vi = alloc_voice(audio);
    if (vi < 0) {
        LOG_WARN("jce_audio", "no free voices");
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
        return JCE_VOICE_INVALID;
    }

    if (ma_sound_init_from_data_source(&audio->engine,
            &v->buffer, 0, NULL, &v->sound) != MA_SUCCESS) {
        LOG_ERROR("jce_audio", "ma_sound_init_from_data_source failed");
        ma_audio_buffer_uninit(&v->buffer);
        return JCE_VOICE_INVALID;
    }

    ma_sound_set_volume(&v->sound, volume);
    ma_sound_set_pitch(&v->sound, pitch);
    ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_start(&v->sound);

    v->inited = true;
    v->sound_slot = buf_slot;
    return (JceVoice)(vi + 1);
}

void jce_audio_stop(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    uninit_voice(&audio->voices[idx]);
}

void jce_audio_pause(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_stop(&audio->voices[idx].sound);
}

void jce_audio_resume(JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    if (!ma_sound_is_playing(&audio->voices[idx].sound))
        ma_sound_start(&audio->voices[idx].sound);
}

void jce_audio_set_volume(JceAudio *audio, JceVoice voice, float volume)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_volume(&audio->voices[idx].sound, volume);
}

void jce_audio_set_pitch(JceAudio *audio, JceVoice voice, float pitch)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_pitch(&audio->voices[idx].sound, pitch);
}

void jce_audio_set_looping(JceAudio *audio, JceVoice voice, bool loop)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return;

    ma_sound_set_looping(&audio->voices[idx].sound, loop ? MA_TRUE : MA_FALSE);
}

bool jce_audio_is_playing(const JceAudio *audio, JceVoice voice)
{
    if (!audio || voice == JCE_VOICE_INVALID) return false;
    int idx = (int)voice - 1;
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
    int idx = (int)voice - 1;
    if (idx < 0 || idx >= JCE_MAX_VOICES || !audio->voices[idx].inited) return 0.0f;
    float cursor = 0.0f;
    ma_sound_get_cursor_in_seconds((ma_sound *)&audio->voices[idx].sound, &cursor);
    return cursor;
}

void jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec)
{
    if (!audio || voice == JCE_VOICE_INVALID) return;
    int idx = (int)voice - 1;
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

#else /* JCE_NO_AUDIO */

JceAudio *jce_audio_create(void) {
    LOG_WARN("jce_audio", "audio disabled (JCE_NO_AUDIO)");
    return NULL;
}
void jce_audio_destroy(JceAudio *audio) { (void)audio; }
JceSound jce_audio_load(JceAudio *audio, PakArchive *pak, const char *path) {
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

#endif /* JCE_NO_AUDIO */
