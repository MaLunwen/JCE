/*
 * jce_audio_route_apply.h  Bind ECS audio-bus-route component → mixer.
 *
 * When an entity carries a JceAudioBusRouteComponent("SFX"), every
 * voice spawned by its AudioSource should be assigned to the mixer's
 * "SFX" bus so volume / mute / solo on that bus actually affects this
 * source.
 *
 * The runtime call sequence is:
 *   1. AudioSource starts a voice → caller obtains a uint64_t voice id.
 *   2. Caller invokes jce_audio_route_apply_voice(scene, entity,
 *                                                  mixer, voice_id).
 *   3. Helper looks up the entity's JceAudioBusRouteComponent and
 *      calls jce_audio_mixer_assign_voice(mixer, voice_id, bus_id).
 *
 * If the entity has no JceAudioBusRouteComponent, or the named bus
 * doesn't exist in the mixer, the voice is left routed to the master
 * bus (the mixer's default behaviour).
 *
 * Layer: middleware / audio (Layer 4) — public.
 */

#ifndef JCE_AUDIO_ROUTE_APPLY_H
#define JCE_AUDIO_ROUTE_APPLY_H

#include <jce/middleware/audio/jce_audio_mixer.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Apply the entity's bus route to a freshly-spawned voice.
 * Returns true if a non-default route was applied. */
JCE_API bool jce_audio_route_apply_voice(JceScene       *scene,
                                          JceEntity       entity,
                                          JceAudioMixer  *mixer,
                                          uint64_t        voice_id);

JCE_EXTERN_C_END

#endif /* JCE_AUDIO_ROUTE_APPLY_H */
