/*
 * jce_scene_components_audio.c  Scene component (de)serialize module
 * for the audio domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_audio_source(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAudioSourceComponent as;
    memset(&as, 0, sizeof(as));
    copy_str(as.clip_path, sizeof(as.clip_path), j_str(c, "clipPath", ""));
    as.volume        = (float)j_num(c, "volume", 1.0);
    as.pitch         = (float)j_num(c, "pitch", 1.0);
    as.spatial_blend  = (float)j_num(c, "spatialBlend", 0.0);
    as.loop          = j_bool(c, "loop", false);
    as.play_on_awake = j_bool(c, "playOnAwake", true);
    /* 3D attenuation (large-world audio); absent keys -> 0 => legacy defaults. */
    as.attenuation_model = j_num(c, "attenuationModel", 0.0);
    as.min_distance      = (float)j_num(c, "minDistance",   0.0);
    as.max_distance      = (float)j_num(c, "maxDistance",   0.0);
    as.rolloff_factor    = (float)j_num(c, "rolloffFactor", 0.0);
    copy_str(as.mixer_bus, sizeof(as.mixer_bus), j_str(c, "mixerBus", ""));
    jce_scene_set_audio_source(s, e, &as);
}

void parse_music_track(JceScene *s, JceEntity e, const cJSON *c)
{
    JceMusicTrackComponent m;
    memset(&m, 0, sizeof(m));
    copy_str(m.track_path, sizeof(m.track_path), j_str(c, "trackPath", ""));
    m.play_on_awake      = j_bool(c, "playOnAwake", true);
    m.initial_intensity  = (float)j_num(c, "initialIntensity", 0.0);
    if (m.initial_intensity < 0.0f) m.initial_intensity = 0.0f;
    if (m.initial_intensity > 1.0f) m.initial_intensity = 1.0f;
    m.bpm                = (int)j_num(c, "bpm", 120);
    jce_scene_set_music_track(s, e, &m);
}

void parse_audio_listener(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAudioListenerComponent l;
    memset(&l, 0, sizeof(l));
    l.volume         = (float)j_num(c, "volume", 1.0);
    l.paused         = j_bool(c, "paused", false);
    l.spatialize     = j_bool(c, "spatialize", true);
    l.doppler_factor = (float)j_num(c, "dopplerFactor", 1.0);
    jce_scene_set_audio_listener(s, e, &l);
}

void parse_audio_reverb_zone(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAudioReverbZoneComponent r;
    memset(&r, 0, sizeof(r));
    r.preset            = (int)j_num(c, "preset", 0);
    r.min_distance      = (float)j_num(c, "minDistance", 10.0);
    r.max_distance      = (float)j_num(c, "maxDistance", 15.0);
    r.room              = (float)j_num(c, "room", -1000.0);
    r.room_hf           = (float)j_num(c, "roomHF", -100.0);
    r.decay_time        = (float)j_num(c, "decayTime", 1.49);
    r.decay_hf_ratio    = (float)j_num(c, "decayHFRatio", 0.83);
    r.reflections       = (float)j_num(c, "reflections", -2602.0);
    r.reflections_delay = (float)j_num(c, "reflectionsDelay", 0.007);
    r.reverb            = (float)j_num(c, "reverb", 200.0);
    r.reverb_delay      = (float)j_num(c, "reverbDelay", 0.011);
    r.hf_reference      = (float)j_num(c, "hfReference", 5000.0);
    r.diffusion         = (float)j_num(c, "diffusion", 100.0);
    r.density           = (float)j_num(c, "density", 100.0);
    jce_scene_set_audio_reverb_zone(s, e, &r);
}

void parse_audio_occlusion(JceScene *s, JceEntity e, const cJSON *c)
{
    JceAudioOcclusionComponent o;
    memset(&o, 0, sizeof(o));
    o.radius            = (float)j_num(c, "radius", 5.0);
    o.attenuation_db    = (float)j_num(c, "attenuationDb", -12.0);
    o.lowpass_cutoff_hz = (float)j_num(c, "lowpassCutoffHz", 1000.0);
    o.layer_mask        = (int)j_num(c, "layerMask", -1);
    o.affects_reverb    = j_bool(c, "affectsReverb", true);
    jce_scene_set_audio_occlusion(s, e, &o);
}

