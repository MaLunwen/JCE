/*
 * jce_audio_duck_pump.h -- the head of sidechain ducking.
 *
 * FEATURE 5.2 splits ducking across two modules on purpose, and neither may
 * include the other:
 *
 *   jce_audio_mixer.h   the envelope follower and compressor curve.  Pure
 *                       bookkeeping, and its own header promises "no
 *                       miniaudio/FMOD coupling" -- which is what lets the
 *                       follower be driven offline over a synthetic envelope
 *                       and be step-for-step the code that runs live.
 *   jce_audio.h         the live bus meter, jce_audio_bus_get_peak(), read
 *                       off the real node graph.
 *
 * This header is the JOIN, which is why it is its own file rather than a
 * function on either side: putting it in the mixer header would make that
 * header's central claim false, and putting it in the audio header would give
 * the backend a dependency on bus bookkeeping it does not need.
 *
 * It also exists so the join is written ONCE.  Before it, the runtime had
 * these few lines inline and the unit test had a second copy describing
 * itself as a model of the first -- an arrangement in which a defect in the
 * join is invisible, because the test's copy can be correct while the
 * runtime's is not.  There was such a defect.
 *
 * Layer: middleware (Layer 4) -- public.
 */
#ifndef JCE_AUDIO_DUCK_PUMP_H
#define JCE_AUDIO_DUCK_PUMP_H

#include <jce/middleware/audio/jce_audio.h>
#include <jce/middleware/audio/jce_audio_mixer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Advance every sidechain installed on `mixer` by `dt` seconds of audio,
 * sampling each key bus's LIVE level from `audio`.
 *
 * Call it once per frame, before reading the duck gain back out with
 * jce_audio_mixer_resolve_volume_ducked() -- which is exactly what the
 * runtime does in rt_apply_mixer().  Without this call the follower never
 * advances, the duck gain is a permanent 1.0, and resolve_volume_ducked()
 * dutifully multiplies it in every frame while nothing ducks.  That was the
 * shipped state until 2026-09-21.
 *
 * The key level comes from jce_audio_bus_get_peak(), which is POST-FADER and
 * READ-AND-CLEAR.  Post-fader is what duck_advance expects: it folds in only
 * the key's mute/solo state, never its volume, because the volume is already
 * inside the measurement.  Read-and-clear is why this function memoises
 * internally -- two targets are allowed to share one key, and reading the
 * meter once per sidechain would leave the second target with nothing.
 *
 * dt <= 0 holds every duck gain where it is, which is correct for a paused
 * frame: no audio was rendered, so there is nothing to measure.  A dt longer
 * than one second is clamped, which settles the follower to the current key
 * level rather than replaying the gap.  No-ops on a NULL argument. */
JCE_API void jce_audio_duck_pump(JceAudio *audio, JceAudioMixer *mixer,
                                 float dt);

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_DUCK_PUMP_H */
