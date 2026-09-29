/*
 * jce_audio_dsp.c  Insert-effect DSP chain implementation.
 *
 * Device-independent DSP: no miniaudio, no SDL, just float math over an
 * interleaved buffer.  jce_audio.c wraps a chain in a custom ma_node, but the
 * chain runs identically offline (which is how it is unit-tested).
 *
 * DSP references:
 *   - EQ:         Robert Bristow-Johnson "Audio EQ Cookbook" biquads.
 *   - Compressor: classic feed-forward peak detector + log-domain knee.
 *   - Limiter:    per-sample brickwall gain reduction toward a ceiling.
 *   - Delay:      single-tap feedback delay line with wet/dry mix.
 */

#include <jce/middleware/audio/jce_audio_dsp.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#ifndef JCE_DSP_PI
#define JCE_DSP_PI 3.14159265358979323846
#endif

/* ── dB <-> linear ─────────────────────────────────────────────────── */

static inline float db_to_lin(float db)
{
    return (float)pow(10.0, (double)db / 20.0);
}

static inline float lin_to_db(float lin)
{
    if (lin < 1.0e-9f) lin = 1.0e-9f;
    return (float)(20.0 * log10((double)lin));
}

/* ── Biquad (direct-form I, per channel) ───────────────────────────── */

typedef struct {
    /* Normalised coefficients (a0 folded in). */
    float b0, b1, b2, a1, a2;
    /* Per-channel state. */
    float x1[JCE_AUDIO_DSP_MAX_CHANNELS];
    float x2[JCE_AUDIO_DSP_MAX_CHANNELS];
    float y1[JCE_AUDIO_DSP_MAX_CHANNELS];
    float y2[JCE_AUDIO_DSP_MAX_CHANNELS];
} Biquad;

static void biquad_reset(Biquad *bq)
{
    for (int c = 0; c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c) {
        bq->x1[c] = bq->x2[c] = bq->y1[c] = bq->y2[c] = 0.0f;
    }
}

/* Compute RBJ cookbook coefficients for the EQ band. */
static void biquad_design(Biquad *bq, const JceAudioEqParams *p, uint32_t sr)
{
    double fs = sr > 0 ? (double)sr : 44100.0;
    double f0 = (double)p->frequency_hz;
    if (f0 < 1.0)        f0 = 1.0;
    if (f0 > fs * 0.49)  f0 = fs * 0.49;   /* keep below Nyquist */
    double q = (double)p->q;
    if (q < 1.0e-4) q = 1.0e-4;

    double w0    = 2.0 * JCE_DSP_PI * f0 / fs;
    double cosw0 = cos(w0);
    double sinw0 = sin(w0);
    double alpha = sinw0 / (2.0 * q);
    double A     = pow(10.0, (double)p->gain_db / 40.0); /* sqrt-amplitude */

    double b0, b1, b2, a0, a1, a2;

    switch (p->shape) {
    case JCE_AUDIO_EQ_PEAKING:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cosw0;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cosw0;
        a2 = 1.0 - alpha / A;
        break;
    case JCE_AUDIO_EQ_LOW_SHELF: {
        double tsa = 2.0 * sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + tsa);
        b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - tsa);
        a0 =          (A + 1.0) + (A - 1.0) * cosw0 + tsa;
        a1 = -2.0 *   ((A - 1.0) + (A + 1.0) * cosw0);
        a2 =          (A + 1.0) + (A - 1.0) * cosw0 - tsa;
        break;
    }
    case JCE_AUDIO_EQ_HIGH_SHELF: {
        double tsa = 2.0 * sqrt(A) * alpha;
        b0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + tsa);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
        b2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - tsa);
        a0 =          (A + 1.0) - (A - 1.0) * cosw0 + tsa;
        a1 =  2.0 *   ((A - 1.0) - (A + 1.0) * cosw0);
        a2 =          (A + 1.0) - (A - 1.0) * cosw0 - tsa;
        break;
    }
    case JCE_AUDIO_EQ_LOW_PASS:
        b0 = (1.0 - cosw0) / 2.0;
        b1 =  1.0 - cosw0;
        b2 = (1.0 - cosw0) / 2.0;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cosw0;
        a2 =  1.0 - alpha;
        break;
    case JCE_AUDIO_EQ_HIGH_PASS:
    default:
        b0 = (1.0 + cosw0) / 2.0;
        b1 = -(1.0 + cosw0);
        b2 = (1.0 + cosw0) / 2.0;
        a0 =  1.0 + alpha;
        a1 = -2.0 * cosw0;
        a2 =  1.0 - alpha;
        break;
    }

    if (a0 == 0.0) a0 = 1.0;
    bq->b0 = (float)(b0 / a0);
    bq->b1 = (float)(b1 / a0);
    bq->b2 = (float)(b2 / a0);
    bq->a1 = (float)(a1 / a0);
    bq->a2 = (float)(a2 / a0);
}

