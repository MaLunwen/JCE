/*
 * jce_music.c -- interactive / adaptive music director (FEATURE 5.3).
 *
 * Pure-CPU state machine.  No audio device, no time source: the host drives it
 * via jce_music_update(dt).  See jce_music.h for the design.
 */

#include <jce/middleware/audio/jce_music.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>
#include <jce/os/core/jce_str.h>

#define LOG_TAG "music"

/* Snap epsilon for "already on a boundary" (fraction of a beat). */
#define BOUNDARY_EPS 1.0e-6f

typedef struct {
    char          name[JCE_MUSIC_NAME];
    JceAudioBusId bus;          /* INVALID if not bound to a mixer bus */
    float         enter;        /* intensity at/below which target = 0 */
    float         full;         /* intensity at/above which target = 1 */
    float         fade_time;    /* seconds to slew gain to a new target */
    float         gain;         /* current linear gain (0..1) */
    float         target;       /* target gain it is slewing toward */
} Layer;

struct JceMusicDirector {
    JceAudioMixer *mixer;        /* not owned */

    float          tempo_bpm;
    uint32_t       beats_per_bar;
    uint32_t       bars_per_segment;

    Layer          layers[JCE_MUSIC_MAX_LAYERS];
    uint32_t       layer_count;

    float          intensity;

    float          playhead;     /* seconds since track start */

    /* Pending quantized transition. */
    bool           xfer_pending;
    float          xfer_time;    /* absolute playhead time to fire at */
    int            xfer_target;  /* segment id to switch to */
    int            cur_segment;  /* segment id currently playing */

    JceMusicTransitionFn xfer_cb;
    void                *xfer_user;