static void ser_audio_source(const JceAudioSourceComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "AudioSource");
    cJSON_AddStringToObject(o, "clipPath", c->clip_path);
    cJSON_AddNumberToObject(o, "volume", c->volume);
    cJSON_AddNumberToObject(o, "pitch", c->pitch);
    cJSON_AddNumberToObject(o, "spatialBlend", c->spatial_blend);
    cJSON_AddBoolToObject(o, "loop", c->loop);
    cJSON_AddBoolToObject(o, "playOnAwake", c->play_on_awake);
    cJSON_AddNumberToObject(o, "attenuationModel", c->attenuation_model);
    cJSON_AddNumberToObject(o, "minDistance",   c->min_distance);
    cJSON_AddNumberToObject(o, "maxDistance",   c->max_distance);
    cJSON_AddNumberToObject(o, "rolloffFactor", c->rolloff_factor);
    cJSON_AddStringToObject(o, "mixerBus", c->mixer_bus);
    cJSON_AddItemToArray(arr, o);
}

static void ser_music_track(const JceMusicTrackComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "MusicTrack");
    cJSON_AddStringToObject(o, "trackPath", c->track_path);
    cJSON_AddBoolToObject  (o, "playOnAwake", c->play_on_awake);
    cJSON_AddNumberToObject(o, "initialIntensity", c->initial_intensity);
    cJSON_AddNumberToObject(o, "bpm", c->bpm);
    cJSON_AddItemToArray(arr, o);
}

static void ser_audio_listener(const JceAudioListenerComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "AudioListener");
    cJSON_AddNumberToObject(o, "volume", c->volume);
    cJSON_AddBoolToObject  (o, "paused", c->paused);
    cJSON_AddBoolToObject  (o, "spatialize", c->spatialize);
    cJSON_AddNumberToObject(o, "dopplerFactor", c->doppler_factor);
    cJSON_AddItemToArray(arr, o);
}

static void ser_audio_reverb_zone(const JceAudioReverbZoneComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "AudioReverbZone");
    cJSON_AddNumberToObject(o, "preset",            c->preset);
    cJSON_AddNumberToObject(o, "minDistance",       c->min_distance);
    cJSON_AddNumberToObject(o, "maxDistance",       c->max_distance);
    cJSON_AddNumberToObject(o, "room",              c->room);
    cJSON_AddNumberToObject(o, "roomHF",            c->room_hf);
    cJSON_AddNumberToObject(o, "decayTime",         c->decay_time);
    cJSON_AddNumberToObject(o, "decayHFRatio",      c->decay_hf_ratio);
    cJSON_AddNumberToObject(o, "reflections",       c->reflections);
    cJSON_AddNumberToObject(o, "reflectionsDelay",  c->reflections_delay);
    cJSON_AddNumberToObject(o, "reverb",            c->reverb);
    cJSON_AddNumberToObject(o, "reverbDelay",       c->reverb_delay);
    cJSON_AddNumberToObject(o, "hfReference",       c->hf_reference);
    cJSON_AddNumberToObject(o, "diffusion",         c->diffusion);
    cJSON_AddNumberToObject(o, "density",           c->density);
    cJSON_AddItemToArray(arr, o);
}

static void ser_audio_occlusion(const JceAudioOcclusionComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "AudioOcclusion");
    cJSON_AddNumberToObject(o, "radius",          c->radius);
    cJSON_AddNumberToObject(o, "attenuationDb",   c->attenuation_db);
    cJSON_AddNumberToObject(o, "lowpassCutoffHz", c->lowpass_cutoff_hz);
    cJSON_AddNumberToObject(o, "layerMask",       c->layer_mask);
    cJSON_AddBoolToObject  (o, "affectsReverb",   c->affects_reverb);
    cJSON_AddItemToArray(arr, o);
}

void serw_audio_source(JceScene *s, JceEntity e, cJSON *arr)
{
    JceAudioSourceComponent *c = jce_scene_get_audio_source(s, e);
    if (c) ser_audio_source(c, arr);
}

void serw_audio_listener(JceScene *s, JceEntity e, cJSON *arr)
{
    JceAudioListenerComponent *c = jce_scene_get_audio_listener(s, e);
    if (c) ser_audio_listener(c, arr);
}

void serw_music_track(JceScene *s, JceEntity e, cJSON *arr)
{
    JceMusicTrackComponent *c = jce_scene_get_music_track(s, e);
    if (c) ser_music_track(c, arr);
}

void serw_audio_reverb_zone(JceScene *s, JceEntity e, cJSON *arr)
{
    JceAudioReverbZoneComponent *c = jce_scene_get_audio_reverb_zone(s, e);
    if (c) ser_audio_reverb_zone(c, arr);
}

void serw_audio_occlusion(JceScene *s, JceEntity e, cJSON *arr)
{
    JceAudioOcclusionComponent *c = jce_scene_get_audio_occlusion(s, e);
    if (c) ser_audio_occlusion(c, arr);
}