static inline float biquad_tick(Biquad *bq, int c, float x)
{
    float y = bq->b0 * x + bq->b1 * bq->x1[c] + bq->b2 * bq->x2[c]
            - bq->a1 * bq->y1[c] - bq->a2 * bq->y2[c];
    bq->x2[c] = bq->x1[c];
    bq->x1[c] = x;
    bq->y2[c] = bq->y1[c];
    bq->y1[c] = y;
    return y;
}

/* ── Compressor (peak-detect feed-forward) ─────────────────────────── */

typedef struct {
    JceAudioCompressorParams p;
    float attack_coef;   /* per-sample smoothing for level rise          */
    float release_coef;  /* per-sample smoothing for level fall          */
    float env;           /* tracked input envelope (linear)              */
    float makeup_lin;
} CompState;

static void comp_design(CompState *s, uint32_t sr)
{
    double fs = sr > 0 ? (double)sr : 44100.0;
    double atk = (double)s->p.attack_ms;
    double rel = (double)s->p.release_ms;
    if (atk < 0.0) atk = 0.0;
    if (rel < 0.0) rel = 0.0;
    /* coef = exp(-1 / (time_seconds * fs)); 0 ms => instantaneous. */
    s->attack_coef  = atk <= 0.0 ? 0.0f
                    : (float)exp(-1.0 / ((atk / 1000.0) * fs));
    s->release_coef = rel <= 0.0 ? 0.0f
                    : (float)exp(-1.0 / ((rel / 1000.0) * fs));
    if (s->p.ratio < 1.0f) s->p.ratio = 1.0f;
    s->makeup_lin = db_to_lin(s->p.makeup_db);
}

/* Static gain curve (dB in -> dB out) with optional soft knee. */
static float comp_curve_gain_db(const CompState *s, float in_db)
{
    float thr  = s->p.threshold_db;
    float knee = s->p.knee_db < 0.0f ? 0.0f : s->p.knee_db;
    float ratio = s->p.ratio;
    float over = in_db - thr;
    float out_db;

    if (knee > 0.0f && over > -knee * 0.5f && over < knee * 0.5f) {
        /* Quadratic soft-knee interpolation. */
        float x = over + knee * 0.5f;            /* 0..knee */
        out_db = in_db + (1.0f / ratio - 1.0f) * (x * x) / (2.0f * knee);
    } else if (over <= 0.0f) {
        out_db = in_db;                          /* below threshold: unity */
    } else {
        out_db = thr + over / ratio;             /* above threshold        */
    }
    return out_db - in_db;                        /* gain reduction (<=0)   */
}

/* ── Limiter (brickwall toward ceiling) ────────────────────────────── */

typedef struct {
    JceAudioLimiterParams p;
    float ceiling_lin;
    float release_coef;
    float gain;          /* current applied gain reduction (0..1)        */
} LimState;

static void lim_design(LimState *s, uint32_t sr)
{
    double fs = sr > 0 ? (double)sr : 44100.0;
    double rel = (double)s->p.release_ms;
    if (rel < 0.0) rel = 0.0;
    s->release_coef = rel <= 0.0 ? 0.0f
                    : (float)exp(-1.0 / ((rel / 1000.0) * fs));
    s->ceiling_lin = db_to_lin(s->p.ceiling_db);
    if (s->ceiling_lin <= 0.0f) s->ceiling_lin = 1.0e-6f;
}

/* ── Delay line ────────────────────────────────────────────────────── */

typedef struct {
    JceAudioDelayParams p;
    float  *buf[JCE_AUDIO_DSP_MAX_CHANNELS];
    int     size;
    int     pos;
} DelayState;

static void delay_free(DelayState *s)
{
    for (int c = 0; c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c) {
        JCE_FREE(s->buf[c]);
        s->buf[c] = NULL;
    }
    s->size = 0;
    s->pos  = 0;
}

static bool delay_alloc(DelayState *s, uint32_t channels, uint32_t sr)
{
    delay_free(s);
    double fs = sr > 0 ? (double)sr : 44100.0;
    double ms = (double)s->p.delay_ms;
    if (ms < 0.0) ms = 0.0;
    int n = (int)(ms / 1000.0 * fs + 0.5);
    if (n < 1) n = 1;
    s->size = n;
    s->pos  = 0;
    for (uint32_t c = 0; c < channels && c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c) {
        s->buf[c] = (float *)JCE_CALLOC((size_t)n, sizeof(float));
        if (!s->buf[c]) { delay_free(s); return false; }
    }
    return true;
}

