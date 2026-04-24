/*
 * jce_asset_cache_material.cpp  Async material extraction for mesh drops.
 */

#include "jce_asset_cache_internal.h"
#include "jce_editor_alloc.h"

static void material_extract_worker(void *arg)
{
    MaterialAsyncContext *ctx = (MaterialAsyncContext *)arg;
    if (!ctx)
        return;

    memset(&ctx->material, 0, sizeof(ctx->material));
    ctx->success = jce_editor_model_extract_material(ctx->file_path,
                                                     &ctx->material);
}

void material_async_start(void)
{
    if (s_mat_async.running)
        return;

    if (!s_mat_async.mutex) s_mat_async.mutex = jce_mutex_create();
    s_mat_async.pool = jce_thread_pool_create(2);
    s_mat_async.generation = 1;
    s_mat_async.inflight.clear();
    s_mat_async.completed.clear();
    s_mat_async.running = (s_mat_async.pool != NULL && s_mat_async.mutex != NULL);

    if (!s_mat_async.running) {
        LOG_WARN(LOG_TAG,
                 "material async disabled: failed to create thread pool");
    }
}

void material_async_stop(void)
{
    if (!s_mat_async.running)
        return;

    std::vector<MaterialInFlightTask> inflight;
    {
        JceMutexGuard lock(s_mat_async.mutex);
        inflight.swap(s_mat_async.inflight);
        s_mat_async.completed.clear();
    }

    for (MaterialInFlightTask &task : inflight) {
        if (task.task) {
            jce_task_wait(task.task);
            jce_task_free(task.task);
        }
        ED_FREE(task.context);
    }

    if (s_mat_async.pool) {
        jce_thread_pool_destroy(s_mat_async.pool);
        s_mat_async.pool = NULL;
    }

    s_mat_async.running = false;
    s_mat_async.generation = 0;

    if (s_mat_async.mutex) {
        jce_mutex_destroy(s_mat_async.mutex);
        s_mat_async.mutex = NULL;
    }
}

void material_async_begin_new_generation(void)
{
    if (!s_mat_async.running)
        return;

    JceMutexGuard lock(s_mat_async.mutex);
    s_mat_async.generation++;
    s_mat_async.completed.clear();
}

uint64_t material_async_current_generation(void)
{
    if (!s_mat_async.running)
        return 0;

    JceMutexGuard lock(s_mat_async.mutex);
    return s_mat_async.generation;
}

void material_async_queue_request(uint32_t entity_id,
                                  const char *mesh_path,
                                  const char *file_path)
{
    if (!s_mat_async.running || entity_id == 0 || !mesh_path || !mesh_path[0]
        || !file_path || !file_path[0]) {
        return;
    }

    MaterialAsyncContext *ctx =
        (MaterialAsyncContext *)ED_CALLOC(1, sizeof(MaterialAsyncContext));
    if (!ctx) {
        LOG_WARN(LOG_TAG, "material async alloc failed for entity %u", entity_id);
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->entity_id = entity_id;
    snprintf(ctx->mesh_path, sizeof(ctx->mesh_path), "%s", mesh_path);
    snprintf(ctx->file_path, sizeof(ctx->file_path), "%s", file_path);

    {
        JceMutexGuard lock(s_mat_async.mutex);
        ctx->generation = s_mat_async.generation;

        for (const MaterialInFlightTask &inflight : s_mat_async.inflight) {
            if (!inflight.context)
                continue;
            const MaterialAsyncContext *pending = inflight.context;
            if (pending->generation == ctx->generation
                && pending->entity_id == entity_id
                && strcmp(pending->mesh_path, mesh_path) == 0) {
                ED_FREE(ctx);
                return;
            }
        }
    }

    JceTask *task = jce_thread_pool_submit_tracked(s_mat_async.pool,
                                                   material_extract_worker,
                                                   ctx);
    if (!task) {
        ED_FREE(ctx);
        LOG_WARN(LOG_TAG, "material async submit failed: %s", file_path);
        return;
    }

    JceMutexGuard lock(s_mat_async.mutex);
    MaterialInFlightTask inflight = {};
    inflight.task = task;
    inflight.context = ctx;
    s_mat_async.inflight.push_back(inflight);
}

static void material_collect_completed_tasks(void)
{
    std::vector<MaterialInFlightTask> finished;
    {
        JceMutexGuard lock(s_mat_async.mutex);
        for (size_t i = 0; i < s_mat_async.inflight.size();) {
            MaterialInFlightTask &entry = s_mat_async.inflight[i];
            if (entry.task && jce_task_done(entry.task)) {
                finished.push_back(entry);
                s_mat_async.inflight.erase(
                    s_mat_async.inflight.begin()
                    + static_cast<std::vector<MaterialInFlightTask>::difference_type>(i));
            } else {
                i++;
            }
        }
    }

    if (finished.empty())
        return;

    const uint64_t generation = material_async_current_generation();
    std::vector<JceEditorMaterialExtractResult> completed;
    completed.reserve(finished.size());

    for (MaterialInFlightTask &entry : finished) {
        if (entry.task) {
            jce_task_wait(entry.task);
            jce_task_free(entry.task);
            entry.task = NULL;
        }

        MaterialAsyncContext *ctx = entry.context;
        entry.context = NULL;
        if (!ctx)
            continue;

        if (ctx->generation == generation) {
            JceEditorMaterialExtractResult result = {};
            result.entity_id = ctx->entity_id;
            snprintf(result.mesh_path, sizeof(result.mesh_path), "%s", ctx->mesh_path);
            result.success = ctx->success;
            if (ctx->success)
                result.material = ctx->material;
            completed.push_back(result);
        }

        ED_FREE(ctx);
    }

    if (!completed.empty()){
        JceMutexGuard lock(s_mat_async.mutex);
        for (JceEditorMaterialExtractResult &result : completed)
            s_mat_async.completed.push_back(result);
    }
}

void material_finalize_completed_loads(void)
{
    if (!s_mat_async.running)
        return;

    material_collect_completed_tasks();
}

bool material_take_completed_result(JceEditorMaterialExtractResult *out_result)
{
    if (!out_result || !s_mat_async.running)
        return false;

    JceMutexGuard lock(s_mat_async.mutex);
    if (s_mat_async.completed.empty())
        return false;

    *out_result = s_mat_async.completed.back();
    s_mat_async.completed.pop_back();
    return true;
}
