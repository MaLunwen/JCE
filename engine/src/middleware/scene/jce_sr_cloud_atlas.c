/*
 * jce_sr_cloud_atlas.c -- asynchronous cloud-density atlas lifecycle.
 *
 * CPU generation is cancellable and low priority.  GPU publication stays on
 * the render thread, where a successful upload atomically replaces the
 * previous resident texture.
 */

#include "jce_sr_internal.h"
#include "renderer/jce_cloud_noise.h"
#include <jce/os/core/jce_timer.h>

#define SR_CLOUD_DIM_X       64u
#define SR_CLOUD_DIM_Y       32u
#define SR_CLOUD_DIM_Z       32u
#define SR_CLOUD_TILES_X      8u
#define SR_CLOUD_BIAS_EPSILON 0.01f
#define SR_CLOUD_RETRY_MS   1000u

typedef struct SrCloudAtlasJob {
    JceCloudNoiseParams params;
    float              *pixels;
    uint32_t            width;
    uint32_t            height;
    float               bias;
    uint64_t            generation;
    float               bake_ms;
} SrCloudAtlasJob;

struct SrCloudAtlasState {
    SrCloudAtlasJob *job;
    JceAsyncTask    *task;
    uint64_t         generation;
    uint64_t         retry_at_ms;
    float            requested_bias;
    bool             requested_valid;
    int              capability;
};

static struct SrCloudAtlasState *sr_cloud_atlas_state_get(
    JceSceneRenderer *sr)
{
    struct SrCloudAtlasState *state;

    if (sr->cloud_atlas_state)
        return sr->cloud_atlas_state;

    state = (struct SrCloudAtlasState *)JCE_CALLOC(1, sizeof(*state));
    if (!state)
        return NULL;
    state->capability = -1;
    sr->cloud_atlas_state = state;
    return state;
}

static bool sr_cloud_atlas_bake_progress(void *user, uint32_t completed,
                                         uint32_t total)
{
    JceAsyncContext *ctx = (JceAsyncContext *)user;
    const float progress = total > 0u
        ? (float)completed / (float)total : 1.0f;

    jce_async_context_set_progress(ctx, progress);
    return !jce_async_context_cancel_requested(ctx);
}

static JceAsyncRunResult sr_cloud_atlas_worker(JceAsyncContext *ctx, void *arg)
{
    SrCloudAtlasJob *job = (SrCloudAtlasJob *)arg;
    const uint64_t started = jce_time_perf_counter();
    bool baked;

    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;

    baked = jce_cloud_noise_bake_atlas_controlled(
        &job->params, job->pixels,
        SR_CLOUD_DIM_X, SR_CLOUD_DIM_Y, SR_CLOUD_DIM_Z, SR_CLOUD_TILES_X,
        sr_cloud_atlas_bake_progress, ctx);
    job->bake_ms = (float)jce_time_perf_to_ms(started,
                                               jce_time_perf_counter());

    if (!baked) {
        if (jce_async_context_cancel_requested(ctx))
            return JCE_ASYNC_RUN_CANCELLED;
        jce_async_context_fail(ctx, -1, "cloud atlas CPU bake failed");
        return JCE_ASYNC_RUN_FAILED;
    }
    if (jce_async_context_cancel_requested(ctx))
        return JCE_ASYNC_RUN_CANCELLED;
    return JCE_ASYNC_RUN_SUCCESS;
}

static void sr_cloud_atlas_job_destroy(SrCloudAtlasJob *job)
{
    if (!job)
        return;
    JCE_FREE(job->pixels);
    JCE_FREE(job);
}

void sr_cloud_atlas_shutdown(JceSceneRenderer *sr)
{
    struct SrCloudAtlasState *state;

    if (!sr || !sr->cloud_atlas_state)
        return;
    state = sr->cloud_atlas_state;
    if (state->task) {
        (void)jce_async_task_discard(state->task);
    }
    sr_cloud_atlas_job_destroy(state->job);
    JCE_FREE(state);
    sr->cloud_atlas_state = NULL;
}

static void sr_cloud_atlas_poll(JceSceneRenderer *sr,
                                struct SrCloudAtlasState *state)
{
    SrCloudAtlasJob *job;
    JceAsyncState task_state;

    if (!state->task || !jce_async_task_is_terminal(state->task))
        return;

    job = state->job;
    task_state = jce_async_task_state(state->task);
    jce_async_task_release(state->task);
    state->task = NULL;
    state->job = NULL;

    if (job && task_state == JCE_ASYNC_STATE_SUCCEEDED &&
        state->requested_valid && job->generation == state->generation &&
        fabsf(job->bias - state->requested_bias) < SR_CLOUD_BIAS_EPSILON) {
        const size_t bytes = (size_t)job->width * (size_t)job->height *
                             sizeof(float);
        const bgfx_memory_t *mem = bgfx_copy(job->pixels, (uint32_t)bytes);
        bgfx_texture_handle_t next = bgfx_create_texture_2d(
            (uint16_t)job->width, (uint16_t)job->height, false, 1,
            BGFX_TEXTURE_FORMAT_R32F,
            BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);

        if (BGFX_HANDLE_IS_VALID(next)) {
            if (BGFX_HANDLE_IS_VALID(sr->cloud_atlas_tex))
                bgfx_destroy_texture(sr->cloud_atlas_tex);
            sr->cloud_atlas_tex = next;
            sr->cloud_atlas_ready = true;
            sr->cloud_atlas_bias = job->bias;
            sr->cloud_atlas_dims[0] = (float)SR_CLOUD_DIM_X;
            sr->cloud_atlas_dims[1] = (float)SR_CLOUD_DIM_Y;
            sr->cloud_atlas_dims[2] = (float)SR_CLOUD_DIM_Z;
            sr->cloud_atlas_dims[3] = (float)SR_CLOUD_TILES_X;
            state->retry_at_ms = 0u;
            LOG_INFO(LOG_TAG,
                     "sky: cloud atlas ready (async generation=%llu, "
                     "bias=%.3f, %.1f ms)",
                     (unsigned long long)job->generation,
                     (double)job->bias, (double)job->bake_ms);
        } else {
            state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
            LOG_WARN(LOG_TAG, "sky: cloud atlas GPU upload failed");
        }
    } else if (job && task_state == JCE_ASYNC_STATE_FAILED) {
        state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
        LOG_WARN(LOG_TAG, "sky: cloud atlas background bake failed");
    }

