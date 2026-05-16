/*
 * jce_sequencer.h -- Sequencer runtime (P1-L backend).
 *
 * Loads .seq.json files authored by the editor's Sequencer panel and
 * evaluates per-track values at a given time.  The runtime knows
 * nothing about scene entities; the integrator queries
 * jce_sequencer_track_eval_*() and applies the result to whatever
 * binding string the track carries.
 *
 * Layer: Scene (Layer 3) — public.
 */

#ifndef JCE_SEQUENCER_PUBLIC_H
#define JCE_SEQUENCER_PUBLIC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSequencer JceSequencer;

typedef enum {
    JCE_SEQ_TRACK_PROPERTY   = 0,
    JCE_SEQ_TRACK_EVENT      = 1,
    JCE_SEQ_TRACK_COLOR      = 2,
    /* Phase-2 track types (B18.6). */
    JCE_SEQ_TRACK_ACTIVATION = 3,    /* enable/disable entity in time ranges */
    JCE_SEQ_TRACK_ANIMATION  = 4,    /* bind clip + blend over duration */
    JCE_SEQ_TRACK_AUDIO      = 5,    /* play clip with delay + pan + volume */
    JCE_SEQ_TRACK_SIGNAL     = 6,    /* emit named signal at markers */
    JCE_SEQ_TRACK_MARKER     = 7,    /* scrubber markers, no runtime effect */
} JceSeqTrackType;

/* -- Lifecycle --------------------------------------------------- */

JCE_API JceSequencer *jce_sequencer_load_file(const char *path);
JCE_API JceSequencer *jce_sequencer_load_text(const char *text, size_t len);
JCE_API void          jce_sequencer_free(JceSequencer *seq);

/* -- Playback state --------------------------------------------- */

JCE_API float jce_sequencer_duration(const JceSequencer *seq);
JCE_API int   jce_sequencer_fps     (const JceSequencer *seq);
JCE_API bool  jce_sequencer_looping (const JceSequencer *seq);

JCE_API void  jce_sequencer_set_playing(JceSequencer *seq, bool playing);
JCE_API void  jce_sequencer_set_time   (JceSequencer *seq, float t);
JCE_API float jce_sequencer_get_time   (const JceSequencer *seq);

/* Advance dt; honours play/pause and loop. */
JCE_API void  jce_sequencer_update(JceSequencer *seq, float dt);

/* -- Tracks ----------------------------------------------------- */

JCE_API int               jce_sequencer_track_count  (const JceSequencer *seq);
JCE_API const char       *jce_sequencer_track_name   (const JceSequencer *seq, int idx);
JCE_API const char       *jce_sequencer_track_binding(const JceSequencer *seq, int idx);
JCE_API JceSeqTrackType   jce_sequencer_track_type   (const JceSequencer *seq, int idx);

/* Evaluate a property track at a given time (linear interpolation). */
JCE_API float jce_sequencer_track_eval_float(const JceSequencer *seq, int idx, float t);

/* Evaluate a color track at a given time (linear in RGB). */
void  jce_sequencer_track_eval_color(const JceSequencer *seq, int idx,
                                     float t, float out_rgb[3]);

/* For event tracks, returns the count of event keys whose t lies in
   the half-open interval (t_prev, t_now].  Useful for firing during
   a frame-step from the previous frame's playhead. */
int   jce_sequencer_track_events_in_range(const JceSequencer *seq, int idx,
                                          float t_prev, float t_now);

/* ── Phase-2 track type evaluators (B18.6) ──────────────────── */

/* Activation: returns true when `t` lies in any of the track's
 * active ranges. */
JCE_API bool jce_sequencer_track_eval_activation(const JceSequencer *seq,
                                                   int idx, float t);

/* Animation track: locate the clip whose [start, start+duration)
 * contains `t`, return its clip path + normalised time + weight.
 * Returns false if no clip is active.  When `out_clip_path` is NULL,
 * the caller is just asking "is anything active". */
JCE_API bool jce_sequencer_track_eval_anim_clip(const JceSequencer *seq,
                                                  int idx, float t,
                                                  const char **out_clip_path,
                                                  float       *out_clip_t,
                                                  float       *out_weight);

/* Audio track: same as animation, but returns clip path + per-key
 * volume + pan.  Useful for one-shot SFX layered along the timeline. */
JCE_API bool jce_sequencer_track_eval_audio_clip(const JceSequencer *seq,
                                                   int idx, float t,
                                                   const char **out_clip_path,
                                                   float       *out_volume,
                                                   float       *out_pan);

/* Signal: returns count of named signals fired in (t_prev, t_now].
 * `out_names` is filled with up to `cap` matching signal names. */
JCE_API int  jce_sequencer_track_signals_in_range(const JceSequencer *seq,
                                                    int idx,
                                                    float t_prev, float t_now,
                                                    const char **out_names,
                                                    int cap);

/* Marker: returns the marker name at time `t` (within `tolerance`
 * seconds), or NULL when no marker is near. */
JCE_API const char *jce_sequencer_track_marker_at(const JceSequencer *seq,
                                                    int idx, float t,
                                                    float tolerance);

JCE_EXTERN_C_END

#endif /* JCE_SEQUENCER_PUBLIC_H */
