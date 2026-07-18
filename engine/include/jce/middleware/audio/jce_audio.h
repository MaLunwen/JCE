/*
 * jce_audio.h  Cross-platform audio system.
 *
 * Loads WAV/OGG from the PAK archive, manages voices for playback.
 */

#ifndef JCE_AUDIO_H
#define JCE_AUDIO_H


#include <jce/middleware/audio/jce_audio_types.h>
#include <jce/middleware/audio/jce_audio_dsp.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Forward declaration  avoids pulling in jce_pak_loader.h in every TU. */
typedef struct JcePakArchive JcePakArchive;

typedef struct JceAudio JceAudio;

/* -- Lifecycle ------------------------------------------------------ */

JCE_API JceAudio *jce_audio_create(void);
JCE_API void      jce_audio_destroy(JceAudio *audio);

/* -- Sound loading (from PAK) --------------------------------------- */

/* Load a sound from the PAK archive.  Supports .wav files.
   Returns JCE_SOUND_INVALID on failure. */
JCE_API JceSound  jce_audio_load(JceAudio *audio, const JcePakArchive *pak, const char *path);

/* Upload pre-decoded PCM data as a sound.
   channels: 1 or 2, bits: 8 or 16.
   Caller retains ownership of pcm_data. */
JceSound  jce_audio_load_pcm(JceAudio *audio,
                               const void *pcm_data, uint32_t pcm_size,
                               uint16_t channels, uint32_t sample_rate,
                               uint16_t bits_per_sample);

/* Unload a previously loaded sound. */
JCE_API void      jce_audio_unload(JceAudio *audio, JceSound snd);

/* -- Worker-decode + main-thread-register split --------------------- *
 *
 * jce_audio_load() does PAK decompress + decode + sound registration in
 * one call.  This pair separates the slow, variable-latency decode (which
 * is safe on any thread) from the registration (which mutates the audio
 * sound table and must stay on the thread that owns it — the main thread,
 * same as jce_audio_load today):
 *
 *   worker:      JceAudioCpu *c = jce_audio_decode_cpu(pak, path);
 *   main thread: JceSound     s = jce_audio_upload_cpu(audio, c);  // consumes c
 *   cancel:      jce_audio_cpu_free(c);                            // no register
 *
 * decode_cpu touches no JceAudio state (PAK decompress + miniaudio decode
 * to a standalone PCM buffer); upload_cpu registers the PCM via
 * jce_audio_load_pcm and frees `c`. */
typedef struct JceAudioCpu JceAudioCpu;
JCE_API JceAudioCpu *jce_audio_decode_cpu(const JcePakArchive *pak, const char *path);
/* Decode encoded audio or a cooked JCEA sound from borrowed memory.  The
 * returned CPU object owns its PCM copy and is independent of `data` after the
 * call.  This is the canonical entry point for VFS/bundle-backed bytes. */
JCE_API JceAudioCpu *jce_audio_decode_cpu_memory(const void *data, size_t size,
                                                 const char *hint_path);
JCE_API JceSound     jce_audio_upload_cpu(JceAudio *audio, JceAudioCpu *cpu);
JCE_API void         jce_audio_cpu_free(JceAudioCpu *cpu);

/* Load encoded audio or a cooked JCEA sound from memory. `hint_path` is used
 * only for diagnostics and encoded-format hints; payload representation wins. */
JCE_API JceSound jce_audio_load_memory(JceAudio *audio, const void *data,
                                       uint32_t size, const char *hint_path);

/* -- Queries -------------------------------------------------------- */

/* Duration of a loaded sound in seconds.  Returns 0 on error. */
JCE_API float     jce_audio_get_duration(const JceAudio *audio, JceSound snd);

/* Current playback position of a voice in seconds. */
JCE_API float     jce_audio_get_time(const JceAudio *audio, JceVoice voice);

/* Seek a playing/paused voice to a specific time (seconds). */
JCE_API void      jce_audio_seek(JceAudio *audio, JceVoice voice, float time_sec);

/* Access decoded PCM data of a loaded sound (16-bit signed).
   Returns NULL on error.  Caller must NOT free the returned pointer. */
const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
                                       uint32_t *out_frame_count,
                                       uint32_t *out_channels);

/* -- Playback ------------------------------------------------------- */

/* Play a sound.  Returns a voice handle for further control.
   volume: 0.01.0,  pitch: 1.0 = normal. */
JceVoice  jce_audio_play(JceAudio *audio, JceSound snd,
                          bool loop, float volume, float pitch);

