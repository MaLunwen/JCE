/* Elemental Serenity policy adapter for JCE AI scene orchestration. */

#ifndef ES_SCENE_ORCHESTRATOR_H
#define ES_SCENE_ORCHESTRATOR_H

#include <jce/api_app.h>
#include <jce/api_core.h>
#include <jce/api_runtime.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct EsSceneOrchestrator EsSceneOrchestrator;

EsSceneOrchestrator *es_scene_orchestrator_create(
    JceRuntime *runtime, const JceFileSystem *filesystem);
void es_scene_orchestrator_destroy(EsSceneOrchestrator *orchestrator);

bool es_scene_orchestrator_request_next(
    EsSceneOrchestrator *orchestrator);
void es_scene_orchestrator_update(EsSceneOrchestrator *orchestrator,
                                  double delta_seconds);

/* 0 pending/none, 1 validated plan, -1 failed. Consumes only the plan result;
 * applying it and acknowledging the authored state belongs to serialized Lua. */
int es_scene_orchestrator_take_result(EsSceneOrchestrator *orchestrator,
                                     int *state, int *error);

uint64_t es_scene_orchestrator_last_plan_hash(
    const EsSceneOrchestrator *orchestrator);

#endif /* ES_SCENE_ORCHESTRATOR_H */