/* ── Modulated delay: CHORUS and FLANGER ───────────────────────────
 *
 * ONE implementation for both, because they are one machine -- a delay line
 * whose read head is swept by an LFO.  What separates them is only where the
 * numbers sit, which is a fact about presets and not about DSP; two copies
 * would have drifted the moment one of them got a fix.
 *
 * The read head is FRACTIONAL and linearly interpolated.  Rounding it to a
 * whole sample makes the sweep a staircase, and a staircase through a delay
 * line is audible as zipper noise -- the one artefact this effect exists to
 * avoid.
 */
typedef struct {
    JceAudioModDelayParams p;
    float   *buf[JCE_AUDIO_DSP_MAX_CHANNELS];
    int      size;                 /* line length in frames */
    int      pos;                  /* write head            */
    float    phase;                /* LFO phase, 0..1       */
    float    phase_inc;            /* per frame             */
    float    centre;               /* delay in frames       */
    float    depth;                /* sweep in frames       */
} ModDelayState;

static void moddelay_free(ModDelayState *s)
{
    for (int c = 0; c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c) {
        JCE_FREE(s->buf[c]);
        s->buf[c] = NULL;
    }
    s->size = 0;
    s->pos  = 0;
}

static bool moddelay_alloc(ModDelayState *s, uint32_t channels, uint32_t sr)
{
    moddelay_free(s);
    const double fs = sr > 0 ? (double)sr : 44100.0;

    double centre_ms = (double)s->p.delay_ms;
    double depth_ms  = (double)s->p.depth_ms;
    if (centre_ms < 0.0) centre_ms = 0.0;
    if (depth_ms  < 0.0) depth_ms  = 0.0;

    s->centre = (float)(centre_ms / 1000.0 * fs);
    s->depth  = (float)(depth_ms  / 1000.0 * fs);

    /* THE LINE MUST HOLD THE DEEPEST SWEEP PLUS A GUARD.  Sizing it to the
     * centre delay alone lets a deep sweep read PAST the write head and
     * return the newest sample instead of the oldest -- a click once per LFO
     * cycle, which reads as a broken effect rather than a short buffer.  +4
     * covers the interpolator's second tap and the rounding above it. */
    int n = (int)(s->centre + s->depth + 4.0f);
    if (n < 4) n = 4;
    s->size = n;
    s->pos  = 0;

    float rate = s->p.rate_hz;
    if (rate < 0.0f)  rate = 0.0f;
    if (rate > 20.0f) rate = 20.0f;      /* past this it is ring modulation */
    s->phase_inc = (float)((double)rate / fs);
    s->phase     = 0.0f;

    for (uint32_t c = 0; c < channels && c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c) {
        s->buf[c] = (float *)JCE_CALLOC((size_t)n, sizeof(float));
        if (!s->buf[c]) { moddelay_free(s); return false; }
    }
    return true;
}

/* Read `d` frames back from the write head, linearly interpolated. */
static float moddelay_read(const ModDelayState *s, int c, float d)
{
    const float *line = s->buf[c];
    if (!line || s->size < 2) return 0.0f;
    if (d < 1.0f) d = 1.0f;
    if (d > (float)(s->size - 2)) d = (float)(s->size - 2);

    const int   i0   = (int)d;
    const float frac = d - (float)i0;

    int r0 = s->pos - i0;
    while (r0 < 0) r0 += s->size;
    int r1 = r0 - 1;
    if (r1 < 0) r1 += s->size;

    return line[r0] * (1.0f - frac) + line[r1] * frac;
}

/* ── Distortion ───────────────────────────────────────────────────── */

typedef struct {
    JceAudioDistortionParams p;
} DistState;

static float dist_shape(const JceAudioDistortionParams *p, float x)
{
    float ceil = p->ceiling;
    if (ceil <= 0.0001f) ceil = 1.0f;

    switch (p->shape) {
    case JCE_AUDIO_DIST_HARD_CLIP:
        if (x >  ceil) return  ceil;
        if (x < -ceil) return -ceil;
        return x;

    case JCE_AUDIO_DIST_FOLDBACK: {
        /* Fold repeatedly: one fold still leaves anything past 3*ceiling
         * outside the range, and a "folder" that clips the loud parts is
         * just a clipper wearing a different name. */
        float y = x;
        int   guard = 0;
        while ((y > ceil || y < -ceil) && guard++ < 8) {
            if (y >  ceil) y =  2.0f * ceil - y;
            if (y < -ceil) y = -2.0f * ceil - y;
        }
        return y;
    }

    case JCE_AUDIO_DIST_SOFT_CLIP:
    default: {
        /* tanh without the libm call: x*(27+x^2)/(27+9x^2) is the standard
         * cheap approximation and matches tanh closely over the range that
         * matters here.  Scaled by the ceiling so `ceiling` means the same
         * thing for all three shapes rather than being ignored by one. */
        const float t = x / ceil;
        const float t2 = t * t;
        const float y = t * (27.0f + t2) / (27.0f + 9.0f * t2);
        return y * ceil;
    }
    }
}

