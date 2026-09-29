/*
 * jce_audio_mixer.h -- generic hierarchical mixer bus tree.
 *
 * Pure-CPU bookkeeping.  Game creates a tree of named "buses"
 * (Master / SFX / Music / Voice / UI / ...) each with parent ID,
 * volume, mute, solo.  Voices are assigned to a bus via opaque
 * uint64_t voice IDs.  jce_audio_mixer_resolve_volume(bus_id) returns
 * the effective gain produced by walking up the tree (gain *= parent.gain),
 * honouring mute and solo (any solo bus mutes all non-solo branches).
 *
 * Engine-agnostic — no miniaudio/FMOD coupling.  Caller multiplies the
 * resolved bus volume into its own per-voice volume each frame, or
 * pushes the value into a hardware bus.
 *
 * Thread-safety: a JceAudioMixer instance is single-threaded — bus
 * graph mutations (add/remove/set_volume/set_muted/set_solo) and voice
 * assignments must not overlap with resolve_volume() reads.  Typically
 * driven from the game/UI thread; the audio thread reads a snapshot.
 *
 * Example:
 *   JceAudioMixer *mx = jce_audio_mixer_create();
 *   JceAudioBusId sfx   = jce_audio_mixer_add_bus(mx, JCE_AUDIO_BUS_MASTER, "SFX",   1.0f);
 *   JceAudioBusId music = jce_audio_mixer_add_bus(mx, JCE_AUDIO_BUS_MASTER, "Music", 0.7f);
 *   jce_audio_mixer_assign_voice(mx, (uint64_t)gunshot_voice.idx, sfx);
 *   ...
 *   jce_audio_mixer_set_volume(mx, music, 0.4f);  // duck music in cutscene
 *   float gain = jce_audio_mixer_resolve_volume(mx, sfx);
 *   jce_audio_set_master_volume(audio, gain);
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_AUDIO_MIXER_H
#define JCE_AUDIO_MIXER_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceAudioMixer JceAudioMixer;

typedef uint16_t JceAudioBusId;
#define JCE_AUDIO_BUS_INVALID ((JceAudioBusId)0)
#define JCE_AUDIO_BUS_MASTER  ((JceAudioBusId)1)

JCE_API JceAudioMixer *jce_audio_mixer_create(void);
JCE_API void           jce_audio_mixer_destroy(JceAudioMixer *m);

/* Master bus is created automatically with id=1 and parent=INVALID.
 * Returns INVALID on failure (out of memory or unknown parent). */
JCE_API JceAudioBusId  jce_audio_mixer_add_bus(JceAudioMixer *m,
                                               JceAudioBusId  parent,
                                               const char    *name,
                                               float          initial_volume);
JCE_API bool           jce_audio_mixer_remove_bus(JceAudioMixer *m, JceAudioBusId bus);
JCE_API JceAudioBusId  jce_audio_mixer_find_bus(const JceAudioMixer *m, const char *name);
JCE_API uint32_t       jce_audio_mixer_bus_count(const JceAudioMixer *m);

JCE_API void           jce_audio_mixer_set_volume(JceAudioMixer *m, JceAudioBusId bus, float v);
JCE_API float          jce_audio_mixer_get_volume(const JceAudioMixer *m, JceAudioBusId bus);
JCE_API void           jce_audio_mixer_set_muted(JceAudioMixer *m, JceAudioBusId bus, bool muted);
JCE_API bool           jce_audio_mixer_is_muted(const JceAudioMixer *m, JceAudioBusId bus);
JCE_API void           jce_audio_mixer_set_solo(JceAudioMixer *m, JceAudioBusId bus, bool solo);
JCE_API bool           jce_audio_mixer_is_solo(const JceAudioMixer *m, JceAudioBusId bus);

/* Effective volume = product of own volume and ancestors' volumes,
 * times 0 if any ancestor (or self) is muted, times 0 if any bus
 * anywhere is solo'd and `bus` is not on a solo path. */
JCE_API float          jce_audio_mixer_resolve_volume(const JceAudioMixer *m, JceAudioBusId bus);

/* Voice -> bus routing. */
JCE_API void           jce_audio_mixer_assign_voice(JceAudioMixer *m, uint64_t voice_id, JceAudioBusId bus);
JCE_API void           jce_audio_mixer_unassign_voice(JceAudioMixer *m, uint64_t voice_id);
JCE_API JceAudioBusId  jce_audio_mixer_get_voice_bus(const JceAudioMixer *m, uint64_t voice_id);

