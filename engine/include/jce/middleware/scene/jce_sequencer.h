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
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSequencer JceSequencer;

typedef enum {
    JCE_SEQ_TRACK_PROPERTY = 0,
    JCE_SEQ_TRACK_EVENT    = 1,
    JCE_SEQ_TRACK_COLOR    = 2,
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
JCE_API void  jce_sequencer_set_looping(JceSequencer *seq, bool looping);
JCE_API void  jce_sequencer_set_time   (JceSequencer *seq, float t);
JCE_API float jce_sequencer_get_time   (const JceSequencer *seq);

/* Advance dt; honours play/pause and loop. */
JCE_API void  jce_sequencer_update(JceSequencer *seq, float dt);

/* -- Tracks ----------------------------------------------------- */

JCE_API int               jce_sequencer_track_count  (const JceSequencer *seq);
JCE_API const char       *jce_sequencer_track_name   (const JceSequencer *seq, int idx);
JCE_API const char       *jce_sequencer_track_binding(const JceSequencer *seq, int idx);
JCE_API JceSeqTrackType   jce_sequencer_track_type   (const JceSequencer *seq, int idx);

/* Structured binding accessors (P1-L integrator).  Authored as additive
   keys (bindProp / bindEntity / bindEntityName) next to the legacy
   free-form "binding" string; a legacy "<digits>/<prop>" binding is
   fallback-parsed into these fields on load. */
JCE_API const char *jce_sequencer_track_bind_prop_name  (const JceSequencer *seq, int idx);
JCE_API uint64_t    jce_sequencer_track_bind_entity_hint(const JceSequencer *seq, int idx);
JCE_API const char *jce_sequencer_track_bind_entity_name(const JceSequencer *seq, int idx);

/* Evaluate a property track at a given time (linear interpolation). */
JCE_API float jce_sequencer_track_eval_float(const JceSequencer *seq, int idx, float t);

/* Evaluate a color track at a given time (linear in RGB). */
JCE_API void  jce_sequencer_track_eval_color(const JceSequencer *seq, int idx,
                                             float t, float out_rgb[3]);

/* For event tracks, returns the count of event keys whose t lies in
   the half-open interval (t_prev, t_now].  Useful for firing during
   a frame-step from the previous frame's playhead. */
JCE_API int   jce_sequencer_track_events_in_range(const JceSequencer *seq, int idx,
                                                  float t_prev, float t_now);

JCE_EXTERN_C_END

#endif /* JCE_SEQUENCER_PUBLIC_H */