/* ── Registered effects ────────────────────────────────────────────
 *
 * Process-global and append-only.  Registration happens at startup, before
 * any audio thread reads the table, and nothing ever removes an entry -- so
 * a reader never sees a half-written row and no lock is needed on the audio
 * thread.  Removing that property (unregistering at runtime) would need one.
 */
#define DSP_MAX_CUSTOM 32

typedef struct {
    JceAudioEffectVTable vt;
    bool                 used;
} CustomSlot;

/* ONE object, not an array plus a loose counter.  Two globals for one
 * registry can disagree; one cannot, and it is also one entry in the
 * global-state audit instead of two. */
static struct {
    CustomSlot slot[DSP_MAX_CUSTOM];
    int        count;
} s_custom_registry;

static const JceAudioEffectVTable *custom_vt(int type)
{
    const int i = type - JCE_AUDIO_EFFECT_CUSTOM_BASE;
    if (i < 0 || i >= s_custom_registry.count || !s_custom_registry.slot[i].used)
        return NULL;
    return &s_custom_registry.slot[i].vt;
}

int JCE_CALL jce_audio_dsp_register_effect(const JceAudioEffectVTable *vt)
{
    if (!vt || !vt->name || !vt->name[0] || !vt->process) return -1;

    /* IDEMPOTENT BY NAME.  Two ids for one name would make a saved mixer
     * depend on the order modules happened to register in. */
    const int existing = jce_audio_dsp_find_effect(vt->name);
    if (existing >= 0) return existing;

    if (s_custom_registry.count >= DSP_MAX_CUSTOM) return -1;
    const int i = s_custom_registry.count++;
    s_custom_registry.slot[i].vt   = *vt;
    s_custom_registry.slot[i].used = true;
    return JCE_AUDIO_EFFECT_CUSTOM_BASE + i;
}

int JCE_CALL jce_audio_dsp_find_effect(const char *name)
{
    if (!name || !name[0]) return -1;
    for (int i = 0; i < s_custom_registry.count; ++i)
        if (s_custom_registry.slot[i].used &&
            strcmp(s_custom_registry.slot[i].vt.name, name) == 0)
            return JCE_AUDIO_EFFECT_CUSTOM_BASE + i;
    return -1;
}

/* THE ONE TABLE OF SPELLINGS.  A serialiser and a parser that each carried
 * their own switch is how an effect ends up writable and unreadable. */
static const struct { int type; const char *name; } k_builtin_names[] = {
    { JCE_AUDIO_EFFECT_EQ,         "eq"         },
    /* "comp", not "compressor".  That is what every mixer already saved on
     * disk spells (jce_audio_mixer_config.c and the editor panel both write
     * it), and a table that disagreed with them would resolve -1 for every
     * existing compressor -- a lookup that is wrong only for the effects
     * people actually have. */
    { JCE_AUDIO_EFFECT_COMPRESSOR, "comp"       },
    { JCE_AUDIO_EFFECT_LIMITER,    "limiter"    },
    { JCE_AUDIO_EFFECT_DELAY,      "delay"      },
    { JCE_AUDIO_EFFECT_CHORUS,     "chorus"     },
    { JCE_AUDIO_EFFECT_FLANGER,    "flanger"    },
    { JCE_AUDIO_EFFECT_DISTORTION, "distortion" },
};

const char *JCE_CALL jce_audio_dsp_effect_name(int type)
{
    for (size_t i = 0; i < sizeof k_builtin_names / sizeof k_builtin_names[0]; ++i)
        if (k_builtin_names[i].type == type) return k_builtin_names[i].name;
    const JceAudioEffectVTable *vt = custom_vt(type);
    return vt ? vt->name : NULL;
}

int JCE_CALL jce_audio_dsp_effect_type_from_name(const char *name)
{
    if (!name || !name[0]) return -1;
    for (size_t i = 0; i < sizeof k_builtin_names / sizeof k_builtin_names[0]; ++i)
        if (strcmp(k_builtin_names[i].name, name) == 0)
            return k_builtin_names[i].type;
    return jce_audio_dsp_find_effect(name);
}

/* ── Effect node ───────────────────────────────────────────────────── */

typedef struct {
    int type;                      /* JceAudioEffectType, or a custom id */
    union {
        Biquad        eq;
        CompState     comp;
        LimState      lim;
        DelayState    delay;
        ModDelayState mod;         /* CHORUS and FLANGER */
        DistState     dist;
    } st;
    /* Registered effects keep their state on the heap: the chain cannot know
     * how big somebody else's effect is, and sizing the union for the worst
     * case would make every chain pay for it. */
    void *custom_state;
} Effect;