/* Iterate all bus IDs in creation order; returns count actually written. */
JCE_API uint32_t       jce_audio_mixer_list_buses(const JceAudioMixer *m,
                                                  JceAudioBusId       *out_buses,
                                                  uint32_t             max_count);

JCE_API const char    *jce_audio_mixer_get_name(const JceAudioMixer *m, JceAudioBusId bus);
JCE_API JceAudioBusId  jce_audio_mixer_get_parent(const JceAudioMixer *m, JceAudioBusId bus);

/* ================================================================== *
 *  FEATURE 5.2 — aux send/return, sidechain ducking, snapshots.       *
 * ================================================================== */

/* -- Aux send / return ---------------------------------------------- *
 *
 * Besides the single parent edge (which carries the whole bus signal up the
 * tree), a bus may have any number of *aux sends*: a parallel tap that routes
 * a configurable fraction of its post-fader signal into another bus (typically
 * a dedicated "return" bus that hosts a reverb/delay insert chain).  This is
 * the classic console send/return wiring — e.g. SFX and Music each send 30 %
 * and 10 % into a shared "Reverb" return.
 *
 * The send is multiplicative: the signal arriving at the destination is the
 * *resolved* (mute/solo/ancestor-folded) volume of the source bus times the
 * send fraction.  jce_audio_mixer_resolve_send() returns exactly that scalar,
 * so a host/node-graph multiplies it onto a parallel copy of the source. */

/* Maximum aux sends a single bus may originate. */
#define JCE_AUDIO_MAX_SENDS 8

/* Create or update an aux send from `src` to `dest` carrying `amount`
 * (0..1, clamped) of the source's post-fader signal.  Setting `amount` to 0
 * leaves the send registered but silent; use remove_send to drop it.
 *
 * Returns false when `src` or `dest` is not a live bus, when they are the
 * same bus, when `src` has no free send slot, or when the send would CLOSE A
 * RING -- A->B plus B->A, or any longer cycle.
 *
 * The ring refusal is not fussiness: an aux send is a parallel edge in the
 * audio graph, and a cycle in that graph has no bottom to recurse to.  It was
 * accepted until 2026-09-21 and was harmless only because nothing routed a
 * send's audio at all, so a caller that used to get `true` for the second
 * half of a ring now gets `false` and should surface the refusal rather than
 * dropping it. */
JCE_API bool           jce_audio_mixer_set_send(JceAudioMixer *m,
                                                JceAudioBusId  src,
                                                JceAudioBusId  dest,
                                                float          amount);
/* Current send amount from `src` to `dest` (0 if no such send). */
JCE_API float          jce_audio_mixer_get_send(const JceAudioMixer *m,
                                                JceAudioBusId src, JceAudioBusId dest);
/* Drop the send from `src` to `dest`.  Returns true if one existed. */
JCE_API bool           jce_audio_mixer_remove_send(JceAudioMixer *m,
                                                   JceAudioBusId src, JceAudioBusId dest);
/* Number of aux sends originating from `src`. */
JCE_API uint32_t       jce_audio_mixer_send_count(const JceAudioMixer *m, JceAudioBusId src);
/* Enumerate the aux sends originating from `src` by dense index
 * 0..send_count-1.  Needed by serializers: get_send() returns 0 both for "no
 * send" and "registered but silent (amount 0)", so only enumeration can
 * round-trip zero-amount sends.  Returns true and fills out_dest/out_amount
 * (either may be NULL) when `index` names a live send. */
JCE_API bool           jce_audio_mixer_send_at(const JceAudioMixer *m,
                                               JceAudioBusId src, uint32_t index,
                                               JceAudioBusId *out_dest,
                                               float *out_amount);

/* Effective gain delivered from `src` into `dest` along the aux send:
 *   resolve_volume(src) * send_amount.
 * Returns 0 if there is no send or either bus is muted/solo-suppressed. */
JCE_API float          jce_audio_mixer_resolve_send(const JceAudioMixer *m,
                                                    JceAudioBusId src, JceAudioBusId dest);

