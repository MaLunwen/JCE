/* Reusable diagnostic scenes for editor, SDK consumers and headless probes. */
#ifndef JCE_SCENE_STRESS_H
#define JCE_SCENE_STRESS_H

#include <jce/middleware/scene/jce_scene.h>

JCE_EXTERN_C_BEGIN

typedef enum JceSceneStressKind {
    JCE_SCENE_STRESS_DRAWCALL = 0,
    JCE_SCENE_STRESS_INSTANCING = 1,
    JCE_SCENE_STRESS_TRIANGLE = 2,
    JCE_SCENE_STRESS_ENTITY = 3,
    JCE_SCENE_STRESS_PHYSICS = 4
} JceSceneStressKind;

typedef struct JceSceneStressDesc {
    JceSceneStressKind kind;
    uint32_t count;
    float spread; /* 0: dense grid; positive: ground-plane extent */
} JceSceneStressDesc;

typedef struct JceSceneStressResult {
    JceEntity root;
    uint32_t spawned; /* actual entities or scatter instances, never requested */
} JceSceneStressResult;

/* Adds an owned subtree without touching existing objects or host cameras.
 * Destroy result.root to release it. Failure rolls back its subtree and zeros
 * the result. Primitive scatter uses deterministic chunks of at most
 * JCE_FOLIAGE_MAX_INSTANCES per component; result counts their actual sum.
 * Requests above two million fail rather than silently clamping the workload.
 * This generator does not create a runtime, start motion or select settings. */
JCE_API bool jce_scene_stress_spawn(JceScene *scene,
                                   const JceSceneStressDesc *desc,
                                   JceSceneStressResult *result);

/* Enables bounded transform integration for this scene's strict descendants
 * of root. The root, cameras and character controllers remain anchored.
 * Disable with enabled=false. State and motion phase belong to the scene;
 * another scene cannot enable, disable or advance this scene's workload. */
JCE_API void jce_scene_stress_motion(JceScene *scene, JceEntity root, bool enabled);

JCE_EXTERN_C_END
#endif /* JCE_SCENE_STRESS_H */
