/*
 * jce_audio_dsp.h  Insert-effect DSP chain (device-independent).
 *
 * FEATURE 5.1 — a chain of ordered DSP "insert" effects that can be attached
 * to a mixer bus or a single voice (see jce_audio_bus_add_effect /
 * jce_audio_voice_add_effect in jce_audio.h).
 *
 * The DSP math here is completely independent of miniaudio and of any audio
 * device: a JceAudioDspChain processes a plain interleaved float buffer in
 * place via jce_audio_dsp_chain_process().  jce_audio.c wraps a chain in a
 * custom ma_node so the same code path runs live in the node graph, but the
 * chain can equally be driven offline over a known PCM buffer (a unit impulse
 * or a sine) — which is exactly how it is unit-tested.
 *
 * Provided effects:
 *   - Parametric EQ      (peaking / low-shelf / high-shelf / low-pass /
 *                          high-pass biquad, RBJ cookbook coefficients)
 *   - Dynamics Compressor (threshold / ratio / attack / release / makeup,
 *                          peak-detecting envelope follower)
 *   - Brickwall Limiter  (instantaneous gain reduction to a ceiling)
 *   - Delay line         (delay time + feedback + wet/dry mix)
 *
 * All gains/levels in dB are decibels; linear amplitudes are 0..1 (or higher).
 * Buffers are 32-bit float, interleaved by channel.  A chain is created for a
 * fixed sample rate and channel count.
 */

#ifndef JCE_AUDIO_DSP_H
#define JCE_AUDIO_DSP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Maximum number of insert effects in a single chain. */
#define JCE_AUDIO_DSP_MAX_EFFECTS 8
/* Maximum channels a chain processes (mono/stereo). */
#define JCE_AUDIO_DSP_MAX_CHANNELS 2

/* -- Effect kinds --------------------------------------------------- */

typedef enum {
    JCE_AUDIO_EFFECT_NONE       = 0,
    JCE_AUDIO_EFFECT_EQ         = 1,  /* parametric biquad EQ band  */
    JCE_AUDIO_EFFECT_COMPRESSOR = 2,  /* dynamics compressor        */
    JCE_AUDIO_EFFECT_LIMITER    = 3,  /* brickwall limiter          */
    JCE_AUDIO_EFFECT_DELAY      = 4   /* feedback delay line        */
} JceAudioEffectType;

/* Filter shapes for the parametric EQ band. */
typedef enum {
    JCE_AUDIO_EQ_PEAKING    = 0,  /* boost/cut around frequency (uses gain_db, q) */
    JCE_AUDIO_EQ_LOW_SHELF  = 1,  /* shelf below frequency       (uses gain_db, q) */
    JCE_AUDIO_EQ_HIGH_SHELF = 2,  /* shelf above frequency       (uses gain_db, q) */
    JCE_AUDIO_EQ_LOW_PASS   = 3,  /* roll off highs              (uses q)          */
    JCE_AUDIO_EQ_HIGH_PASS  = 4   /* roll off lows               (uses q)          */
} JceAudioEqShape;

/* -- Effect parameter structs --------------------------------------- */

typedef struct {
    JceAudioEqShape shape;
    float           frequency_hz;  /* center / corner frequency        */
    float           gain_db;       /* peaking/shelf gain (ignored LP/HP)*/
    float           q;             /* bandwidth / resonance (e.g. 0.707)*/
} JceAudioEqParams;

typedef struct {
    float threshold_db;   /* level above which gain reduction starts   */
    float ratio;          /* >1: e.g. 4.0 => 4:1                       */
    float attack_ms;      /* envelope attack time                      */
    float release_ms;     /* envelope release time                     */
    float makeup_db;      /* post-compression makeup gain              */
    float knee_db;        /* soft-knee width (0 = hard knee)           */
} JceAudioCompressorParams;

typedef struct {
    float ceiling_db;     /* output never exceeds this (e.g. -0.1 dB)  */
    float release_ms;     /* gain-reduction release time               */
} JceAudioLimiterParams;

typedef struct {
    float delay_ms;       /* delay time                                */
    float feedback;       /* 0..<1, amount fed back into the line      */
    float wet;            /* 0..1 wet (delayed) level                  */
    float dry;            /* 0..1 dry (input) level                    */
} JceAudioDelayParams;

/* Tagged union describing one insert effect's configuration. */
typedef struct {
    JceAudioEffectType type;
    union {
        JceAudioEqParams         eq;
        JceAudioCompressorParams comp;
        JceAudioLimiterParams    limiter;
        JceAudioDelayParams      delay;
    } u;
} JceAudioEffectDesc;

/* -- Chain (opaque) ------------------------------------------------- */

typedef struct JceAudioDspChain JceAudioDspChain;

/* Create an empty insert chain for `channels` (1 or 2) at `sample_rate`.
 * Returns NULL on bad args or OOM. */
JCE_API JceAudioDspChain *JCE_CALL jce_audio_dsp_chain_create(uint32_t channels,
                                                              uint32_t sample_rate);

/* Destroy a chain and all its effects.  NULL-safe. */
JCE_API void JCE_CALL jce_audio_dsp_chain_destroy(JceAudioDspChain *chain);

/* Number of effects currently in the chain. */
JCE_API uint32_t JCE_CALL jce_audio_dsp_chain_count(const JceAudioDspChain *chain);

/* Append an effect described by `desc` to the end of the chain.  Returns the
 * new effect's index (>=0), or -1 on failure (chain full / bad args). */
JCE_API int JCE_CALL jce_audio_dsp_chain_add(JceAudioDspChain *chain,
                                             const JceAudioEffectDesc *desc);

/* Reconfigure the effect at `index` in place (keeps its delay/envelope state
 * where compatible).  Returns true on success. */
JCE_API bool JCE_CALL jce_audio_dsp_chain_set(JceAudioDspChain *chain,
                                              uint32_t index,
                                              const JceAudioEffectDesc *desc);

/* Remove the effect at `index`, shifting later effects down.  Returns true if
 * an effect was removed. */
JCE_API bool JCE_CALL jce_audio_dsp_chain_remove(JceAudioDspChain *chain,
                                                 uint32_t index);

/* Remove every effect (the chain becomes a pass-through). */
JCE_API void JCE_CALL jce_audio_dsp_chain_clear(JceAudioDspChain *chain);

/* Read back the effect type at `index` (JCE_AUDIO_EFFECT_NONE if invalid). */
JCE_API JceAudioEffectType JCE_CALL jce_audio_dsp_chain_type(
    const JceAudioDspChain *chain, uint32_t index);

/* Reset all internal state (filter/envelope/delay history) to silence without
 * changing the effect list — e.g. when a voice rewinds. */
JCE_API void JCE_CALL jce_audio_dsp_chain_reset(JceAudioDspChain *chain);

/* Process `frame_count` interleaved frames of `channels` in place.  An empty
 * chain leaves the buffer untouched.  This is the single DSP entry point used
 * both by the live ma_node and by offline unit tests. */
JCE_API void JCE_CALL jce_audio_dsp_chain_process(JceAudioDspChain *chain,
                                                  float *buffer,
                                                  uint32_t frame_count);

/* -- Stand-alone single-effect helpers (handy for testing) ---------- *
 *
 * Each creates a one-effect chain configured from a default-filled desc with
 * the given primary parameters.  They are thin wrappers over the chain API so
 * a test can spin up an isolated effect in one call. */

/* Sensible default desc for an effect type (fills the active union member). */
JCE_API JceAudioEffectDesc JCE_CALL jce_audio_effect_default(JceAudioEffectType type);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_DSP_H */
