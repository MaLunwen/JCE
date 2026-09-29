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

/* An engine the shared output device does NOT pump -- inaudible, and driven
 * only by jce_audio_render_offline() below.  Everything else behaves
 * identically: sounds load, voices play, buses mix, inserts and the reverb
 * tail run.  Use it for an offline bounce, a headless/dedicated server, or a
 * test that must observe the real node graph without sound hardware.
 * jce_audio_render_offline() refuses an engine from jce_audio_create(),
 * because two pumps on one graph race for the same read cursors. */
JCE_API JceAudio *jce_audio_create_offline(void);

JCE_API void      jce_audio_destroy(JceAudio *audio);

/* -- Master mix tap -------------------------------------------------- */

/* All JceAudio engines in the process are summed by ONE shared output device
 * (the master bus — see .docs/AUDIO_MASTER_MIX_DESIGN.md).  The tap observes
 * that final mix: interleaved f32, 48000 Hz, 2 channels, called from the
 * audio device thread at a constant cadence (silence when engines are idle,
 * so the stream is gapless — recording-encoder friendly).  Keep the callback
 * cheap (copy to a queue).  One consumer at a time; fn=NULL clears the tap
 * and returns only after the callback can no longer fire into the old fn. */
typedef void (*JceAudioMasterTapFn)(void *ud, const float *pcm,
                                    uint32_t frames, uint32_t sample_rate,
                                    uint32_t channels);
JCE_API bool jce_audio_master_tap_set(JceAudioMasterTapFn fn, void *ud);

/* -- Sound loading (from PAK) --------------------------------------- */

/* Load a sound from the PAK archive.  Supports .wav files.
   Returns JCE_SOUND_INVALID on failure. */
JCE_API JceSound  jce_audio_load(JceAudio *audio, const JcePakArchive *pak, const char *path);

/* Upload pre-decoded PCM data as a sound.
   channels: 1 or 2, bits: 8 or 16.
   Caller retains ownership of pcm_data. */
JCE_API JceSound  jce_audio_load_pcm(JceAudio *audio,
                               const void *pcm_data, uint32_t pcm_size,
                               uint16_t channels, uint32_t sample_rate,
                               uint16_t bits_per_sample);

/* Load a sound for STREAMING: the encoded bytes stay resident and each voice
 * decodes them on demand, instead of one up-front decode into s16 PCM.
 *
 * Every clip was decompress-on-load, with no engine-provided alternative: a
 * five-minute stereo track cost ~50 MB resident and a full decode before the
 * first sample.  jce_audio_play_stream existed but is a bare pull callback --
 * the caller supplies its own decoder and its own thread-safe ring, which is
 * why its only in-tree consumer is the video viewer feeding an already
 * demuxed track.  This is the clip-level answer: pass a PAK (or NULL to read
 * the host filesystem) and play the handle like any other.
 *
 * COSTS AND LIMITS, because they decide which clips want this.  Memory drops
 * to the COMPRESSED size, so it is the right choice for music and ambience
 * and the wrong one for a footstep: each voice carries its own decoder and
 * decodes on the mix thread, so many concurrent streaming voices trade RAM
 * for CPU.  jce_audio_get_pcm_data returns NULL for a streaming sound -- there
 * is no decoded buffer to hand out, and that is how a caller can tell.
 *
 * Returns JCE_SOUND_INVALID if the asset is missing or not decodable; the
 * format is proven at load, not at first play, so a bad clip fails where the
 * caller can see it rather than as a silent voice later. */
JCE_API JceSound jce_audio_load_streaming(JceAudio *audio,
                                          const JcePakArchive *pak,
                                          const char *path);

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

/* Read back a decoded CPU sound without registering it in an audio device.
 * This is what makes jce_audio_decode_cpu_memory() the ONE decoder for the
 * whole engine: callers that only want PCM bytes (offline asset cooking, the
 * async loader's worker thread) go through it instead of driving a private
 * ma_decoder, which would silently miss the Opus custom backend and the
 * M4A/AAC path this module registers (audit A2-AUDIO-DECODE-DRIFT).
 *
 * The PCM stays owned by `cpu` — it dies with jce_audio_cpu_free() and is
 * consumed by jce_audio_upload_cpu(), so a caller that needs to outlive `cpu`
 * must copy.  Any out-param may be NULL.  Returns false when `cpu` is NULL or
 * carries no samples. */