/* -- Sidechain ducking ---------------------------------------------- *
 *
 * A *sidechain* makes one bus (the "target", e.g. Music) automatically duck
 * when another bus (the "key", e.g. Voice/Dialogue) is loud.  An envelope
 * follower tracks the key's level; a downward compressor curve converts that
 * to a gain-reduction multiplier applied on top of the target's resolved
 * volume.  This is the standard "music ducks under dialogue" effect.
 *
 * The follower is advanced deterministically by the host by feeding it the
 * key bus's current peak level once per block via duck_advance(); the
 * resulting reduction is read back with duck_gain(), or folded into
 * resolve_volume_ducked().
 *
 * The math lives HERE and nowhere else -- there is no duplicate of it inside
 * the audio backend.  What the backend supplies is the measurement: the live
 * key peak comes from jce_audio_bus_get_peak() (jce_audio.h), which meters
 * the real miniaudio node the bus plays through.  The runtime closes that
 * loop once per frame in rt_apply_mixer() (jce_rt_audio.c).  Because the only
 * device-side part is the meter, the same follower can be driven offline over
 * a synthetic key envelope in a unit test and step for step it is the code
 * that runs live.
 *
 * (Until 2026-09-21 this comment claimed the envelope+curve math ALSO ran in
 * a live miniaudio node.  It never did: duck_advance had no caller outside
 * the tests, so the duck gain was permanently 1.0 in every shipped game while
 * resolve_volume_ducked dutifully applied it.  The sentence is what stopped
 * anyone looking -- it said the live path was handled and the offline driver
 * was merely its test twin.) */

typedef struct {
    JceAudioBusId key;          /* bus whose level drives the duck            */
    float threshold_db;         /* key level above which ducking starts       */
    float ratio;                /* >1 : amount of reduction per dB over thr.  */
    float attack_ms;            /* how fast the duck clamps down              */
    float release_ms;           /* how fast it recovers                       */
    float max_attenuation_db;   /* floor on reduction (e.g. -24 dB), <=0      */
} JceAudioDuckParams;

/* Sensible defaults: -30 dB threshold, 8:1, 10 ms attack, 250 ms release,
 * -24 dB max attenuation; key = INVALID (caller fills it in). */
JCE_API JceAudioDuckParams jce_audio_duck_default_params(void);

/* Install / replace the sidechain on `target` driven by `params.key`.
 * `sample_rate` sizes the attack/release coefficients for the block rate used
 * with duck_advance (pass the audio device rate, e.g. 48000).  Passing a NULL
 * or key==INVALID params clears the sidechain.  Returns true on success. */
JCE_API bool           jce_audio_mixer_set_sidechain(JceAudioMixer *m,
                                                     JceAudioBusId target,
                                                     const JceAudioDuckParams *params,
                                                     uint32_t sample_rate);
/* Remove the sidechain on `target`.  Returns true if one existed. */
JCE_API bool           jce_audio_mixer_clear_sidechain(JceAudioMixer *m, JceAudioBusId target);
/* True if `target` has a sidechain installed. */
JCE_API bool           jce_audio_mixer_has_sidechain(const JceAudioMixer *m, JceAudioBusId target);
/* Read back the duck params currently installed on `target` into `*out`.
 * Returns true if `target` has a sidechain (out filled), false otherwise
 * (out left untouched).  Lets a tool/editor serialize the sidechain config. */
JCE_API bool           jce_audio_mixer_get_sidechain(const JceAudioMixer *m,
                                                     JceAudioBusId target,
                                                     JceAudioDuckParams *out);

/* Advance every installed sidechain by one block of `frames` samples,
 * pulling each key bus's peak level via `key_peak(key_bus, user)` (linear,
 * normally 0..1).  This evolves the envelope follower deterministically:
 * rising key level pulls the duck gain down (toward max_attenuation), falling
 * key level lets it recover toward unity.  `frames` is the block length the
 * coefficients are sized against for THIS call -- they are re-derived from it
 * every time, so a variable block (a frame's worth of audio at a variable
 * frame rate) is exact, not an approximation.
 *
 * `key_peak` must return the key's POST-FADER peak -- its level with its own
 * mixer volume already applied, which is what jce_audio_bus_get_peak() reads
 * off the live bus node.  Only the key's mute/solo state is folded in here,
 * so a muted or solo-excluded key cannot duck its target no matter how loud
 * its signal is.  (Before 2026-09-21 this folded in the key's full resolved
 * VOLUME, which squares the key gain against a post-fader meter: exact at
 * volume 1.0 and 6 dB wrong at 0.5.  It had no live caller then, and its own
 * comment already said "mute/solo".) */
