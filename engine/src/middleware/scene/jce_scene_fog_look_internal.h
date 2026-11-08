#ifndef JCE_SCENE_FOG_LOOK_INTERNAL_H
#define JCE_SCENE_FOG_LOOK_INTERNAL_H

#include "jce_scene_internal.h"
#include <jce/middleware/world/jce_environment.h>

typedef struct cJSON cJSON;

/* Absent/null clears the optional settings. The reader never mutates a scene. */
bool jce_scene_fog_look_read_json(const cJSON *rendering, JceSceneFogLookSettings *out);
bool jce_scene_fog_look_write_json(cJSON *fog, const JceScene *scene);
void jce_scene_fog_look_set(JceScene *scene, const JceSceneFogLookSettings *settings);
void jce_scene_fog_look_advance(JceScene *scene, const JceEnvironmentState *env, float dt);
void jce_scene_fog_look_bind(const JceScene *scene, JceVolumetricFog *fog);

float jce_scene_fog_look_screen_shafts(const JceScene *scene);

#endif