JCE_API bool jce_audio_cpu_get_pcm(const JceAudioCpu *cpu,
                                   const void **out_pcm,
                                   uint32_t *out_pcm_bytes,
                                   uint16_t *out_channels,
                                   uint32_t *out_sample_rate,
                                   uint16_t *out_bits_per_sample);

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
JCE_API const int16_t *jce_audio_get_pcm_data(const JceAudio *audio, JceSound snd,
                                       uint32_t *out_frame_count,
                                       uint32_t *out_channels);

/* -- Playback ------------------------------------------------------- */

/* Play a sound.  Returns a voice handle for further control.
   volume: 0.01.0,  pitch: 1.0 = normal. */
JCE_API JceVoice  jce_audio_play(JceAudio *audio, JceSound snd,
                          bool loop, float volume, float pitch);

/* -- Voice priority ------------------------------------------------- *
 *
 * The voice pool is fixed (64).  When it is full and everything is playing,
 * something has to give, and WITHOUT A PRIORITY THE ONLY AVAILABLE ANSWER IS
 * AGE: a boss cue or a line of dialogue was stolen by whatever happened to
 * start after it.  Priority is what lets an author say which sounds matter.
 *
 * HIGHER IS MORE IMPORTANT, and 0 is normal.  That matches Unreal's
 * FSoundBase::Priority and INVERTS Unity's AudioSource.priority, where 0 is
 * the most important and 256 the least.  The reason is not taste: every
 * component struct in this engine is zero-initialised and every scene saved
 * before this field existed has a 0 in it, so 0 has to mean "what this engine
 * already did".  Unity's numbering would silently promote every existing
 * source to maximum importance.
 *
 * THE RULE, both halves of it:
 *   - the victim is the LOWEST priority voice, age breaking ties -- so a
 *     protected voice outlives an unprotected one regardless of age;
 *   - a voice is never stolen by a sound of STRICTLY LOWER priority.  When
 *     every live voice outranks the incoming one, the new sound is dropped
 *     and jce_audio_play_priority returns JCE_VOICE_INVALID.  That is Unreal's
 *     "prevent new" and Unity's virtualisation, and it is the half that makes
 *     the protection real: choosing a better victim still evicts the boss cue
 *     once every voice is a boss cue.
 *
 * Looping voices are still preferred as survivors over one-shots at equal
 * priority, because background music being cut is the failure the age-only
 * policy was already written to avoid. */
#define JCE_AUDIO_PRIORITY_NORMAL 0

/* Play at a given priority.  jce_audio_play is this with priority 0.
 * Returns JCE_VOICE_INVALID when the pool is full of higher-priority voices. */
JCE_API JceVoice  jce_audio_play_priority(JceAudio *audio, JceSound snd,
                          bool loop, float volume, float pitch, int priority);

/* Re-prioritise a voice that is already playing.  Takes effect on the next
 * allocation that has to steal.  No-op on a stale/invalid voice. */
JCE_API void      jce_audio_voice_set_priority(JceAudio *audio, JceVoice voice,
                                               int priority);
/* 0 for a stale/invalid voice, which is ALSO what a live normal-priority
 * voice reads -- this getter cannot tell you which one you have, and there is
 * no public voice-validity predicate to ask.  It answers "how important is
 * this voice", not "does this voice exist". */
