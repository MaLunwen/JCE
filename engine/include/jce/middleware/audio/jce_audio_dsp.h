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
    JCE_AUDIO_EFFECT_DELAY      = 4,  /* feedback delay line        */
    /* APPENDED.  These values are written into audio_mixer.json, so they are
     * wire values: never reordered, never reused. */
    JCE_AUDIO_EFFECT_CHORUS     = 5,  /* modulated delay, thickens  */
    JCE_AUDIO_EFFECT_FLANGER    = 6,  /* short modulated delay + fb */
    JCE_AUDIO_EFFECT_DISTORTION = 7   /* waveshaper                 */
} JceAudioEffectType;

/* Where a REGISTERED effect's type id starts.  Chosen far above the built-in
 * range so a project's id can never collide with a built-in added later --
 * the alternative, handing out the next free small integer, would make every
 * new built-in a silent renumbering of somebody's saved mixer. */
#define JCE_AUDIO_EFFECT_CUSTOM_BASE 256

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

/* CHORUS and FLANGER are the same machine -- a delay line whose read head is
 * swept by an LFO -- and share these parameters rather than getting one
 * struct each.  What separates them is where the numbers live: a chorus sits
 * around 20-30 ms with little or no feedback and sounds like more than one
 * player; a flanger sits around 1-5 ms with heavy feedback and sounds like a
 * jet.  Two structs with identical fields would have invited them to drift.
 *
 * `depth_ms` is swept EITHER SIDE of delay_ms, and the effect clamps the read
 * head to the line rather than letting a deep sweep wrap past the write head
 * -- which is not a subtle artefact, it is a click every LFO cycle. */
typedef struct {
    float delay_ms;       /* centre delay (chorus ~20, flanger ~2)     */
    float depth_ms;       /* sweep either side of it                   */
    float rate_hz;        /* LFO speed                                 */
    float feedback;       /* 0..<1 (chorus ~0, flanger high)           */
    float wet;            /* 0..1 wet level                            */
    float dry;            /* 0..1 dry level                            */
    float stereo_phase;   /* 0..1: LFO phase offset of the RIGHT
                           * channel, in cycles.  0.25-0.5 is what makes
                           * a chorus wide; 0 makes it mono-ish.        */
} JceAudioModDelayParams;

/* Shapes a waveshaper can take.  Wire values. */
typedef enum {
    JCE_AUDIO_DIST_SOFT_CLIP = 0,  /* tanh-ish: warm, no hard corners  */
    JCE_AUDIO_DIST_HARD_CLIP = 1,  /* flat top: fuzzy, lots of odd hs  */
    JCE_AUDIO_DIST_FOLDBACK  = 2   /* folds past the ceiling: ring-y   */
} JceAudioDistortionShape;

typedef struct {
    JceAudioDistortionShape shape;
    float drive;          /* >=1 pre-gain into the shaper              */
    float ceiling;        /* clip/fold threshold, typically 1.0        */
    float wet;            /* 0..1 shaped level                         */
    float dry;            /* 0..1 clean level                          */
    float output_gain;    /* post gain; drive without it just gets loud */
} JceAudioDistortionParams;

/* Tagged union describing one insert effect's configuration. */
typedef struct {
    JceAudioEffectType type;
    union {
        JceAudioEqParams         eq;
        JceAudioCompressorParams comp;
        JceAudioLimiterParams    limiter;
        JceAudioDelayParams      delay;
        JceAudioModDelayParams   mod_delay;   /* CHORUS and FLANGER */
        JceAudioDistortionParams distortion;
    } u;
    /* REGISTERED effects only (type >= JCE_AUDIO_EFFECT_CUSTOM_BASE): the
     * parameter blob handed to the vtable's configure().
     *
     * APPENDED AFTER THE UNION, not added to it.  A new union member changes
     * what the FIRST bytes of this struct mean for a caller compiled against
     * the old one; a field after it does not.
     *
     * The pointer need only be valid FOR THE DURATION of the add/set call --
     * configure() copies whatever it needs into the effect's own state.  It
     * is not retained, so a caller may pass a stack local. */
    const void *custom_params;
    uint32_t    custom_params_size;
} JceAudioEffectDesc;

/* -- Registering an effect the engine does not ship ------------------
 *
 * This is the axis the parity row actually names: "Unity's is a plugin
 * surface; this is a closed set."  A palette is always short of somebody's
 * fifteen; a set a project can extend is not waiting on this engine.
 *
 * The chain owns one `state_size` allocation per instance and passes it to
 * every callback, so an effect keeps its filter/envelope/delay history the
 * same way a built-in does.  `process` runs ON THE AUDIO THREAD: it must not
 * allocate, lock, or block, exactly as the built-ins do not.
 *
 * `name` is what audio_mixer.json spells, so a registered effect is
 * authorable rather than script-only.  Registration is process-global and
 * idempotent by name: registering the same name twice returns the SAME id
 * rather than a second one, because two ids for one name would make a saved
 * mixer depend on registration order.
 */
typedef struct JceAudioEffectVTable {
    const char *name;         /* authored spelling; must outlive the process */
    uint32_t    state_size;   /* bytes of per-instance state (may be 0)      */
    /* Configure or reconfigure one instance.  `params` is the desc's
     * custom_params blob and is NOT retained.  Return false to refuse. */
    bool (*configure)(void *state, uint32_t channels, uint32_t sample_rate,
                      const void *params, uint32_t params_size);
    /* Return the instance to silence without changing its configuration.
     * May be NULL: the chain then zeroes the state block, which is correct
     * for any effect whose history is plain floats. */
    void (*reset)(void *state);
    /* Interleaved, in place, on the audio thread. */
    void (*process)(void *state, float *buffer, uint32_t frames,
                    uint32_t channels);
    /* Release anything configure() allocated.  May be NULL. */
    void (*release)(void *state);
} JceAudioEffectVTable;

/* Register `vt` and return its type id (>= JCE_AUDIO_EFFECT_CUSTOM_BASE), or
 * -1 when vt / vt->name / vt->process is NULL or the table is full.  Returns
 * the EXISTING id when the name is already registered. */
JCE_API int JCE_CALL jce_audio_dsp_register_effect(const JceAudioEffectVTable *vt);

/* Type id previously registered under `name`, or -1.  This is how a mixer
 * config turns an authored string into a type. */
JCE_API int JCE_CALL jce_audio_dsp_find_effect(const char *name);

/* Authored spelling of a type id, or NULL.  Answers for BUILT-INS too
 * ("eq", "compressor", "limiter", "delay", "chorus", "flanger",
 * "distortion"), so a serialiser has ONE place to ask rather than a switch
 * that drifts from the parser's. */
JCE_API const char *JCE_CALL jce_audio_dsp_effect_name(int type);

/* Type id for an authored spelling, built-in or registered, or -1.  The
 * inverse of the above and the other half of that single source of truth. */
JCE_API int JCE_CALL jce_audio_dsp_effect_type_from_name(const char *name);

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