typedef float (*JceAudioKeyPeakFn)(JceAudioBusId key_bus, void *user);
JCE_API void           jce_audio_mixer_duck_advance(JceAudioMixer *m,
                                                    uint32_t frames,
                                                    JceAudioKeyPeakFn key_peak,
                                                    void *user);

/* Current duck gain multiplier (0..1) on `target` from its sidechain
 * (1.0 = no ducking / no sidechain). */
JCE_API float          jce_audio_mixer_duck_gain(const JceAudioMixer *m, JceAudioBusId target);

/* resolve_volume(target) with the sidechain duck gain folded in. */
JCE_API float          jce_audio_mixer_resolve_volume_ducked(const JceAudioMixer *m,
                                                             JceAudioBusId target);

/* -- Mixer snapshots ------------------------------------------------ *
 *
 * A *snapshot* captures a named set of per-bus volumes (e.g. "Combat",
 * "Stealth", "Paused").  Applying a snapshot with a fade time crossfades the
 * live bus volumes from their current values to the snapshot's target over
 * `fade_seconds`, linearly per bus.  The crossfade is advanced by the host
 * once per frame via jce_audio_mixer_update(dt); a fade of 0 snaps instantly. */

#define JCE_AUDIO_MAX_SNAPSHOTS   16
#define JCE_AUDIO_SNAPSHOT_NAME   32

/* Create (or replace) a snapshot named `name` capturing the *current* volume
 * of every live bus.  Returns true on success (false if the table is full). */
JCE_API bool           jce_audio_mixer_capture_snapshot(JceAudioMixer *m, const char *name);
/* Set the stored target volume for `bus` within snapshot `name` explicitly
 * (creating the snapshot if needed).  Lets a game author a snapshot without
 * first dialling the live mixer to it. */
JCE_API bool           jce_audio_mixer_snapshot_set_volume(JceAudioMixer *m,
                                                           const char *name,
                                                           JceAudioBusId bus,
                                                           float volume);
/* Read back a snapshot's stored target volume for `bus` (NaN-free; returns a
 * negative value if the snapshot or bus entry does not exist). */
JCE_API float          jce_audio_mixer_snapshot_get_volume(const JceAudioMixer *m,
                                                           const char *name,
                                                           JceAudioBusId bus);
JCE_API bool           jce_audio_mixer_remove_snapshot(JceAudioMixer *m, const char *name);
JCE_API uint32_t       jce_audio_mixer_snapshot_count(const JceAudioMixer *m);
/* Enumerate stored snapshots by dense index 0..snapshot_count-1, copying the
 * snapshot's name into `out` (NUL-terminated, truncated to `cap`).  Returns
 * true if `index` named a live snapshot.  The order is stable for a given
 * mixer state but is not otherwise guaranteed; iterate by count.  Lets a
 * tool/editor enumerate snapshot names for serialization. */
JCE_API bool           jce_audio_mixer_snapshot_name(const JceAudioMixer *m,
                                                     uint32_t index,
                                                     char *out, uint32_t cap);

/* Begin crossfading the live bus volumes toward snapshot `name` over
 * `fade_seconds` (0 = snap instantly).  Returns false if the snapshot is
 * unknown.  The fade is driven by jce_audio_mixer_update(). */
JCE_API bool           jce_audio_mixer_apply_snapshot(JceAudioMixer *m,
                                                      const char *name,
                                                      float fade_seconds);
/* True while a snapshot crossfade is in progress. */
JCE_API bool           jce_audio_mixer_snapshot_fading(const JceAudioMixer *m);
/* 0..1 progress of the active crossfade (1.0 when none / finished). */
JCE_API float          jce_audio_mixer_snapshot_progress(const JceAudioMixer *m);

/* Advance any in-flight snapshot crossfade by `dt` seconds, writing the
 * interpolated volumes into the live buses.  When the fade completes the live
 * volumes equal the target snapshot exactly.  Safe to call every frame. */
JCE_API void           jce_audio_mixer_update(JceAudioMixer *m, float dt_seconds);

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_MIXER_H */