JCE_API int       jce_audio_voice_get_priority(const JceAudio *audio,
                                               JceVoice voice);

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
JCE_API JceVoice  jce_audio_play_stream(JceAudio *audio,
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

/* -- Aux sends (device side) ---------------------------------------- *
 *
 * Up to JCE_AUDIO_BUS_MAX_SENDS per bus, which is deliberately the same 8 that
 * jce_audio_mixer.h's JCE_AUDIO_MAX_SENDS allows: a device that routed fewer
 * than the mixer can author would be the same partial wiring this whole
 * facility exists to remove.  Declared here rather than included from the
 * mixer header because that header promises to stay engine-agnostic, and the
 * dependency would only run the wrong way to share a number.
 *
 * A console aux send is a PARALLEL tap: the bus keeps feeding its normal
 * output and a scaled copy also goes to another bus (a reverb or delay
 * return).  jce_audio_mixer has modelled this for a long time -- set_send,
 * send_at, resolve_send -- the editor authors it per bus and the config parser
 * reads it back at Play start, and NOTHING ROUTED ANY AUDIO: the device side
 * offered only bus_create / bus_set_volume / voice_set_bus, so every bus was a
 * flat child of Master with one gain and no parallel tap to send into.
 *
 * `amount` is linear, 0 = off.  Calling it again for the SAME destination
 * re-scales that send; a different destination ADDS one, the way a console
 * strip feeds several returns at once -- it does not re-point the first.
 * Setting 0 retires the send and frees its slot.  A bus may not send to
 * ITSELF -- that is a ring in the node
 * graph and pulling frames through it recurses with no bottom.  Longer rings
 * are refused one layer up by jce_audio_mixer_set_send.
 *
 * Returns false if either bus is unknown, the send is a self-send, or the
 * graph refused the attachment; the bus is left exactly as it was. */
#define JCE_AUDIO_BUS_MAX_SENDS 8

JCE_API bool  jce_audio_bus_set_send(JceAudio *audio, const char *from_bus,
                                     const char *to_bus, float amount);
/* 0 when there is no send from `from_bus` to that specific bus. */
JCE_API float jce_audio_bus_get_send(const JceAudio *audio,
                                     const char *from_bus, const char *to_bus);

/* Route a voice's output into the named bus.  Call after jce_audio_play.
 * Unknown bus name or "Master" routes the voice straight to the endpoint.
 * No-op on a stale/invalid voice. */
JCE_API void      jce_audio_voice_set_bus(JceAudio *audio, JceVoice voice,
                                          const char *bus_name);

/* Peak level of the signal a named bus put out since the LAST call to this
 * function, linear and POST-FADER: the bus's own gain (as last pushed by
 * jce_audio_bus_set_volume) is already in it, exactly as a DAW channel meter
 * reads.  Normally 0..1, but nothing clamps a hot mix, so treat >1 as real.
 *
 * Read-and-clear: each call returns the maximum |sample| seen since the
 * previous call and resets the accumulator, so a caller polling once per
 * frame sees that frame's peak and never a stale hold.  Calling it from two
 * places splits the measurement between them -- there is one meter per bus,
 * not one per reader.
 *
 * Metering is enabled lazily by the first call, so the first call after
 * creating a bus returns 0 (nothing was being measured yet) and subsequent
 * calls return real levels.  Returns 0 for an unknown bus, for "Master"
 * (which is the engine endpoint, not a bus node), and when audio is
 * disabled.
 *
 * Thread-safe: the audio thread writes the accumulator and any thread may
 * read-and-clear it.  This is what feeds jce_audio_mixer_duck_advance()'s
 * key-peak callback -- see jce_audio_mixer.h. */
JCE_API float     jce_audio_bus_get_peak(JceAudio *audio, const char *name);

/* Render the engine's node graph OFFLINE into `out`, without a device.
 *
 * Every JceAudio is device-less (see the master mix above); normally the one
 * shared output device pumps it.  This pumps it directly instead, running
 * every live node -- voices, bus groups, insert chains, the reverb tail --
 * exactly as the device callback would.
 *
 * On success it writes EXACTLY `frames` frames of interleaved f32 at the
 * engine's channel count (2), silence-padded when the graph produces less --
 * an idle engine, or one with nothing attached to its endpoint at all, which
 * reads zero frames because miniaudio has no silence to mix.  That is the
 * same gapless property the shared device gives (its callback runs at a
 * constant cadence and sums to silence when engines are idle), and it is why
 * there is no frames-written out-parameter: a caller bouncing to a file or
 * feeding an encoder needs the block, not a short read it has to pad itself.
 *
 * That makes the audio path testable and usable with no sound hardware:
 * headless/dedicated-server builds, offline bounce, and unit tests that must
 * observe the real graph rather than skipping for want of a device.
 *
 * `audio` MUST come from jce_audio_create_offline(); this returns false for a
 * device-pumped engine rather than racing the device thread for the same read
 * cursors.  That is a refusal, not a warning in a comment, because a comment
 * is the one thing a caller can be sure of not reading.
 *
 * Returns false -- writing NOTHING -- on a NULL/invalid argument, on a
 * device-pumped engine, or when audio is disabled. */
JCE_API bool      jce_audio_render_offline(JceAudio *audio, float *out,
                                           uint32_t frames);

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
 * damping/lowpass_hz roll off the wet high frequencies.
 *
 * THIS COMMENT USED TO SAY "room_size (m) scales the pre-delay" and that
 * "diffusion/density are accepted for completeness".  Neither was true:
 * jce_audio_set_reverb read five of the nine fields and there was no
 * pre-delay in the DSP at all.  Now: pre_delay_ms delays the late tail (and
 * room_size derives one when pre_delay_ms is 0, at ~0.34 m per ms, which is
 * what the sentence had always claimed); diffusion drives the allpass
 * feedback, which is what diffusion IS.  density remains unread and is the
 * one field still described as accepted -- it would mean a variable comb
 * count, and a fixed network cannot express it.
 *
 * early_mix / early_delay_ms are the FIRST reflection: one clean tap before
 * the diffuse tail, which is what tells a listener the size of a room.
 * APPENDED. */
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
    float early_mix;        /* 0 = no early reflection */
    float early_delay_ms;   /* time to the first reflection */
} JceAudioReverbParams;