    /* One-shot stinger overlay. */
    float          sting_remaining;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Map intensity onto a layer's [enter,full] audible band -> target gain. */
static float layer_target_for(const Layer *L, float intensity)
{
    /* Hard step when the band is degenerate (enter >= full). */
    if (L->full <= L->enter)
        return intensity >= L->full ? 1.0f : 0.0f;
    if (intensity <= L->enter) return 0.0f;
    if (intensity >= L->full)  return 1.0f;
    return (intensity - L->enter) / (L->full - L->enter);
}

/* ------------------------------------------------------------------ */
/* authoring + lifecycle                                               */
/* ------------------------------------------------------------------ */

bool JCE_CALL jce_music_track_desc_default(JceMusicTrackDesc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->tempo_bpm        = 120.0f;
    out->beats_per_bar    = 4u;
    out->bars_per_segment = 4u;
    out->layer_count      = 0u;
    return true;
}

JceMusicDirector *JCE_CALL jce_music_create(const JceMusicTrackDesc *desc,
                                            JceAudioMixer           *mixer)
{
    if (!desc) return NULL;
    if (desc->tempo_bpm <= 0.0f) return NULL;
    if (desc->layer_count > JCE_MUSIC_MAX_LAYERS) return NULL;

    JceMusicDirector *d = (JceMusicDirector *)JCE_CALLOC(1, sizeof(*d));
    if (!d) return NULL;

    d->mixer            = mixer;
    d->tempo_bpm        = desc->tempo_bpm;
    d->beats_per_bar    = desc->beats_per_bar    ? desc->beats_per_bar    : 4u;
    d->bars_per_segment = desc->bars_per_segment ? desc->bars_per_segment : 1u;
    d->layer_count      = desc->layer_count;
    d->intensity        = 0.0f;
    d->playhead         = 0.0f;
    d->xfer_pending     = false;
    d->xfer_time        = -1.0f;
    d->xfer_target      = -1;
    d->cur_segment      = 0;
    d->sting_remaining  = 0.0f;

    for (uint32_t i = 0; i < d->layer_count; ++i) {
        const JceMusicLayerDesc *src = &desc->layers[i];
        Layer *L = &d->layers[i];
        jce_strlcpy(L->name, src->name, JCE_MUSIC_NAME);
        L->enter     = clamp01(src->intensity_enter);
        L->full      = clamp01(src->intensity_full);
        L->fade_time = src->fade_time < 0.0f ? 0.0f : src->fade_time;
        L->bus       = JCE_AUDIO_BUS_INVALID;
        if (mixer && src->bus_name[0]) {
            JceAudioBusId b = jce_audio_mixer_find_bus(mixer, src->bus_name);
            if (b != JCE_AUDIO_BUS_INVALID) L->bus = b;
            else LOG_WARN(LOG_TAG, "layer '%s' bus '%s' not found", src->name, src->bus_name);
        }
        /* Initial gain == initial target at intensity 0 (no startup ramp). */
        L->target = layer_target_for(L, d->intensity);
        L->gain   = L->target;
        if (L->bus != JCE_AUDIO_BUS_INVALID)
            jce_audio_mixer_set_volume(mixer, L->bus, L->gain);
    }
    return d;
}

void JCE_CALL jce_music_destroy(JceMusicDirector *d)
{
    if (!d) return;
    JCE_FREE(d);
}

/* ------------------------------------------------------------------ */
/* layer queries                                                       */
/* ------------------------------------------------------------------ */

uint32_t JCE_CALL jce_music_layer_count(const JceMusicDirector *d)
{ return d ? d->layer_count : 0u; }

int JCE_CALL jce_music_find_layer(const JceMusicDirector *d, const char *name)
{
    if (!d || !name) return -1;
    for (uint32_t i = 0; i < d->layer_count; ++i)
        if (strncmp(d->layers[i].name, name, JCE_MUSIC_NAME) == 0)
            return (int)i;
    return -1;
}

static bool layer_index_ok(const JceMusicDirector *d, int i)
{ return d && i >= 0 && (uint32_t)i < d->layer_count; }

float JCE_CALL jce_music_layer_gain(const JceMusicDirector *d, int i)
{ return layer_index_ok(d, i) ? d->layers[i].gain : -1.0f; }

float JCE_CALL jce_music_layer_target(const JceMusicDirector *d, int i)
{ return layer_index_ok(d, i) ? d->layers[i].target : -1.0f; }

JceAudioBusId JCE_CALL jce_music_layer_bus(const JceMusicDirector *d, int i)
{ return layer_index_ok(d, i) ? d->layers[i].bus : JCE_AUDIO_BUS_INVALID; }

/* ------------------------------------------------------------------ */
/* intensity (vertical layering)                                       */
/* ------------------------------------------------------------------ */

void JCE_CALL jce_music_set_intensity(JceMusicDirector *d, float intensity)
{
    if (!d) return;
    d->intensity = clamp01(intensity);
    for (uint32_t i = 0; i < d->layer_count; ++i)
        d->layers[i].target = layer_target_for(&d->layers[i], d->intensity);
}

float JCE_CALL jce_music_get_intensity(const JceMusicDirector *d)
{ return d ? d->intensity : 0.0f; }

bool JCE_CALL jce_music_is_fading(const JceMusicDirector *d)
{
    if (!d) return false;
    for (uint32_t i = 0; i < d->layer_count; ++i) {
        float diff = d->layers[i].gain - d->layers[i].target;
        if (diff < 0.0f) diff = -diff;
        if (diff > 1.0e-6f) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* horizontal transition (beat/bar quantize)                           */
/* ------------------------------------------------------------------ */

float JCE_CALL jce_music_next_boundary(float from_time, float bpm,
                                       uint32_t beats_per_bar,
                                       JceMusicQuantize quant)
{
    if (from_time < 0.0f) from_time = 0.0f;
    if (quant == JCE_MUSIC_QUANT_IMMEDIATE || bpm <= 0.0f) return from_time;

    double sec_per_beat = 60.0 / (double)bpm;
    double grid;
    if (quant == JCE_MUSIC_QUANT_BAR) {
        uint32_t bpb = beats_per_bar ? beats_per_bar : 1u;
        grid = sec_per_beat * (double)bpb;
    } else { /* BEAT */
        grid = sec_per_beat;
    }
    if (grid <= 0.0) return from_time;

    /* Number of whole grid cells already elapsed, then advance to the next
     * cell boundary -- but if we are essentially exactly on a boundary, that
     * same boundary counts (don't skip a whole cell). */
    double cells = (double)from_time / grid;
    double floor_cells = floor(cells);
    double frac = cells - floor_cells;            /* 0..1 within current cell */
    double eps_frac = (double)BOUNDARY_EPS;       /* fraction-of-a-cell snap */
    double next_cells;
    if (frac <= eps_frac || frac >= 1.0 - eps_frac)
        next_cells = (frac >= 1.0 - eps_frac) ? (floor_cells + 1.0) : floor_cells;
    else
        next_cells = floor_cells + 1.0;
    return (float)(next_cells * grid);
}

float JCE_CALL jce_music_request_transition(JceMusicDirector *d,
                                            int               to_segment,
                                            JceMusicQuantize  quant)
{
    if (!d || to_segment < 0) return -1.0f;
    float when = jce_music_next_boundary(d->playhead, d->tempo_bpm,
                                         d->beats_per_bar, quant);
    d->xfer_pending = true;
    d->xfer_time    = when;
    d->xfer_target  = to_segment;
    return when;
}

bool JCE_CALL jce_music_transition_pending(const JceMusicDirector *d)
{ return d && d->xfer_pending; }

float JCE_CALL jce_music_transition_time(const JceMusicDirector *d)
{ return (d && d->xfer_pending) ? d->xfer_time : -1.0f; }

int JCE_CALL jce_music_transition_target(const JceMusicDirector *d)
{ return (d && d->xfer_pending) ? d->xfer_target : -1; }

int JCE_CALL jce_music_current_segment(const JceMusicDirector *d)
{ return d ? d->cur_segment : -1; }

void JCE_CALL jce_music_set_transition_cb(JceMusicDirector    *d,
                                          JceMusicTransitionFn cb,
                                          void                *user)
{
    if (!d) return;
    d->xfer_cb   = cb;
    d->xfer_user = user;
}

/* ------------------------------------------------------------------ */
/* playhead + stingers                                                 */
/* ------------------------------------------------------------------ */

float JCE_CALL jce_music_playhead(const JceMusicDirector *d)
{ return d ? d->playhead : 0.0f; }

void JCE_CALL jce_music_fire_stinger(JceMusicDirector *d, float duration_seconds)
{
    if (!d) return;
    d->sting_remaining = duration_seconds > 0.0f ? duration_seconds : 0.0f;
}

bool JCE_CALL jce_music_stinger_active(const JceMusicDirector *d)
{ return d && d->sting_remaining > 0.0f; }

float JCE_CALL jce_music_stinger_remaining(const JceMusicDirector *d)
{ return d ? d->sting_remaining : 0.0f; }

/* ------------------------------------------------------------------ */
/* tick                                                                */
/* ------------------------------------------------------------------ */

void JCE_CALL jce_music_update(JceMusicDirector *d, float dt)
{
    if (!d) return;
    if (dt < 0.0f) dt = 0.0f;

    float prev_head = d->playhead;
    d->playhead = prev_head + dt;

    /* 1. Slew each layer's gain toward its target over fade_time (linear). */
    for (uint32_t i = 0; i < d->layer_count; ++i) {
        Layer *L = &d->layers[i];
        if (L->fade_time <= 0.0f) {
            L->gain = L->target;                 /* instant */
        } else if (L->gain != L->target) {
            float step = dt / L->fade_time;      /* gain units per second * dt */
            if (L->gain < L->target) {
                L->gain += step;
                if (L->gain > L->target) L->gain = L->target;
            } else {
                L->gain -= step;
                if (L->gain < L->target) L->gain = L->target;
            }
        }
        L->gain = clamp01(L->gain);
        /* Push the resolved layer gain onto its bound bus (drives live mix). */
        if (d->mixer && L->bus != JCE_AUDIO_BUS_INVALID)
            jce_audio_mixer_set_volume(d->mixer, L->bus, L->gain);
    }

    /* 2. Fire a scheduled quantized transition once the playhead reaches it.
     *    Uses [prev_head, playhead] so an exact-boundary hit fires this tick. */
    if (d->xfer_pending && d->playhead + 1.0e-6f >= d->xfer_time) {
        int from = d->cur_segment;
        int to   = d->xfer_target;
        d->cur_segment  = to;
        d->xfer_pending = false;
        d->xfer_time    = -1.0f;
        d->xfer_target  = -1;
        if (d->xfer_cb) d->xfer_cb(d, from, to, d->xfer_user);
    }

    /* 3. Count down a one-shot stinger; never touches layer gains/transition. */
    if (d->sting_remaining > 0.0f) {
        d->sting_remaining -= dt;
        if (d->sting_remaining < 0.0f) d->sting_remaining = 0.0f;
    }
}
