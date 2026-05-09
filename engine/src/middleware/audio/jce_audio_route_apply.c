/*
 * jce_audio_route_apply.c  Read JceAudioBusRouteComponent → mixer.
 */

#include <jce/middleware/audio/jce_audio_route_apply.h>
#include <jce/middleware/scene/jce_scene.h>

bool jce_audio_route_apply_voice(JceScene *scene, JceEntity entity,
                                  JceAudioMixer *mixer, uint64_t voice_id)
{
    if (!scene || !mixer || voice_id == 0) return false;

    JceAudioBusRouteComponent *route =
        jce_scene_get_audio_bus_route(scene, entity);
    if (!route || !route->bus_name[0]) return false;

    JceAudioBusId bus = jce_audio_mixer_find_bus(mixer, route->bus_name);
    if (bus == JCE_AUDIO_BUS_INVALID) return false;

    jce_audio_mixer_assign_voice(mixer, voice_id, bus);
    return true;
}
