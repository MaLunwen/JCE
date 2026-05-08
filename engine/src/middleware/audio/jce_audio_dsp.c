/*
 * jce_audio_dsp.c  Biquad + Ducker implementation.
 *
 * Filter design follows RBJ audio EQ cookbook (well-known closed-form
 * formulas; cf. https://www.w3.org/TR/audio-eq-cookbook/).  The
 * cookbook gives normalised b0/b1/b2 + a0/a1/a2; we divide by a0 and
 * negate a1/a2 for direct-form-II transposed.
 */

#include <jce/middleware/audio/jce_audio_dsp.h>

#include <math.h>
#include <string.h>

#ifndef JCE_PI
#define JCE_PI 3.14159265358979323846f
#endif

void jce_biquad_reset(JceBiquad *bq)
{
    if (!bq) return;
    bq->z1 = bq->z2 = 0.0f;
}

void jce_biquad_configure(JceBiquad *bq, JceBiquadKind kind,
                          float sample_rate_hz,
                          float cutoff_hz,
                          float q)
{
    if (!bq) return;
    if (sample_rate_hz <= 0.0f) sample_rate_hz = 48000.0f;
    if (cutoff_hz < 1.0f)       cutoff_hz = 1.0f;
    /* Nyquist clamp. */
    float ny = sample_rate_hz * 0.499f;
    if (cutoff_hz > ny) cutoff_hz = ny;
    if (q < 0.001f)     q = 0.001f;

    float omega = 2.0f * JCE_PI * cutoff_hz / sample_rate_hz;
    float cs    = cosf(omega);
    float sn    = sinf(omega);
    float alpha = sn / (2.0f * q);

    float b0, b1, b2, a0, a1, a2;
    switch (kind) {
        case JCE_BIQUAD_LOWPASS:
            b0 = (1.0f - cs) * 0.5f;
            b1 = 1.0f - cs;
            b2 = (1.0f - cs) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha;
            break;
        case JCE_BIQUAD_HIGHPASS:
            b0 = (1.0f + cs) * 0.5f;
            b1 = -(1.0f + cs);
            b2 = (1.0f + cs) * 0.5f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha;
            break;
        case JCE_BIQUAD_BANDPASS:
            b0 = alpha;
            b1 = 0.0f;
            b2 = -alpha;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha;
            break;
        case JCE_BIQUAD_NOTCH:
        default:
            b0 = 1.0f;
            b1 = -2.0f * cs;
            b2 = 1.0f;
            a0 = 1.0f + alpha;
            a1 = -2.0f * cs;
            a2 = 1.0f - alpha;
            break;
    }

    /* Normalise so a0 = 1. */
    float inv_a0 = 1.0f / a0;
    bq->b0 = b0 * inv_a0;
    bq->b1 = b1 * inv_a0;
    bq->b2 = b2 * inv_a0;
    bq->a1 = a1 * inv_a0;
    bq->a2 = a2 * inv_a0;
    /* State preserved across reconfigure to avoid clicks on cutoff sweeps. */
}

void jce_biquad_process_mono(JceBiquad *bq, float *samples, uint32_t count)
{
    if (!bq || !samples) return;
    /* DF-II transposed: y[n] = b0*x[n] + z1
     *                   z1   = b1*x[n] - a1*y[n] + z2
     *                   z2   = b2*x[n] - a2*y[n] */
    float z1 = bq->z1;
    float z2 = bq->z2;
    for (uint32_t i = 0; i < count; ++i) {
        float x = samples[i];
        float y = bq->b0 * x + z1;
        z1 = bq->b1 * x - bq->a1 * y + z2;
        z2 = bq->b2 * x - bq->a2 * y;
        samples[i] = y;
    }
    bq->z1 = z1;
    bq->z2 = z2;
}

void jce_biquad_process_stereo(JceBiquad *bq_l, JceBiquad *bq_r,
                               float *interleaved, uint32_t frames)
{
    if (!bq_l || !bq_r || !interleaved) return;
    float l1 = bq_l->z1, l2 = bq_l->z2;
    float r1 = bq_r->z1, r2 = bq_r->z2;
    for (uint32_t i = 0; i < frames; ++i) {
        float xl = interleaved[2*i+0];
        float xr = interleaved[2*i+1];
        float yl = bq_l->b0 * xl + l1;
        l1 = bq_l->b1 * xl - bq_l->a1 * yl + l2;
        l2 = bq_l->b2 * xl - bq_l->a2 * yl;
        float yr = bq_r->b0 * xr + r1;
        r1 = bq_r->b1 * xr - bq_r->a1 * yr + r2;
        r2 = bq_r->b2 * xr - bq_r->a2 * yr;
        interleaved[2*i+0] = yl;
        interleaved[2*i+1] = yr;
    }
    bq_l->z1 = l1; bq_l->z2 = l2;
    bq_r->z1 = r1; bq_r->z2 = r2;
}

/* ── Ducker ────────────────────────────────────────────────────────── */

static float ms_to_coef(float ms, float sr_hz)
{
    /* One-pole IIR coefficient for a given time-constant.  exp(-1/(tc*sr)). */
    if (ms < 0.01f) ms = 0.01f;
    float tc_samples = (ms * 0.001f) * sr_hz;
    return expf(-1.0f / tc_samples);
}

static float db_to_lin(float db) { return powf(10.0f, db * 0.05f); }

void jce_ducker_configure(JceDucker *d, float sr_hz,
                          float threshold_db, float ratio,
                          float attack_ms, float release_ms)
{
    if (!d) return;
    if (sr_hz <= 0.0f) sr_hz = 48000.0f;
    if (ratio < 1.0f)  ratio = 1.0f;
    d->threshold_lin = db_to_lin(threshold_db);
    d->ratio         = ratio;
    d->attack_coef   = ms_to_coef(attack_ms,  sr_hz);
    d->release_coef  = ms_to_coef(release_ms, sr_hz);
    d->envelope      = 0.0f;
    d->current_gain  = 1.0f;
}

void jce_ducker_process(JceDucker *d,
                        const float *trigger, float *duckee,
                        uint32_t count)
{
    if (!d || !trigger || !duckee) return;
    float env = d->envelope;
    float gain = d->current_gain;
    for (uint32_t i = 0; i < count; ++i) {
        float t = trigger[i];
        float amp = t < 0.0f ? -t : t;
        /* Attack rises (amp > env), release falls (amp < env). */
        float coef = (amp > env) ? d->attack_coef : d->release_coef;
        env = coef * env + (1.0f - coef) * amp;

        /* Compute target gain: 1.0 below threshold, attenuate by
         * 1/ratio dB per dB above threshold. */
        float target = 1.0f;
        if (env > d->threshold_lin && d->threshold_lin > 0.0f) {
            float over_db = 20.0f * log10f(env / d->threshold_lin);
            float reduce_db = over_db * (1.0f - 1.0f / d->ratio);
            target = db_to_lin(-reduce_db);
        }
        /* Smooth gain too so transitions don't click. */
        gain = 0.99f * gain + 0.01f * target;
        duckee[i] *= gain;
    }
    d->envelope     = env;
    d->current_gain = gain;
}