struct JceAudioDspChain {
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t count;
    Effect   fx[JCE_AUDIO_DSP_MAX_EFFECTS];
};

/* ── Effect lifecycle ──────────────────────────────────────────────── */

static void effect_reset_state(Effect *e)
{
    /* Registered effects first: their ids are outside the enum, so the
     * switch below cannot name them. */
    {
        const JceAudioEffectVTable *vt = custom_vt(e->type);
        if (vt) {
            if (vt->reset) {
                vt->reset(e->custom_state);
            } else if (e->custom_state && vt->state_size > 0) {
                /* NO reset callback means "my history is plain floats", so
                 * zeroing it IS silence.  Documented in the header, because
                 * the alternative -- doing nothing -- would leave a tail
                 * ringing through a rewind. */
                memset(e->custom_state, 0, vt->state_size);
            }
            return;
        }
    }

    switch (e->type) {
    case JCE_AUDIO_EFFECT_EQ:
        biquad_reset(&e->st.eq);
        break;
    case JCE_AUDIO_EFFECT_COMPRESSOR:
        e->st.comp.env = 0.0f;
        break;
    case JCE_AUDIO_EFFECT_LIMITER:
        e->st.lim.gain = 1.0f;
        break;
    case JCE_AUDIO_EFFECT_CHORUS:
    case JCE_AUDIO_EFFECT_FLANGER: {
        ModDelayState *m = &e->st.mod;
        for (int c = 0; c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c)
            if (m->buf[c]) memset(m->buf[c], 0, (size_t)m->size * sizeof(float));
        m->phase = 0.0f;
        break;
    }

    case JCE_AUDIO_EFFECT_DELAY:
        for (int c = 0; c < JCE_AUDIO_DSP_MAX_CHANNELS; ++c)
            if (e->st.delay.buf[c])
                memset(e->st.delay.buf[c], 0,
                       (size_t)e->st.delay.size * sizeof(float));
        e->st.delay.pos = 0;
        break;
    default:
        break;
    }
}

/* Free any heap held by an effect.  THREE kinds hold some now -- the delay
 * line, the modulated delay line, and a registered effect'"'"'s state block --
 * so this is no longer the one-liner its previous comment described. */
static void effect_free(Effect *e)
{
    {
        const JceAudioEffectVTable *vt = custom_vt(e->type);
        if (vt) {
            if (vt->release) vt->release(e->custom_state);
            JCE_FREE(e->custom_state);
            e->custom_state = NULL;
            e->type = JCE_AUDIO_EFFECT_NONE;
            return;
        }
    }
    if (e->type == JCE_AUDIO_EFFECT_CHORUS ||
        e->type == JCE_AUDIO_EFFECT_FLANGER)
        moddelay_free(&e->st.mod);

    if (e->type == JCE_AUDIO_EFFECT_DELAY)
        delay_free(&e->st.delay);
    e->type = JCE_AUDIO_EFFECT_NONE;
}

/* (Re)configure an effect from a desc.  `keep_state` preserves filter/env
 * history when the type is unchanged. */
static bool effect_configure(Effect *e, const JceAudioEffectDesc *desc,
                             uint32_t channels, uint32_t sr, bool keep_state)
{
    bool same_type = (e->type == desc->type);

    /* If the type changes we must release the old effect's resources. */
    if (!same_type)
        effect_free(e);

    e->type = desc->type;

    switch (desc->type) {
    case JCE_AUDIO_EFFECT_EQ:
        biquad_design(&e->st.eq, &desc->u.eq, sr);
        if (!same_type || !keep_state) biquad_reset(&e->st.eq);
        return true;

    case JCE_AUDIO_EFFECT_COMPRESSOR:
        if (!same_type || !keep_state) e->st.comp.env = 0.0f;
        e->st.comp.p = desc->u.comp;
        comp_design(&e->st.comp, sr);
        return true;

    case JCE_AUDIO_EFFECT_LIMITER:
        if (!same_type || !keep_state) e->st.lim.gain = 1.0f;
        e->st.lim.p = desc->u.limiter;
        lim_design(&e->st.lim, sr);
        return true;

    case JCE_AUDIO_EFFECT_DELAY: {
        DelayState *d = &e->st.delay;
        d->p = desc->u.delay;
        if (!delay_alloc(d, channels, sr))
            return false;
        return true;
    }

    case JCE_AUDIO_EFFECT_CHORUS:
    case JCE_AUDIO_EFFECT_FLANGER: {
        /* ONE branch for both: the type is kept so a serialiser and an
         * author can tell them apart, but nothing below it differs. */
        ModDelayState *m = &e->st.mod;
        m->p = desc->u.mod_delay;
        if (!moddelay_alloc(m, channels, sr))
            return false;
        return true;
    }

    case JCE_AUDIO_EFFECT_DISTORTION:
        e->st.dist.p = desc->u.distortion;
        return true;   /* stateless: nothing to keep or reset */

    case JCE_AUDIO_EFFECT_NONE:
        e->type = JCE_AUDIO_EFFECT_NONE;
        return true;

    default: {
        /* A REGISTERED EFFECT.  Anything that is not a built-in is looked up
         * rather than treated as NONE -- silently degrading an unknown type
         * to a pass-through is how an effect ends up authored, saved, loaded
         * and inaudible with nothing reporting it. */
        const JceAudioEffectVTable *vt = custom_vt(desc->type);
        if (!vt) {
            e->type = JCE_AUDIO_EFFECT_NONE;
            return false;
        }
        if (vt->state_size > 0 && !e->custom_state) {
            e->custom_state = JCE_CALLOC(1, vt->state_size);
            if (!e->custom_state) { e->type = JCE_AUDIO_EFFECT_NONE; return false; }
        }
        if (!same_type || !keep_state) {
            if (e->custom_state && vt->state_size > 0)
                memset(e->custom_state, 0, vt->state_size);
        }
        if (vt->configure &&
            !vt->configure(e->custom_state, channels, sr,
                           desc->custom_params, desc->custom_params_size))
            return false;
        return true;
    }
    }
}

