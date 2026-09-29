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

uint64_t es_scene_orchestrator_last_plan_hash(
    const EsSceneOrchestrator *orchestrator);

#endif /* ES_SCENE_ORCHESTRATOR_H */
