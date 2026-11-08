/* Native public director -> serialized plan. Lua owns actual apply/ack/UI. */
#include "es_aid_mood.h"
#include "es_scene_orchestrator.h"
#include <math.h>
#include <string.h>
struct EsAidMood {
    JceScene *scene;
    EsSceneOrchestrator *orchestrator;
    JceEntity request, plan, provider;
    uint32_t last_sequence, active_sequence;
};
static JceEntity unique_node(JceScene *scene, const char *name)
{
    JceEntity found[2];
    return jce_scene_query_by_name(scene, name, found, 2) == 1 ? found[0] : 0;
}
static bool write_node(EsAidMood *mood, JceEntity entity, float x, float y, float z)
{
    JceTransform *current = jce_scene_get_transform(mood->scene, entity);
    JceTransform next;
    if (!current) return false;
    next = *current;
    next.position.x = x; next.position.y = y; next.position.z = z;
    jce_scene_set_transform(mood->scene, entity, &next);
    return true;
}
EsAidMood *es_aid_mood_create(JceRuntime *runtime, JceScene *scene,
                             const JceFileSystem *filesystem)
{
    EsAidMood *mood;
    if (!runtime || !scene || !filesystem) return NULL;
    mood = (EsAidMood *)jce_malloc(sizeof *mood);
    if (!mood) return NULL;
    memset(mood, 0, sizeof *mood); mood->scene = scene;
    mood->request = unique_node(scene, "MoodRequest");
    mood->plan = unique_node(scene, "MoodPlan");
    mood->provider = unique_node(scene, "MoodProvider");
    if (!mood->request || !mood->plan || !mood->provider ||
        !jce_scene_has_transform(scene, mood->request) ||
        !jce_scene_has_transform(scene, mood->plan) ||
        !jce_scene_has_transform(scene, mood->provider)) {
        LOG_WARN("es_mood", "%s", "native UNAVAILABLE: stage=serialized_nodes; preset fallback remains");
        jce_free(mood); return NULL;
    }
    mood->orchestrator = es_scene_orchestrator_create(runtime, filesystem);
    if (!mood->orchestrator) {
        write_node(mood, mood->provider, -1, 1, 0);
        LOG_WARN("es_mood", "%s", "native UNAVAILABLE: stage=director_bootstrap; preset fallback remains");
        jce_free(mood); return NULL;
    }
    write_node(mood, mood->provider, 1, 0, 0);
    LOG_INFO("es_mood", "%s", "native available: public director offline bootstrap; Lua apply/ack");
    return mood;
}
void es_aid_mood_update(EsAidMood *mood, float delta_seconds)
{
    JceTransform *request;
    int state = 0, error = 0, result;
    float sequence;
    if (!mood) return;
    request = jce_scene_get_transform(mood->scene, mood->request);
    if (!request) return;
    sequence = request->position.x;
    if (isfinite(sequence) && sequence >= 1 && sequence <= 16777215 &&
        floorf(sequence) == sequence && (uint32_t)sequence > mood->last_sequence) {
        mood->last_sequence = (uint32_t)sequence;
        if (request->position.z == 1) {
            if (request->position.y != -1 || mood->active_sequence ||
                !es_scene_orchestrator_request_next(mood->orchestrator)) {
                write_node(mood, mood->plan, sequence, -4, -1);
                LOG_WARN("es_mood", "request=%u FAILED: stage=admission", mood->last_sequence);
            } else {
                mood->active_sequence = mood->last_sequence;
                write_node(mood, mood->plan, sequence, 0, 0);
            }
        }
    }
    es_scene_orchestrator_update(mood->orchestrator, delta_seconds);
    result = es_scene_orchestrator_take_result(mood->orchestrator, &state, &error);
    if (result && mood->active_sequence) {
        write_node(mood, mood->plan, (float)mood->active_sequence,
                    (float)(result == 1 ? state : error), (float)result);
        mood->active_sequence = 0;
    }
}
void es_aid_mood_destroy(EsAidMood *mood)
{
    if (!mood) return;
    write_node(mood, mood->provider, 0, 0, 0);
    es_scene_orchestrator_destroy(mood->orchestrator);
    jce_free(mood);
}
