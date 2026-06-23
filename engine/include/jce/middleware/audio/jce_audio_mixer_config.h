/*
 * jce_audio_mixer_config.h -- consume the editor's audio_mixer.json routing.
 *
 * The editor's Audio Mixer panel (jce_panel_audio_mixer.cpp) serializes the
 * full authored routing into audio_mixer.json:
 *
 *   {
 *     "buses": [
 *       { "id", "parent", "name", "volume", "muted", "solo",
 *         "sends":     [ { "dest", "amount" }, ... ],
 *         "sidechain": { "key", "threshold_db", "ratio", "attack_ms",
 *                        "release_ms", "floor_db" },
 *         "effects":   [ { "type": "eq|comp|limiter|delay", ... }, ... ] },
 *       ...
 *     ],
 *     "snapshots": [ { "name", "volumes": [ { "id", "volume" }, ... ] }, ... ]
 *   }
 *
 * The runtime previously loaded ONLY the bus tree (volume/mute/solo), so the
 * authored aux sends, sidechain ducking, named snapshots, and per-bus insert
 * effects were persisted yet INERT at Play start.  This module parses the same
 * keys the editor writes and applies them so authored routing goes live.
 *
 * Two distinct surfaces, deliberately split by device coupling:
 *
 *   - jce_audio_mixer_apply_config()  is DEVICE-FREE: it mutates a JceAudioMixer
 *     (pure-CPU bus tree + sends + sidechain + snapshots), so it can be driven
 *     headlessly in a unit test with NO miniaudio device.
 *
 *   - jce_audio_mixer_config_each_effect()  enumerates the per-bus insert-effect
 *     chains (bus NAME + JceAudioEffectDesc in authored order) to a callback.
 *     Effects attach to a LIVE audio device by bus name (jce_audio_bus_add_effect),
 *     which only exists when the device/graph is up — so the runtime drives the
 *     attach in a device-GATED pass and this function stays pure parsing.
 *
 * The id-remap concern: jce_audio_mixer_add_bus reassigns dense bus ids, so the
 * JSON ids referenced by sends/sidechain/snapshots are remapped to the live ids
 * actually assigned during the load (mirrors the editor loader).
 *
 * Layer: middleware (Layer 4) — public.
 */
#ifndef JCE_AUDIO_MIXER_CONFIG_H
#define JCE_AUDIO_MIXER_CONFIG_H

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/audio/jce_audio_dsp.h>

#include <stdbool.h>
#include <stddef.h>

JCE_EXTERN_C_BEGIN

/* Parse `json` (the editor's audio_mixer.json text, `len` bytes) and apply the
 * full authored routing onto `m`: builds the bus tree (volume/mute/solo), then
 * the aux sends, per-bus sidechain ducking, and named snapshots — all through
 * the public JceAudioMixer setters, with a JSON-id -> live-id remap.
 *
 * DEVICE-FREE: touches only the JceAudioMixer bookkeeping (no audio device),
 * so a unit test can drive it headlessly.  Insert effects are NOT applied here
 * (they require a live device — see jce_audio_mixer_config_each_effect).
 *
 * Returns true if at least one bus row parsed (so the caller can fall back to a
 * default tree on an empty/unreadable config).  NULL/empty json -> false. */
JCE_API bool JCE_CALL jce_audio_mixer_apply_config(JceAudioMixer *m,
                                                   const char *json,
                                                   size_t len);

/* Callback receiving one authored insert effect for `bus_name`, in chain order.
 * `index` is the effect's position within its bus chain (0-based). */
typedef void (*JceAudioMixerEffectFn)(const char *bus_name,
                                      const JceAudioEffectDesc *desc,
                                      uint32_t index,
                                      void *user);

/* Enumerate every per-bus insert-effect chain authored in `json`, invoking `cb`
 * once per effect in authored order (bus name + parsed JceAudioEffectDesc).
 * Pure parsing — no device, no mixer mutation; the caller attaches each effect
 * to its live bus by name.  No-op when json is NULL/empty or carries no effects.
 * Returns the number of effects enumerated. */
JCE_API uint32_t JCE_CALL jce_audio_mixer_config_each_effect(const char *json,
                                                             size_t len,
                                                             JceAudioMixerEffectFn cb,
                                                             void *user);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_MIXER_CONFIG_H */
