/*
 * jce_scene_async.h  Asynchronous scene loading (Unity SceneManager
 *                    LoadSceneAsync equivalent).
 *
 * Lets game code initiate a scene load without blocking the main
 * thread for the file IO + JSON parse + entity instantiation work.
 * The load runs in background phases (read file → parse JSON →
 * create entities → finalize); each tick advances by one phase.
 *
 * Mirrors Unity's `AsyncOperation`:
 *   - `progress` 0..1 over the whole load
 *   - `is_done`  true when the scene is in memory and ready
 *   - `allow_activation` lets caller hold the load at progress = 0.9
 *     (last 10% = activation = entities placed in the live scene)
 *
 * Layer: middleware / scene (Layer 4) — public.
 */

#ifndef JCE_SCENE_ASYNC_H
#define JCE_SCENE_ASYNC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene          JceScene;
typedef struct JceSceneAsyncOp   JceSceneAsyncOp;

typedef enum {
    JCE_SCENE_LOAD_MODE_REPLACE = 0,  /* destroy current entities first */
    JCE_SCENE_LOAD_MODE_ADDITIVE = 1, /* merge into current scene */
} JceSceneLoadMode;

/* Begin a load.  Returns NULL on failure (file missing, OOM).  The
 * op's lifetime is owned by the engine — call jce_scene_async_release
 * after `is_done` returns true to free the bookkeeping. */
JCE_API JceSceneAsyncOp *jce_scene_load_async(JceScene         *scene,
                                               const char       *path,
                                               JceSceneLoadMode  mode);

/* Tick the load by one phase.  Call once per frame.  Updates
 * `progress`; when `progress` reaches `allow_activation_threshold`
 * (default 0.9) the op pauses unless allow_activation is set. */
JCE_API void  jce_scene_async_tick(JceSceneAsyncOp *op);

JCE_API float jce_scene_async_progress(const JceSceneAsyncOp *op);
JCE_API bool  jce_scene_async_is_done (const JceSceneAsyncOp *op);

/* Permit the final-activation phase to run.  Default is true (load
 * runs to completion); setting to false lets the caller hold the
 * scene at 90% until they're ready to swap. */
JCE_API void  jce_scene_async_set_allow_activation(JceSceneAsyncOp *op,
                                                    bool allow);

/* Cancel a load and release its resources.  Safe to call on a
 * completed op too. */
JCE_API void  jce_scene_async_release(JceSceneAsyncOp *op);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_ASYNC_H */
