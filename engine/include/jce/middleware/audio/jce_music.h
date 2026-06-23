/*
 * jce_music.h -- interactive / adaptive music director (FEATURE 5.3).
 *
 * A small, engine-agnostic state machine that drives ADAPTIVE music on top of
 * the existing mixer + voice playback.  Three classic interactive-music tools:
 *
 *   1. VERTICAL re-orchestration ("layers" / "stems").  A track is split into
 *      stacked stems (drums / bass / pad / lead / ...).  Every stem plays in
 *      sync at full level; what the player hears is shaped by fading individual
 *      stems in and out.  A single game "intensity" parameter (0..1) maps onto
 *      a per-stem audible band [enter,full] so rising intensity layers more
 *      stems in and falling intensity peels them back, each over its own fade
 *      time.  The per-stem gain is a deterministic linear ramp toward its
 *      target; the resolved gain is pushed onto the stem's mixer bus (when a
 *      JceAudioMixer is attached) so it drives the live mix through the normal
 *      bus path -- no new audio coupling.
 *
 *   2. HORIZONTAL re-sequencing ("transitions" / "segments").  Switching from
 *      one musical segment to another mid-phrase sounds wrong; transitions are
 *      QUANTIZED to a musical boundary (next beat / bar) derived from the
 *      tempo.  jce_music_request_transition() computes the next boundary time
 *      from the current playhead and schedules the switch there -- it is NOT
 *      immediate.  jce_music_update() fires the (optional) switch callback when
 *      the playhead reaches the scheduled time.
 *
 *   3. STINGERS.  A one-shot musical accent (sting) overlaid on top of the bed
 *      -- a hit on a kill, a fanfare on a pickup.  A stinger is fire-and-forget
 *      and MUST NOT disturb the layer gains or the transition schedule; the
 *      director only tracks how long it has left to play so the host can keep
 *      its overlay voice alive.
 *
 * AUTHORING.  A whole track is described by a flat, minimal struct
 * (JceMusicTrackDesc) -- tempo, beats-per-bar, bars-per-segment, and a list of
 * layers each with {name, intensity enter/full thresholds, fade time}.  This
 * mirrors the JSON a tool would emit field-for-field; see the struct comments.
 *
 * DETERMINISM / TESTABILITY.  The director holds no audio device and no time
 * source: the host advances it with jce_music_update(dt).  All gain ramps and
 * the quantized switch time are pure arithmetic, so the exact runtime math is
 * unit-tested headlessly (and the resolved layer gains can be applied to a real
 * PCM buffer through miniaudio's offline read path with no device).
 *
 * Thread-safety: a JceMusicDirector is single-threaded, same contract as
 * JceAudioMixer -- driven from the game thread; the audio thread reads a
 * resolved snapshot.
 *
 * Layer: middleware (Layer 4) -- public.
 */
#ifndef JCE_MUSIC_H
#define JCE_MUSIC_H

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceMusicDirector JceMusicDirector;

/* Hard limits (kept small and static; a director is cheap to create). */
#define JCE_MUSIC_MAX_LAYERS   16
#define JCE_MUSIC_NAME         32

/* Musical boundary a horizontal transition may be quantized to. */
typedef enum {
    JCE_MUSIC_QUANT_IMMEDIATE = 0, /* switch as soon as possible (no wait)     */
    JCE_MUSIC_QUANT_BEAT      = 1, /* next beat boundary                       */
    JCE_MUSIC_QUANT_BAR       = 2  /* next bar (measure) boundary              */
} JceMusicQuantize;

/* -- Authoring (one stem/layer) ------------------------------------- *
 *
 * A layer is audible (target gain 1) when the intensity parameter is at or
 * above `intensity_full`, silent (target 0) at or below `intensity_enter`, and
 * linearly cross-faded in between.  Set enter==full for a hard switch at that
 * threshold.  A "base" layer that is always on uses enter=full=0.  `fade_time`
 * is how long (seconds) the gain takes to slew to a newly-chosen target.  When
 * a mixer is attached, `bus` (looked up by `bus_name` at build time) receives
 * the resolved gain each tick. */
typedef struct {
    char  name[JCE_MUSIC_NAME];     /* stem name (e.g. "drums")                */
    char  bus_name[JCE_MUSIC_NAME]; /* mixer bus to drive (empty = none)       */
    float intensity_enter;          /* intensity at/below which target = 0     */
    float intensity_full;           /* intensity at/above which target = 1     */
    float fade_time;                /* seconds to slew gain to a new target    */
} JceMusicLayerDesc;

/* -- Authoring (a whole track) -------------------------------------- */
typedef struct {
    float             tempo_bpm;      /* beats per minute (> 0)                 */
    uint32_t          beats_per_bar;  /* time signature numerator (>= 1)        */
    uint32_t          bars_per_segment; /* segment length in bars (>= 1)        */
    uint32_t          layer_count;
    JceMusicLayerDesc layers[JCE_MUSIC_MAX_LAYERS];
} JceMusicTrackDesc;

/* Fill `out` with sensible defaults: 120 BPM, 4/4, 4-bar segments, no layers.
 * Returns false only if `out` is NULL. */
JCE_API bool JCE_CALL jce_music_track_desc_default(JceMusicTrackDesc *out);

/* -- Lifecycle ------------------------------------------------------ */

/* Build a director from `desc`.  If `mixer` is non-NULL, each layer whose
 * `bus_name` resolves to an existing bus is bound to that bus and will have its
 * resolved gain pushed onto the bus volume every update; layers with no/unknown
 * bus simply expose their gain via jce_music_layer_gain() for the host to use.
 * The director does NOT take ownership of `mixer`.  Returns NULL on bad args
 * (NULL/invalid tempo or > JCE_MUSIC_MAX_LAYERS layers). */