JCE_API void      jce_audio_stop(JceAudio *audio, JceVoice voice);
JCE_API void      jce_audio_pause(JceAudio *audio, JceVoice voice);
JCE_API void      jce_audio_resume(JceAudio *audio, JceVoice voice);

JCE_API void      jce_audio_set_volume(JceAudio *audio, JceVoice voice, float volume);
/* Per-voice low-pass cutoff in Hz for occlusion muffling (22050 = bypass). */
JCE_API void      jce_audio_set_lowpass(JceAudio *audio, JceVoice voice, float cutoff_hz);
JCE_API void      jce_audio_set_pitch(JceAudio *audio, JceVoice voice, float pitch);
JCE_API void      jce_audio_set_looping(JceAudio *audio, JceVoice voice, bool loop);
JCE_API bool      jce_audio_is_playing(const JceAudio *audio, JceVoice voice);

/* -- Streaming playback ---------------------------------------------- */

/* Pull callback for jce_audio_play_stream(). Must write up to
 * `frames` interleaved s16 frames into `out` and return the number
 * actually produced. Returning 0 signals end-of-stream. Called from
 * the audio device thread. Must be thread-safe. */
typedef uint32_t (*JceAudioStreamPullFn)(void *ud,
                                          int16_t *out,
                                          uint32_t frames);

/* Begin streaming playback driven by a pull callback. Returns a voice
 * handle. The caller retains ownership of `ud` and is responsible for
 * keeping it valid until the voice is stopped (jce_audio_stop). */
JceVoice  jce_audio_play_stream(JceAudio *audio,
                                 JceAudioStreamPullFn on_read, void *ud,
                                 uint16_t channels, uint32_t sample_rate,
                                 float volume, float pitch);

/* -- Global --------------------------------------------------------- */

JCE_API void      jce_audio_set_master_volume(JceAudio *audio, float volume);
JCE_API void      jce_audio_stop_all(JceAudio *audio);

/* -- Mixer buses ---------------------------------------------------- */

/*
 * Named mixer buses backed by ma_sound_group nodes.  Voices routed to a
 * bus inherit that bus's gain, so the runtime can drive Music/SFX/Voice
 * sliders (resolved from a JceAudioMixer) straight onto the live mix.
 *
 * The "Master" bus always exists implicitly (it is the engine endpoint —
 * jce_audio_set_master_volume controls it).  Other buses are created by
 * name; creating the same name twice returns the existing bus.  Buses are
 * flat (one level under Master) on the audio side — the hierarchical
 * solo/mute/parent math lives in jce_audio_mixer.h and is collapsed into a
 * single resolved gain per bus that the caller pushes here each frame.
 *
 * Returns true on success.  All no-op safely when audio is disabled. */
JCE_API bool      jce_audio_bus_create(JceAudio *audio, const char *name);

/* Set the linear gain of a named bus (clamped >= 0).  No-op for unknown
 * names; the implicit "Master" bus maps to the engine master volume. */
JCE_API void      jce_audio_bus_set_volume(JceAudio *audio, const char *name,
                                           float volume);

/* Route a voice's output into the named bus.  Call after jce_audio_play.
 * Unknown bus name or "Master" routes the voice straight to the endpoint.
 * No-op on a stale/invalid voice. */
JCE_API void      jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice,
                                          const char *bus_name);

/* -- Insert-effect DSP chains (FEATURE 5.1) ------------------------- */

/*
 * Ordered DSP "insert" effects (EQ / compressor / limiter / delay; see
 * jce_audio_dsp.h) attachable to a mixer bus or a single voice.  Each effect
 * is appended to the target's chain and runs in order on the signal flowing
 * from the source into the bus/endpoint, via a custom node spliced into the
 * miniaudio node graph (source → [inserts] → bus group / reverb / endpoint).
 *
 * The chain is lazily created on the first add; while empty the original
 * routing (and the reverb/lowpass behavior) is left completely unchanged, so
 * a target with no inserts has zero added cost.
 *
 * The same JceAudioDspChain math runs offline over a known PCM buffer, which
 * is how the effects are unit-tested with no audio device.
 *
 * Returns the new effect's index (>=0) on success, or -1 on failure (unknown
 * bus/voice, chain full, audio disabled). */
JCE_API int  jce_audio_bus_add_effect(JceAudio *audio, const char *bus_name,
                                      const JceAudioEffectDesc *desc);
JCE_API int  jce_audio_voice_add_effect(JceAudio *audio, JceVoice voice,
                                        const JceAudioEffectDesc *desc);

/* Reconfigure the insert at `index` on a bus/voice chain.  Returns true on
 * success. */
JCE_API bool jce_audio_bus_set_effect(JceAudio *audio, const char *bus_name,
                                      uint32_t index,
                                      const JceAudioEffectDesc *desc);