/* Apply the blended reverb preset to the global reverb node (created lazily
 * on first call).  Bus output is routed through the reverb node so the wet
 * tail is audible.  Passing wet_mix <= 0 leaves the dry signal untouched.
 * No-op safely when audio is disabled. */
JCE_API void      jce_audio_set_reverb(JceAudio *audio,
                                       const JceAudioReverbParams *params);

/* Run the SAME reverb DSP over a known buffer, with no audio device.
 *
 * The identical entry point the DSP chain has, and for the identical reason:
 * "the same math runs offline over a known PCM buffer, which is how the
 * effects are unit-tested with no audio device".  The reverb had no such
 * door, so nothing about it could be asserted -- which is how six authored
 * numbers (room_size, diffusion, density, pre_delay_ms, and the component's
 * reflections pair) came to reach no part of the DSP without anything
 * noticing.
 *
 * `frames` frames of `channels` interleaved f32 in `io`, processed in place.
 * A fresh reverb state per call, so an impulse response starts from silence
 * and two calls with the same input give the same output.  channels is
 * clamped to 1..2; returns false if the state could not be allocated. */
JCE_API bool      jce_audio_reverb_process_offline(
                      const JceAudioReverbParams *params,
                      float *io, uint32_t frames, int channels,
                      int sample_rate);

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
/* Continuous 2D<->3D blend, Unity's AudioSource.spatialBlend.
 *
 * jce_audio_voice_set_3d below is a BOOLEAN, and for a long time it was the
 * only consumer of a float the inspector edits in 0.01 steps: the runtime read
 * `spatial_blend > 0.5f`, so 0.0 and 0.49 were bit-identical in the mix and so
 * were 0.51 and 1.0.  A hundred authorable values, two reachable states.
 *
 * WHAT IS BLENDED, AND WHAT IS NOT.  miniaudio has no spatial-blend knob, so
 * this scales the DISTANCE ATTENUATION by putting a floor under it: at 0.25
 * the attenuation can only pull the voice a quarter of the way toward silence.
 * That is the audible axis and it is now continuous.  PANNING IS NOT BLENDED
 * -- once spatialisation is on, miniaudio pans from the 3D direction, and
 * blending that needs a manual pan this engine does not drive.  Unity blends
 * both; saying so here rather than letting the difference be discovered.
 *
 * blend <= 0 turns spatialisation off entirely, which is what 0 already did. */
JCE_API void jce_audio_voice_set_spatial_blend(JceAudio *audio, JceVoice voice,
                                               float blend);

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
