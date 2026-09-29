/* Legacy compatibility declarations. Active AI policy uses the public SDK
 * through es_scene_orchestrator.
 */
#ifndef ES_AID_MOOD_H
#define ES_AID_MOOD_H

#include <jce/api_app.h>
#include <jce/api_core.h>
#include <jce/api_runtime.h>
#include <jce/api.h>

#include <stdbool.h>

/* Forward decls at FILE scope: a struct tag first introduced INSIDE a
 * prototype's parameter list is scoped to that prototype only, so the stub
 * .c's own `struct JceUICanvas *` parameter became a SECOND, incompatible
 * type — "conflicting types for es_aid_mood_ui" on clang/wasm (MSVC is
 * lenient).  Declaring the tags here gives prototype and definition the
 * same type. */
struct JceUICanvas;
struct JceInput;
struct JceRenderer;
struct JceWindow;
struct JceCamera;

typedef struct EsAidMood EsAidMood;

/* NULL when the SDK has no ai_dispatch module (stub build) — the app
 * treats that as "feature absent", not an error. */
EsAidMood *es_aid_mood_create(JceRuntime *runtime, JceScene *scene);
void       es_aid_mood_destroy(EsAidMood *m);

/* Per-frame: key handling (G/O/F via the given input), pending-result
 * bookkeeping and the autotest state machine.  `input` may be NULL. */
void es_aid_mood_update(EsAidMood *m, float dt, const struct JceInput *input);

/* True while an AI request is in flight (sent, not yet applied).  The app
 * uses this to throttle its render loop during generation: the LLM call runs
 * on a worker thread, so an uncapped render loop would otherwise steal GPU
 * cycles the local inference server needs, roughly halving tokens/sec.  Yields
 * the GPU for the ~6 s wait; false (full speed) the rest of the time. */
bool es_aid_mood_is_generating(const EsAidMood *m);

/* Per-frame UI tick (call AFTER es_aid_mood_update, with the app's live
 * UI canvas).  Builds the on-screen panel on first call, then polls the
 * "generate" + style buttons (pure C, entity-id switch), drives the
 * progress bar and status label.  No-op if uc is NULL or the module is
 * a stub. */
void es_aid_mood_ui(EsAidMood *m, struct JceUICanvas *uc, float dt);

/* Per-frame debug overlay (ES_AID_DEBUG_VIZ=1): draws the collision footprints,
 * pond-exclusion and ground rings into a 3D overlay pass on the backbuffer, so
 * the AI placement/dispatch process is visible.  Call in the draw phase, AFTER
 * the scene+postfx have presented.  No-op unless the viz env is set. */
void es_aid_mood_debug_draw(EsAidMood *m, struct JceRenderer *renderer,
                            struct JceWindow *window, const struct JceCamera *cam);

#endif /* ES_AID_MOOD_H */