/* ── Per-effect processing over an interleaved block ───────────────── */

static void effect_process(Effect *e, float *buf, uint32_t frames,
                           uint32_t channels)
{
    uint32_t ch = channels;
    if (ch > JCE_AUDIO_DSP_MAX_CHANNELS) ch = JCE_AUDIO_DSP_MAX_CHANNELS;

    switch (e->type) {
    case JCE_AUDIO_EFFECT_EQ: {
        Biquad *bq = &e->st.eq;
        for (uint32_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < ch; ++c) {
                float *p = &buf[i * channels + c];
                *p = biquad_tick(bq, (int)c, *p);
            }
        break;
    }

    case JCE_AUDIO_EFFECT_COMPRESSOR: {
        CompState *s = &e->st.comp;
        for (uint32_t i = 0; i < frames; ++i) {
            /* Detect on the max-abs across channels (linked stereo). */
            float peak = 0.0f;
            for (uint32_t c = 0; c < ch; ++c) {
                float a = buf[i * channels + c];
                if (a < 0.0f) a = -a;
                if (a > peak) peak = a;
            }
            /* Envelope follower (attack/release smoothing). */
            float coef = peak > s->env ? s->attack_coef : s->release_coef;
            s->env = coef * (s->env - peak) + peak;
            float env_db   = lin_to_db(s->env);
            float gr_db    = comp_curve_gain_db(s, env_db);  /* <= 0 */
            float gain     = db_to_lin(gr_db) * s->makeup_lin;
            for (uint32_t c = 0; c < ch; ++c)
                buf[i * channels + c] *= gain;
        }
        break;
    }

    case JCE_AUDIO_EFFECT_LIMITER: {
        LimState *s = &e->st.lim;
        for (uint32_t i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (uint32_t c = 0; c < ch; ++c) {
                float a = buf[i * channels + c];
                if (a < 0.0f) a = -a;
                if (a > peak) peak = a;
            }
            /* Required gain to bring this sample to the ceiling. */
            float target = 1.0f;
            if (peak > s->ceiling_lin)
                target = s->ceiling_lin / peak;
            /* Attack is instantaneous (clamp immediately); release ramps. */
            if (target < s->gain)
                s->gain = target;
            else
                s->gain = s->release_coef * (s->gain - target) + target;
            for (uint32_t c = 0; c < ch; ++c)
                buf[i * channels + c] *= s->gain;
        }
        break;
    }

    case JCE_AUDIO_EFFECT_DELAY: {
        DelayState *s = &e->st.delay;
        if (s->size < 1) break;
        float fb  = s->p.feedback;
        if (fb < 0.0f) fb = 0.0f;
        if (fb > 0.99f) fb = 0.99f;
        float wet = s->p.wet;
        float dry = s->p.dry;
        for (uint32_t i = 0; i < frames; ++i) {
            int rp = s->pos;
            for (uint32_t c = 0; c < ch; ++c) {
                float *line = s->buf[c];
                if (!line) continue;
                float x       = buf[i * channels + c];
                float delayed = line[rp];
                line[rp]      = x + delayed * fb;         /* write w/ feedback */
                buf[i * channels + c] = x * dry + delayed * wet;
            }
            if (++s->pos >= s->size) s->pos = 0;
        }
        break;
    }

    case JCE_AUDIO_EFFECT_CHORUS:
    case JCE_AUDIO_EFFECT_FLANGER: {
        ModDelayState *s = &e->st.mod;
        if (s->size < 4) break;
        float fb = s->p.feedback;
        if (fb < 0.0f)  fb = 0.0f;
        if (fb > 0.95f) fb = 0.95f;   /* past this it self-oscillates */
        const float wet = s->p.wet;
        const float dry = s->p.dry;
        float ph_r = s->p.stereo_phase;
        if (ph_r < 0.0f) ph_r = 0.0f;
        if (ph_r > 1.0f) ph_r = 1.0f;

        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < ch; ++c) {
                float *line = s->buf[c];
                if (!line) continue;
                /* The RIGHT channel's LFO runs offset, which is what makes a
                 * chorus wide rather than just thick. */
                float p = s->phase + ((c == 1) ? ph_r : 0.0f);
                p -= (float)(int)p;
                /* Triangle, not sine: no libm on the audio thread, and the
                 * constant sweep RATE of a triangle is what a flanger wants
                 * anyway -- a sine lingers at the turnarounds. */
                const float tri = (p < 0.5f) ? (4.0f * p - 1.0f)
                                             : (3.0f - 4.0f * p);
                const float d   = s->centre + tri * s->depth;

                const float x       = buf[i * channels + c];
                const float delayed = moddelay_read(s, (int)c, d);
                line[s->pos] = x + delayed * fb;
                buf[i * channels + c] = x * dry + delayed * wet;
            }
            if (++s->pos >= s->size) s->pos = 0;
            s->phase += s->phase_inc;
            if (s->phase >= 1.0f) s->phase -= 1.0f;
        }
        break;
    }

    case JCE_AUDIO_EFFECT_DISTORTION: {
        const JceAudioDistortionParams *p = &e->st.dist.p;
        float drive = p->drive;
        if (drive < 0.0f) drive = 0.0f;
        const float wet = p->wet;
        const float dry = p->dry;
        const float out = (p->output_gain != 0.0f) ? p->output_gain : 1.0f;
        for (uint32_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < ch; ++c) {
                float *q = &buf[i * channels + c];
                const float x = *q;
                *q = (x * dry + dist_shape(p, x * drive) * wet) * out;
            }
        break;
    }

    case JCE_AUDIO_EFFECT_NONE:
        break;

    default: {
        const JceAudioEffectVTable *vt = custom_vt(e->type);
        if (vt && vt->process)
            vt->process(e->custom_state, buf, frames, channels);
        break;
    }
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