JCE_API bool jce_audio_voice_set_effect(JceAudio *audio, JceVoice voice,
                                        uint32_t index,
                                        const JceAudioEffectDesc *desc);

/* Remove the insert at `index`.  Returns true if one was removed. */
JCE_API bool jce_audio_bus_remove_effect(JceAudio *audio, const char *bus_name,
                                         uint32_t index);
JCE_API bool jce_audio_voice_remove_effect(JceAudio *audio, JceVoice voice,
                                           uint32_t index);

/* Number of inserts currently on a bus/voice (0 if none / unknown). */
JCE_API uint32_t jce_audio_bus_effect_count(JceAudio *audio,
                                            const char *bus_name);
JCE_API uint32_t jce_audio_voice_effect_count(JceAudio *audio, JceVoice voice);

/* -- Global reverb (driven by reverb zones) ------------------------- */

/*
 * Generic reverb parameters consumed by the global reverb DSP node.  This
 * mirrors jce_reverb_zones.h's JceReverbPreset field-for-field so the
 * runtime can sample a blended zone preset and hand it straight here
 * without coupling the audio device to the (engine-agnostic) zone module.
 *
 * wet_mix/dry_mix are 0..1 send levels; decay_seconds is the tail length;
 * room_size (m) scales the pre-delay; damping/lowpass_hz roll off the wet
 * high frequencies.  diffusion/density are accepted for completeness but
 * are baked into the fixed comb/allpass network. */
typedef struct {
    float wet_mix;
    float dry_mix;
    float decay_seconds;
    float room_size;
    float damping;
    float diffusion;
    float density;
    float pre_delay_ms;
    float lowpass_hz;
} JceAudioReverbParams;

/* Apply the blended reverb preset to the global reverb node (created lazily
 * on first call).  Bus output is routed through the reverb node so the wet
 * tail is audible.  Passing wet_mix <= 0 leaves the dry signal untouched.
 * No-op safely when audio is disabled. */
JCE_API void      jce_audio_set_reverb(JceAudio *audio,
                                       const JceAudioReverbParams *params);

/* -- 3D positional audio -------------------------------------------- */

/*
 * Listener orientation.  Each frame the game updates the listener
 * with the camera (or player ear) transform; voices marked spatial
 * are then attenuated, panned, and Doppler-shifted relative to it.
 *
 * Coordinate convention matches miniaudio: right-handed, +Y up.
 * Position/forward/up/velocity are world-space.  Velocity is only
 * used for Doppler — pass {0,0,0} if you don't want Doppler.
 */
typedef struct {
    float position[3];
    float forward[3];
    float up[3];
    float velocity[3];
} JceAudioListener;

/* Update the (single) listener.  Safe to call every frame. */
JCE_API void jce_audio_set_listener(JceAudio *audio,
                                     const JceAudioListener *l);

/* Global Doppler factor.  1.0 = realistic, 0.0 = disabled. */
JCE_API void jce_audio_set_doppler_factor(JceAudio *audio, float factor);

/* Distance attenuation models — matches miniaudio. */
typedef enum {
    JCE_AUDIO_ATTEN_NONE        = 0,
    JCE_AUDIO_ATTEN_INVERSE     = 1,  /* 1/d falloff (default) */
    JCE_AUDIO_ATTEN_LINEAR      = 2,
    JCE_AUDIO_ATTEN_EXPONENTIAL = 3
} JceAudioAttenuation;

/* Enable / disable spatialisation on a voice.  Voices default to
 * non-spatial (UI sounds, music) — call this immediately after
 * jce_audio_play() to make a voice positional. */
JCE_API void jce_audio_voice_set_3d(JceAudio *audio, JceVoice voice,
                                     bool spatial);

/* World-space source position. */
JCE_API void jce_audio_voice_set_position(JceAudio *audio, JceVoice voice,
                                            float x, float y, float z);

/* World-space source velocity (m/s) — drives Doppler. */
JCE_API void jce_audio_voice_set_velocity(JceAudio *audio, JceVoice voice,
                                            float vx, float vy, float vz);

/* Per-voice attenuation parameters.
 *   min_distance — full volume up to this distance
 *   max_distance — silent past this distance (NONE/LINEAR);
 *                   ignored by INVERSE/EXPONENTIAL
 *   rolloff      — falloff factor (1.0 default).  Higher = quicker
 *                   attenuation. */
JCE_API void jce_audio_voice_set_attenuation(JceAudio *audio, JceVoice voice,
                                               JceAudioAttenuation model,
                                               float min_distance,
                                               float max_distance,
                                               float rolloff);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_H */
