/*
 * jce_audio_dsp.h  Tiny DSP building blocks for the mixer.
 *
 * Provides Biquad lowpass / highpass / bandpass / notch filters and a
 * sidechain ducking processor.  Pure CPU, integer-cost per sample.
 *
 * Usage pattern: caller owns the JceBiquad / JceDucker structs and
 * calls jce_biquad_process_block() once per audio buffer (typically
 * inside the audio thread or callback).  The mixer itself doesn't
 * automatically wire DSP — engine consumers route their voices /
 * busses through these as needed.
 *
 * Filter design uses Robert Bristow-Johnson's audio EQ cookbook
 * formulae (the de-facto industry standard).
 *
 * Layer: middleware (Layer 4) — public.
 */

#ifndef JCE_AUDIO_DSP_H
#define JCE_AUDIO_DSP_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── Biquad filter ─────────────────────────────────────────────────── */

typedef enum {
    JCE_BIQUAD_LOWPASS  = 0,
    JCE_BIQUAD_HIGHPASS = 1,
    JCE_BIQUAD_BANDPASS = 2,
    JCE_BIQUAD_NOTCH    = 3,
} JceBiquadKind;

/* Direct-Form-II Transposed coefficients + one channel of state. */
typedef struct {
    float b0, b1, b2;   /* feed-forward */
    float a1, a2;       /* feedback (a0 normalised to 1) */
    float z1, z2;       /* state */
} JceBiquad;

/* Reset filter state to silence (z1 = z2 = 0).  Coefficients unchanged. */
JCE_API void jce_biquad_reset(JceBiquad *bq);

/* Configure a biquad.  q ~ 0.707 = Butterworth (no resonant peak).
 * Higher q produces a peak; lower q produces a softer roll-off. */
JCE_API void jce_biquad_configure(JceBiquad *bq, JceBiquadKind kind,
                                  float sample_rate_hz,
                                  float cutoff_hz,
                                  float q);

/* Process a mono buffer in-place (block of `count` samples). */
JCE_API void jce_biquad_process_mono(JceBiquad *bq,
                                     float *samples, uint32_t count);

/* Process a stereo interleaved buffer in-place using two filter
 * instances (one per channel) sharing identical coefficients but
 * independent state. */
JCE_API void jce_biquad_process_stereo(JceBiquad *bq_left,
                                       JceBiquad *bq_right,
                                       float *interleaved,
                                       uint32_t frame_count);

/* ── Sidechain Ducker ──────────────────────────────────────────────── */

/* Reduce the gain of a "background" bus when a "trigger" bus is loud.
 * Common case: dim music when dialogue plays.  This is a one-pole
 * envelope follower on the trigger's RMS, mapped to a linear gain
 * applied to the duckee.
 *
 * threshold_db   below this: full pass-through (gain = 1).
 * ratio          gain reduction per dB above threshold (e.g., 4 → 4:1).
 * attack_ms      how fast envelope rises to peak.
 * release_ms     how fast envelope decays back.
 *
 * Typical music-under-VO settings: threshold = -30 dB, ratio = 6:1,
 * attack = 5 ms, release = 200 ms. */
typedef struct {
    float threshold_lin;
    float ratio;
    float attack_coef;        /* 0..1 IIR coefficient */
    float release_coef;
    float envelope;           /* current follower output */
    float current_gain;       /* applied to duckee */
} JceDucker;

JCE_API void jce_ducker_configure(JceDucker *d,
                                  float sample_rate_hz,
                                  float threshold_db,
                                  float ratio,
                                  float attack_ms,
                                  float release_ms);

/* Process one block.  trigger_samples is mono (the loudness reference);
 * duckee is mono and modified in-place.  Both buffers are `count`
 * samples long. */
JCE_API void jce_ducker_process(JceDucker *d,
                                const float *trigger_samples,
                                float *duckee_samples,
                                uint32_t count);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_DSP_H */
