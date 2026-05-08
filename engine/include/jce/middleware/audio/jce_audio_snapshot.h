/*
 * jce_audio_snapshot.h  Mixer snapshot capture / restore with fade.
 *
 * A "snapshot" is a recorded set of (bus_id → volume / muted / solo)
 * tuples taken from a JceAudioMixer.  Game / cutscene scripts can
 * capture multiple named snapshots ("Combat", "Stealth", "Menu") and
 * smoothly transition the mixer between them with an exponential
 * fade.  Mirrors Unity's AudioMixerSnapshot.TransitionTo().
 *
 * Pure CPU.  Snapshot fade integration is single-threaded — call
 * jce_audio_snapshot_tick(dt) once per audio update to apply the
 * current fade target.
 *
 * Layer: middleware (Layer 4) — public.
 */

#ifndef JCE_AUDIO_SNAPSHOT_H
#define JCE_AUDIO_SNAPSHOT_H

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceAudioSnapshot JceAudioSnapshot;

/* Capture the current state of every bus in the mixer.  The returned
 * snapshot owns its own copy of the data and is independent of further
 * mixer mutations.  Caller frees with jce_audio_snapshot_destroy(). */
JCE_API JceAudioSnapshot *jce_audio_snapshot_capture(const JceAudioMixer *m);

/* Discard a snapshot. */
JCE_API void              jce_audio_snapshot_destroy(JceAudioSnapshot *s);

/* Begin a smooth transition over `seconds` from the current mixer
 * state to the snapshot's recorded state.  If `seconds <= 0`, applies
 * instantaneously.  If a transition is already in progress, the new
 * one supersedes it and re-snapshots the current state as the from-
 * point so volume jumps don't occur.
 *
 * Volume blends linearly in dB (i.e., logarithmically); mute/solo
 * snap mid-fade (50%) since they're boolean. */
JCE_API void              jce_audio_snapshot_transition_to(JceAudioMixer *m,
                                                            const JceAudioSnapshot *s,
                                                            float seconds);

/* Tick the in-progress fade.  Must be called once per audio update;
 * dt is in seconds.  No-op when no transition is active. */
JCE_API void              jce_audio_snapshot_tick(JceAudioMixer *m, float dt);

/* Snapshot to apply instantly without fade — equivalent to
 * jce_audio_snapshot_transition_to(m, s, 0). */
JCE_API void              jce_audio_snapshot_apply(JceAudioMixer *m,
                                                    const JceAudioSnapshot *s);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_SNAPSHOT_H */
