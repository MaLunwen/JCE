/* Optional native plan provider for the shared serialized Mood consumer. */
#ifndef ES_AID_MOOD_H
#define ES_AID_MOOD_H
#include <jce/api.h>
typedef struct EsAidMood EsAidMood;
EsAidMood *es_aid_mood_create(JceRuntime *runtime, JceScene *scene,
                             const JceFileSystem *filesystem);
void es_aid_mood_update(EsAidMood *mood, float delta_seconds);
void es_aid_mood_destroy(EsAidMood *mood);
#endif