JCE_API JceMusicDirector *JCE_CALL jce_music_create(const JceMusicTrackDesc *desc,
                                                    JceAudioMixer           *mixer);
JCE_API void              JCE_CALL jce_music_destroy(JceMusicDirector *d);

/* -- Layer queries -------------------------------------------------- */

JCE_API uint32_t      JCE_CALL jce_music_layer_count(const JceMusicDirector *d);
JCE_API int           JCE_CALL jce_music_find_layer(const JceMusicDirector *d,
                                                    const char *name);
/* Current (post-ramp) linear gain of layer `i` (0..1); -1 on bad index. */
JCE_API float         JCE_CALL jce_music_layer_gain(const JceMusicDirector *d, int i);
/* The target gain layer `i` is currently slewing toward (0..1); -1 on bad. */
JCE_API float         JCE_CALL jce_music_layer_target(const JceMusicDirector *d, int i);
/* The mixer bus bound to layer `i` (INVALID if none). */
JCE_API JceAudioBusId JCE_CALL jce_music_layer_bus(const JceMusicDirector *d, int i);

/* -- Intensity (vertical layering) ---------------------------------- */

/* Set the game intensity parameter (0..1, clamped).  Recomputes every layer's
 * target gain from its [enter,full] band; jce_music_update() then ramps the
 * current gains toward those targets over each layer's fade_time. */
JCE_API void  JCE_CALL jce_music_set_intensity(JceMusicDirector *d, float intensity);
JCE_API float JCE_CALL jce_music_get_intensity(const JceMusicDirector *d);
/* True while any layer's current gain has not yet reached its target. */
JCE_API bool  JCE_CALL jce_music_is_fading(const JceMusicDirector *d);

/* -- Horizontal transition (beat/bar-quantized switch) -------------- */

/* Request a switch to segment id `to_segment`, quantized to the next `quant`
 * boundary from the CURRENT playhead.  Returns the absolute playhead time
 * (seconds) at which the switch will fire -- this is the NEXT boundary at or
 * after now, never immediate (unless quant==IMMEDIATE).  Returns a negative
 * value on bad args.  Only one transition may be pending; requesting again
 * replaces it. */
JCE_API float JCE_CALL jce_music_request_transition(JceMusicDirector *d,
                                                    int               to_segment,
                                                    JceMusicQuantize  quant);
/* True while a quantized transition is scheduled but not yet fired. */
JCE_API bool  JCE_CALL jce_music_transition_pending(const JceMusicDirector *d);
/* Absolute playhead time the pending transition fires at (< 0 if none). */
JCE_API float JCE_CALL jce_music_transition_time(const JceMusicDirector *d);
/* Segment id the pending transition targets (< 0 if none). */
JCE_API int   JCE_CALL jce_music_transition_target(const JceMusicDirector *d);
/* The segment id currently playing (starts at 0). */
JCE_API int   JCE_CALL jce_music_current_segment(const JceMusicDirector *d);

/* Pure helper: the absolute time of the next `quant` boundary at or after
 * `from_time`, given tempo `bpm` and `beats_per_bar`.  Exposed so a host (or
 * test) can compute boundaries without a director.  A time already exactly on a
 * boundary returns that same time. */
JCE_API float JCE_CALL jce_music_next_boundary(float from_time, float bpm,
                                               uint32_t beats_per_bar,
                                               JceMusicQuantize quant);

/* Fired by jce_music_update() the instant the playhead crosses a scheduled
 * transition; `from`/`to` are segment ids.  The host swaps the playing segment
 * here (e.g. seamlessly seeks/cross-starts the new segment's voices). */
typedef void (JCE_CALL *JceMusicTransitionFn)(JceMusicDirector *d,
                                              int from_segment, int to_segment,
                                              void *user);
JCE_API void JCE_CALL jce_music_set_transition_cb(JceMusicDirector    *d,
                                                  JceMusicTransitionFn cb,
                                                  void                *user);

/* -- Playhead ------------------------------------------------------- */

/* Current musical playhead (seconds since the track started).  Advanced by
 * jce_music_update(); also readable so a transition time can be compared. */
JCE_API float JCE_CALL jce_music_playhead(const JceMusicDirector *d);

/* -- Stingers (one-shot overlay) ------------------------------------ */

/* Fire a one-shot stinger that overlays the bed for `duration_seconds`.  This
 * does NOT change any layer gain or the transition schedule -- the host plays
 * its own overlay voice; the director merely tracks the remaining time so the
 * host can poll jce_music_stinger_active()/jce_music_stinger_remaining() to
 * know when to release the overlay.  A new stinger replaces a playing one. */
JCE_API void  JCE_CALL jce_music_fire_stinger(JceMusicDirector *d, float duration_seconds);
JCE_API bool  JCE_CALL jce_music_stinger_active(const JceMusicDirector *d);
JCE_API float JCE_CALL jce_music_stinger_remaining(const JceMusicDirector *d);

/* -- Tick ----------------------------------------------------------- */

/* Advance the director by `dt` seconds:
 *   - advances the playhead,
 *   - slews each layer's gain toward its target over fade_time (linear),
 *   - pushes resolved layer gains onto bound mixer buses,
 *   - fires the transition callback (once) when the playhead reaches a
 *     scheduled quantized switch, advancing the current segment,
 *   - counts down any active stinger.
 * Safe to call every frame; a dt <= 0 is treated as 0. */
JCE_API void JCE_CALL jce_music_update(JceMusicDirector *d, float dt);

JCE_EXTERN_C_END

#endif /* JCE_MUSIC_H */
