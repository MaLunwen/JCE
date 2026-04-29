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

#ifdef __cplusplus
}
#endif

#endif /* JCE_AUDIO_MIXER_H */