JceAudioDspChain *JCE_CALL jce_audio_dsp_chain_create(uint32_t channels,
                                                      uint32_t sample_rate)
{
    if (channels < 1 || channels > JCE_AUDIO_DSP_MAX_CHANNELS) return NULL;
    if (sample_rate == 0) return NULL;
    JceAudioDspChain *chain =
        (JceAudioDspChain *)JCE_CALLOC(1, sizeof(*chain));
    if (!chain) return NULL;
    chain->channels    = channels;
    chain->sample_rate = sample_rate;
    chain->count       = 0;
    return chain;
}

void JCE_CALL jce_audio_dsp_chain_destroy(JceAudioDspChain *chain)
{
    if (!chain) return;
    for (uint32_t i = 0; i < chain->count; ++i)
        effect_free(&chain->fx[i]);
    JCE_FREE(chain);
}

uint32_t JCE_CALL jce_audio_dsp_chain_count(const JceAudioDspChain *chain)
{
    return chain ? chain->count : 0;
}

int JCE_CALL jce_audio_dsp_chain_add(JceAudioDspChain *chain,
                                     const JceAudioEffectDesc *desc)
{
    if (!chain || !desc) return -1;
    if (chain->count >= JCE_AUDIO_DSP_MAX_EFFECTS) return -1;
    Effect *e = &chain->fx[chain->count];
    memset(e, 0, sizeof(*e));
    e->type = JCE_AUDIO_EFFECT_NONE;
    if (!effect_configure(e, desc, chain->channels, chain->sample_rate, false)) {
        effect_free(e);
        return -1;
    }
    int idx = (int)chain->count;
    chain->count++;
    return idx;
}

bool JCE_CALL jce_audio_dsp_chain_set(JceAudioDspChain *chain, uint32_t index,
                                      const JceAudioEffectDesc *desc)
{
    if (!chain || !desc || index >= chain->count) return false;
    return effect_configure(&chain->fx[index], desc,
                            chain->channels, chain->sample_rate, true);
}

bool JCE_CALL jce_audio_dsp_chain_remove(JceAudioDspChain *chain, uint32_t index)
{
    if (!chain || index >= chain->count) return false;
    effect_free(&chain->fx[index]);
    /* Shift later effects down by one (preserves order). */
    for (uint32_t i = index; i + 1 < chain->count; ++i)
        chain->fx[i] = chain->fx[i + 1];
    chain->count--;
    memset(&chain->fx[chain->count], 0, sizeof(chain->fx[chain->count]));
    return true;
}