    sr_cloud_atlas_job_destroy(job);
}

static void sr_cloud_atlas_start(struct SrCloudAtlasState *state,
                                 float bias, uint32_t width, uint32_t height)
{
    SrCloudAtlasJob *job;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;
    const size_t bytes = (size_t)width * (size_t)height * sizeof(float);

    if (state->task || jce_time_ticks_ms() < state->retry_at_ms)
        return;

    job = (SrCloudAtlasJob *)JCE_CALLOC(1, sizeof(*job));
    if (job)
        job->pixels = (float *)JCE_MALLOC(bytes);
    if (!job || !job->pixels) {
        sr_cloud_atlas_job_destroy(job);
        state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
        LOG_WARN(LOG_TAG, "sky: cloud atlas allocation failed");
        return;
    }

    jce_cloud_noise_params_default(&job->params);
    job->params.weather_coverage_bias = bias;
    job->width = width;
    job->height = height;
    job->bias = bias;
    job->generation = state->generation;

    jce_async_task_desc_init(&desc);
    desc.work = sr_cloud_atlas_worker;
    desc.user_data = job;
    desc.debug_name = "scene.cloud-atlas.bake";
    desc.priority = JCE_ASYNC_PRIORITY_LOW;
    task = jce_async_submit(jce_async_default_executor(), &desc);
    if (!task) {
        sr_cloud_atlas_job_destroy(job);
        state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
        LOG_WARN(LOG_TAG, "sky: cloud atlas async queue is full");
        return;
    }

    state->job = job;
    state->task = task;
}

bool sr_cloud_atlas_update(JceSceneRenderer *sr, bool enabled)
{
    struct SrCloudAtlasState *state;
    const float want_bias = sr
        ? jce_cloud_bias_for_coverage(sr->cloud_coverage) : -1.0f;
    uint32_t width = 0u;
    uint32_t height = 0u;

    if (!sr)
        return false;

    state = sr->cloud_atlas_state;
    if (!enabled) {
        if (!state)
            return false;
        if (state->requested_valid) {
            state->requested_valid = false;
            state->generation++;
        }
        if (state->task)
            (void)jce_async_task_cancel(state->task);
        sr_cloud_atlas_poll(sr, state);
        return false;
    }

    state = sr_cloud_atlas_state_get(sr);
    if (!state)
        return false;

    if (!state->requested_valid ||
        fabsf(want_bias - state->requested_bias) >= SR_CLOUD_BIAS_EPSILON) {
        state->requested_valid = true;
        state->requested_bias = want_bias;
        state->generation++;
        if (state->task)
            (void)jce_async_task_cancel(state->task);
    }

    sr_cloud_atlas_poll(sr, state);

    if (state->capability < 0) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        state->capability =
            caps && (caps->formats[BGFX_TEXTURE_FORMAT_R32F] &
                     BGFX_CAPS_FORMAT_TEXTURE_2D) != 0 ? 1 : 0;
        if (state->capability == 0) {
            LOG_WARN(LOG_TAG,
                     "sky: R32F unavailable; volumetric clouds disabled");
        }
    }
    if (state->capability == 0)
        return false;

    if (!BGFX_HANDLE_IS_VALID(sr->u_sky_clouds)) {
        sr->u_sky_clouds = bgfx_create_uniform("s_sky_clouds",
                                               BGFX_UNIFORM_TYPE_SAMPLER, 1);
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_params)) {
        sr->u_cloud_params = bgfx_create_uniform("u_cloud_params",
                                                 BGFX_UNIFORM_TYPE_VEC4, 1);
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_atlas)) {
        sr->u_cloud_atlas = bgfx_create_uniform("u_cloud_atlas",
                                                BGFX_UNIFORM_TYPE_VEC4, 1);
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_quality)) {
        sr->u_cloud_quality = bgfx_create_uniform("u_cloud_quality",
                                                  BGFX_UNIFORM_TYPE_VEC4, 1);
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_period)) {
        sr->u_cloud_period = bgfx_create_uniform("u_cloud_period",
                                                 BGFX_UNIFORM_TYPE_VEC4, 1);
    }

    if (!jce_cloud_noise_atlas_size(SR_CLOUD_DIM_X, SR_CLOUD_DIM_Y,
                                    SR_CLOUD_DIM_Z, SR_CLOUD_TILES_X,
                                    &width, &height))
        return false;

    if (!state->task &&
        (!sr->cloud_atlas_ready ||
         fabsf(state->requested_bias - sr->cloud_atlas_bias) >=
             SR_CLOUD_BIAS_EPSILON)) {
        sr_cloud_atlas_start(state, state->requested_bias, width, height);
    }

    return BGFX_HANDLE_IS_VALID(sr->cloud_atlas_tex);
}

bool sr_cloud_atlas_is_pending(const JceSceneRenderer *sr)
{
    return sr && sr->cloud_atlas_state && sr->cloud_atlas_state->task;
}
