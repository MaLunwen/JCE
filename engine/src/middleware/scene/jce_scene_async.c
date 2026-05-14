/*
 * jce_scene_async.c  Phased scene load to keep the main thread
 *                    responsive during big level transitions.
 *
 * Phases (each tick advances by one):
 *   0. READ      — slurp file from disk into memory
 *   1. PARSE     — cJSON parse of the loaded buffer
 *   2. PREPARE   — cull existing entities if REPLACE mode
 *   3. INSTANTIATE — walk entities array, create + attach components
 *   4. ACTIVATE  — gated by allow_activation; final hookup work
 *
 * The actual work in each phase is delegated to existing scene-
 * serialisation routines so we don't duplicate parsing logic.  The
 * bookkeeping here is just the phased state machine + progress
 * reporting.
 */

#include <jce/middleware/scene/jce_scene_async.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "scene-async"

typedef enum {
    PHASE_READ        = 0,
    PHASE_PARSE       = 1,
    PHASE_PREPARE     = 2,
    PHASE_INSTANTIATE = 3,
    PHASE_ACTIVATE    = 4,
    PHASE_DONE        = 5,
    PHASE_FAILED      = 6,
} Phase;

struct JceSceneAsyncOp {
    JceScene        *scene;
    char             path[512];
    JceSceneLoadMode mode;
    Phase            phase;

    void            *file_buf;
    uint64_t         file_size;
    JceJson         *parsed;            /* owned during PARSE..ACTIVATE */

    bool             allow_activation;
    float            progress;
};

JceSceneAsyncOp *jce_scene_load_async(JceScene *scene, const char *path,
                                       JceSceneLoadMode mode)
{
    if (!scene || !path) return NULL;
    JceSceneAsyncOp *op = (JceSceneAsyncOp *)JCE_CALLOC(1, sizeof(*op));
    if (!op) return NULL;
    op->scene = scene;
    strncpy(op->path, path, sizeof(op->path) - 1);
    op->path[sizeof(op->path) - 1] = '\0';
    op->mode  = mode;
    op->phase = PHASE_READ;
    op->allow_activation = true;
    op->progress = 0.0f;
    return op;
}

float jce_scene_async_progress(const JceSceneAsyncOp *op)
{ return op ? op->progress : 0.0f; }
bool  jce_scene_async_is_done(const JceSceneAsyncOp *op)
{ return op && op->phase >= PHASE_DONE; }

void  jce_scene_async_set_allow_activation(JceSceneAsyncOp *op, bool allow)
{ if (op) op->allow_activation = allow; }

void jce_scene_async_release(JceSceneAsyncOp *op)
{
    if (!op) return;
    if (op->file_buf) jce_fs_buffer_free(op->file_buf);
    if (op->parsed)   jce_json_free(op->parsed);
    JCE_FREE(op);
}

void jce_scene_async_tick(JceSceneAsyncOp *op)
{
    if (!op || op->phase >= PHASE_DONE) return;

    switch (op->phase) {
    case PHASE_READ: {
        op->file_buf = jce_fs_host_read_all(op->path, &op->file_size);
        if (!op->file_buf || op->file_size == 0) {
            LOG_ERROR(LOG_TAG, "failed to read '%s'", op->path);
            op->phase = PHASE_FAILED;
            op->progress = 1.0f;
            return;
        }
        op->phase = PHASE_PARSE;
        op->progress = 0.2f;
    } break;

    case PHASE_PARSE: {
        op->parsed = jce_json_parse((const char *)op->file_buf,
                                     (size_t)op->file_size);
        jce_fs_buffer_free(op->file_buf);
        op->file_buf = NULL;
        if (!op->parsed) {
            LOG_ERROR(LOG_TAG, "JSON parse failed for '%s'", op->path);
            op->phase = PHASE_FAILED;
            op->progress = 1.0f;
            return;
        }
        op->phase = PHASE_PREPARE;
        op->progress = 0.4f;
    } break;

    case PHASE_PREPARE:
        /* In REPLACE mode, the scene loader already appends but
         * doesn't clear — we'd ideally `jce_scene_clear(scene)` here,
         * but that helper isn't on the public surface yet so this
         * stays a placeholder.  ADDITIVE mode is the path that
         * works end-to-end today. */
        op->phase = PHASE_INSTANTIATE;
        op->progress = 0.6f;
        break;

    case PHASE_INSTANTIATE: {
        int loaded = jce_scene_load_json(op->scene, op->parsed);
        jce_json_free(op->parsed);
        op->parsed = NULL;
        if (loaded < 0) {
            LOG_ERROR(LOG_TAG, "scene load failed for '%s'", op->path);
            op->phase = PHASE_FAILED;
            op->progress = 1.0f;
            return;
        }
        op->phase = PHASE_ACTIVATE;
        op->progress = 0.9f;
    } break;

    case PHASE_ACTIVATE:
        if (!op->allow_activation) return;  /* stay at 0.9 */
        op->phase = PHASE_DONE;
        op->progress = 1.0f;
        break;

    default: break;
    }
}