void JCE_CALL jce_audio_dsp_chain_clear(JceAudioDspChain *chain)
{
    if (!chain) return;
    for (uint32_t i = 0; i < chain->count; ++i)
        effect_free(&chain->fx[i]);
    chain->count = 0;
}

JceAudioEffectType JCE_CALL jce_audio_dsp_chain_type(const JceAudioDspChain *chain,
                                                     uint32_t index)
{
    if (!chain || index >= chain->count) return JCE_AUDIO_EFFECT_NONE;
    return chain->fx[index].type;
}

void JCE_CALL jce_audio_dsp_chain_reset(JceAudioDspChain *chain)
{
    if (!chain) return;
    for (uint32_t i = 0; i < chain->count; ++i)
        effect_reset_state(&chain->fx[i]);
}

void JCE_CALL jce_audio_dsp_chain_process(JceAudioDspChain *chain,
                                          float *buffer, uint32_t frame_count)
{
    if (!chain || !buffer || frame_count == 0) return;
    for (uint32_t i = 0; i < chain->count; ++i)
        effect_process(&chain->fx[i], buffer, frame_count, chain->channels);
}

JceAudioEffectDesc JCE_CALL jce_audio_effect_default(JceAudioEffectType type)
{
    JceAudioEffectDesc d;
    memset(&d, 0, sizeof(d));
    d.type = type;
    switch (type) {
    case JCE_AUDIO_EFFECT_EQ:
        d.u.eq.shape        = JCE_AUDIO_EQ_PEAKING;
        d.u.eq.frequency_hz = 1000.0f;
        d.u.eq.gain_db      = 0.0f;
        d.u.eq.q            = 0.707f;
        break;
    case JCE_AUDIO_EFFECT_COMPRESSOR:
        d.u.comp.threshold_db = -18.0f;
        d.u.comp.ratio        = 4.0f;
        d.u.comp.attack_ms    = 5.0f;
        d.u.comp.release_ms   = 100.0f;
        d.u.comp.makeup_db    = 0.0f;
        d.u.comp.knee_db      = 0.0f;
        break;
    case JCE_AUDIO_EFFECT_LIMITER:
        d.u.limiter.ceiling_db = -0.1f;
        d.u.limiter.release_ms = 50.0f;
        break;
    case JCE_AUDIO_EFFECT_DELAY:
        d.u.delay.delay_ms = 250.0f;
        d.u.delay.feedback = 0.3f;
        d.u.delay.wet      = 0.5f;
        d.u.delay.dry      = 1.0f;
        break;
    /* CHORUS AND FLANGER SHARE A STRUCT AND MUST NOT SHARE DEFAULTS -- the
     * numbers are the entire difference between the two effects.  A chorus
     * sits around 20 ms with almost no feedback and a wide stereo sweep; a
     * flanger sits at 2 ms with heavy feedback and sounds like a jet.  Handing
     * both the same defaults would make "chorus" and "flanger" two names for
     * one sound, which is exactly the criticism this row makes of a palette
     * that is shorter than it looks. */
    case JCE_AUDIO_EFFECT_CHORUS:
        d.u.mod_delay.delay_ms     = 22.0f;
        d.u.mod_delay.depth_ms     = 6.0f;
        d.u.mod_delay.rate_hz      = 0.6f;
        d.u.mod_delay.feedback     = 0.05f;
        d.u.mod_delay.wet          = 0.5f;
        d.u.mod_delay.dry          = 1.0f;
        d.u.mod_delay.stereo_phase = 0.33f;
        break;
    case JCE_AUDIO_EFFECT_FLANGER:
        d.u.mod_delay.delay_ms     = 2.0f;
        d.u.mod_delay.depth_ms     = 1.5f;
        d.u.mod_delay.rate_hz      = 0.25f;
        d.u.mod_delay.feedback     = 0.7f;
        d.u.mod_delay.wet          = 0.7f;
        d.u.mod_delay.dry          = 1.0f;
        d.u.mod_delay.stereo_phase = 0.5f;
        break;
    case JCE_AUDIO_EFFECT_DISTORTION:
        d.u.distortion.shape       = JCE_AUDIO_DIST_SOFT_CLIP;
        d.u.distortion.drive       = 4.0f;
        d.u.distortion.ceiling     = 1.0f;
        d.u.distortion.wet         = 1.0f;
        d.u.distortion.dry         = 0.0f;
        /* Drive without a matching cut is just "louder", and an author
         * reaching for distortion is not reaching for +12 dB. */
        d.u.distortion.output_gain = 0.5f;
        break;
    case JCE_AUDIO_EFFECT_NONE:
    default:
        /* A REGISTERED type has no defaults the engine could know.  The zeroed
         * desc plus a NULL custom_params is what its configure() receives, and
         * the vtable is the only thing that can decide what that means. */
        break;
    }
    return d;
}
