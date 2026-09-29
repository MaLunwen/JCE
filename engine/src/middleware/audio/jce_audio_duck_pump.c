/*
 * jce_audio_duck_pump.c  --  the head of sidechain ducking.
 *
 * FEATURE 5.2 splits ducking across two modules on purpose:
 *
 *   jce_audio_mixer.c   the envelope follower and compressor curve.  Pure
 *                       bookkeeping, no device, no miniaudio -- which is what
 *                       lets it be driven offline over a synthetic envelope.
 *   jce_audio.c         the live bus meter (jce_audio_bus_get_peak), sampled
 *                       on the real node graph by the audio thread.
 *
 * Neither may include the other: the mixer would stop being device-free, and
 * the audio backend would gain a dependency on bus bookkeeping it does not
 * need.  This file is the one place that knows about both, and it exists so
 * that the join lives in ONE place instead of being written out at every
 * caller -- the runtime had it inline, and the unit test then had a second
 * copy that called itself a model of the first.  A defect in the join (there
 * was one; see the memo below) was invisible to that arrangement, because the
 * test's copy could be correct while the runtime's was not.
 */

#include <jce/middleware/audio/jce_audio_duck_pump.h>

#include <stdint.h>

/* The rate the sidechain coefficients are sized against.  It must match what
 * installs them -- jce_audio_mixer_config.c passes 48000 to set_sidechain for
 * every authored sidechain, and the editor's mixer panel does the same -- or
 * the attack/release times stop meaning milliseconds.  Named once, here,
 * rather than at each caller of duck_advance. */
#define DUCK_RATE 48000u

/* Bound on distinct key buses memoised per pump.  jce_audio_mixer's bus table
 * is the real limit; this is sized past it so that raising that one cannot
 * silently turn the memo into a buffer overrun. */
#define DUCK_MAX_KEYS 64

typedef struct {
    JceAudio      *audio;
    JceAudioMixer *mixer;
    uint32_t       n;
    JceAudioBusId  ids[DUCK_MAX_KEYS];
    float          peaks[DUCK_MAX_KEYS];
} KeyCache;

/* jce_audio_bus_get_peak is READ-AND-CLEAR, and duck_advance calls back once
 * per installed sidechain.  Two targets may share one key -- Music and SFX
 * both ducking under Voice is the ordinary configuration, not an exotic one
 * -- so reading the meter per callback would hand the first target the key's
 * peak and the second whatever arrived in the microseconds after it, i.e.
 * nothing.  That bus would simply not duck: silent, and only in projects that
 * configure a second sidechain on the same key.  One read per key per pump,
 * shared. */
static float key_peak(JceAudioBusId key, void *user)
{
    KeyCache *c = (KeyCache *)user;
    for (uint32_t i = 0; i < c->n; ++i)
        if (c->ids[i] == key) return c->peaks[i];

    const char *nm = jce_audio_mixer_get_name(c->mixer, key);
    float pk = (nm && nm[0]) ? jce_audio_bus_get_peak(c->audio, nm) : 0.0f;
    if (c->n < DUCK_MAX_KEYS) {
        c->ids[c->n]   = key;
        c->peaks[c->n] = pk;
        c->n++;
    }
    return pk;
}

void jce_audio_duck_pump(JceAudio *audio, JceAudioMixer *mixer, float dt)
{
    if (!audio || !mixer) return;

    /* A long hitch settles the follower to the current key level rather than
     * replaying the gap, which is what a large block count already makes the
     * coefficients do -- the clamp just keeps the multiply in range.  dt <= 0
     * (a paused frame) holds the gain where it is, which is correct: nothing
     * was rendered to measure. */
    float step = dt > 1.0f ? 1.0f : dt;
    if (step <= 0.0f) return;

    KeyCache keys;
    keys.audio = audio;
    keys.mixer = mixer;
    keys.n     = 0;
    jce_audio_mixer_duck_advance(mixer,
                                 (uint32_t)(step * (float)DUCK_RATE),
                                 key_peak, &keys);
}
