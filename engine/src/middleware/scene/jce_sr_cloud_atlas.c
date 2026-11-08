/*
 * jce_sr_cloud_atlas.c -- asynchronous cloud-density atlas lifecycle.
 *
 * CPU generation is cancellable and low priority.  GPU publication stays on
 * the render thread, where a successful upload atomically replaces the
 * previous resident texture.
 */

#include "jce_sr_internal.h"
#include "renderer/jce_cloud_noise.h"
#include "os/core/jce_half_internal.h"
#include <jce/middleware/scene/jce_scene_cloud_shape.h>
#include <jce/os/core/jce_timer.h>

#define SR_CLOUD_DIM_X       64u
#define SR_CLOUD_HIGH_DIM    128u
#define SR_CLOUD_DIM_Y       32u
#define SR_CLOUD_HIGH_DIM_Y  64u
#define SR_CLOUD_DIM_Z       32u
#define SR_CLOUD_TILES_X      8u
#define SR_CLOUD_BIAS_EPSILON 0.01f
#define SR_CLOUD_RETRY_MS   1000u

typedef struct SrCloudAtlasJob {
    JceCloudNoiseParams params;
    float              *pixels;
    uint8_t            *detail;
    bool                split;
    uint32_t            width;
    uint32_t            height;
    uint32_t            dim_x;
    uint32_t            dim_y;
    uint32_t            dim_z;
    uint64_t            generation;
    float               bake_ms;
} SrCloudAtlasJob;

struct SrCloudAtlasState {
    SrCloudAtlasJob *job;
    JceAsyncTask    *task;
    uint64_t         generation;
    uint64_t         retry_at_ms;
    JceCloudNoiseParams requested;
    JceCloudNoiseParams resident;
    uint32_t         dim_x;
    uint32_t         dim_y;
    uint32_t         dim_z;
    bool             requested_valid;
    int              capability;
    bool             split_requested;
    bool             split_disabled;
};

void sr_cloud_noise_for_scene(const JceScene *scene, float coverage, JceCloudNoiseParams *out)
{
    jce_cloud_noise_params_default(out);
    out->weather_coverage_bias = jce_cloud_bias_for_coverage(coverage);
    const JceSceneRenderingSettings *rendering = jce_scene_get_rendering_settings(scene);
    if (rendering) {
        const float bottom = rendering->cloud_bottom_km > 0.0f ? rendering->cloud_bottom_km : 1.5f;
        const float top = rendering->cloud_top_km > bottom ? rendering->cloud_top_km : 4.0f;
        if (top > bottom) out->period_y = (top - bottom) * 1000.0f;
    }
    const JceSceneCloudShapeSettings *shape = jce_scene_get_cloud_shape_settings(scene);
    if (shape) {
        out->detail_strength = shape->detail_erosion;
        out->curl_metres = shape->curl_metres;
    }
}

bool sr_cloud_shape_equal(const JceCloudNoiseParams *a, const JceCloudNoiseParams *b)
{
    return a->detail_strength == b->detail_strength && a->curl_metres == b->curl_metres &&
           a->period_y == b->period_y;
}

static bool sr_cloud_atlas_key_equal(const JceCloudNoiseParams *a, const JceCloudNoiseParams *b)
{
    return fabsf(a->weather_coverage_bias - b->weather_coverage_bias) < SR_CLOUD_BIAS_EPSILON &&
           sr_cloud_shape_equal(a, b);
}

bool sr_cloud_atlas_shadow_compatible(const JceSceneRenderer *sr)
{
    const struct SrCloudAtlasState *state = sr->cloud_atlas_state;
    return !state || !state->requested_valid || !sr->cloud_atlas_ready ||
           sr_cloud_shape_equal(&state->resident, &sr->cloud_shadow_noise);
}

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

    if (job->detail) {
        baked = jce_cloud_detail_bake_bands_controlled(&job->params, job->detail,
            JCE_CLOUD_DETAIL_DIM, SR_CLOUD_TILES_X, sr_cloud_atlas_bake_progress, ctx);
        if (!baked) return jce_async_context_cancel_requested(ctx)
            ? JCE_ASYNC_RUN_CANCELLED : JCE_ASYNC_RUN_FAILED;
        LOG_INFO(LOG_TAG, "sky: cloud detail bake complete (generation=%llu, %ux%ux%u, bytes=%u, bands=3)",
            (unsigned long long)job->generation, JCE_CLOUD_DETAIL_DIM, JCE_CLOUD_DETAIL_DIM,
            JCE_CLOUD_DETAIL_DIM, (unsigned)(JCE_CLOUD_DETAIL_DIM * JCE_CLOUD_DETAIL_DIM *
                JCE_CLOUD_DETAIL_DIM * 4u));
    }
    baked = job->split
        ? jce_cloud_shape_bake_atlas_controlled(&job->params, job->pixels,
            job->dim_x, job->dim_y, job->dim_z, SR_CLOUD_TILES_X,
            sr_cloud_atlas_bake_progress, ctx)
        : jce_cloud_noise_bake_atlas_controlled(&job->params, job->pixels,
            job->dim_x, job->dim_y, job->dim_z, SR_CLOUD_TILES_X,
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
    JCE_FREE(job->detail);
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

static bgfx_texture_format_t sr_cloud_atlas_format(const JceSceneRenderer *sr)
{
    return sr->sky_legacy_gl21 ? BGFX_TEXTURE_FORMAT_RGBA8 : BGFX_TEXTURE_FORMAT_R32F;
}

static const bgfx_memory_t *sr_cloud_atlas_stage(const SrCloudAtlasJob *job,
    bgfx_texture_format_t format)
{
    const uint32_t count = job->width * job->height;
    if (format == BGFX_TEXTURE_FORMAT_RG16F) {
        const bgfx_memory_t *mem = bgfx_alloc(count * 4u);
        if (!mem) return NULL;
        uint16_t *halves = (uint16_t *)mem->data;
        for (uint32_t i = 0; i < count * 2u; ++i) halves[i] = jce_float_to_half(job->pixels[i]);
        return mem;
    }
    if (format == BGFX_TEXTURE_FORMAT_R32F)
        return bgfx_copy(job->pixels, count * (uint32_t)sizeof(float));

    const bgfx_memory_t *mem = bgfx_alloc(count * 4u);
    if (!mem) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const float density = job->pixels[i];
        const float clamped = !(density > 0.0f) ? 0.0f : density > 1.0f ? 1.0f : density;
        uint8_t *texel = mem->data + i * 4u;
        texel[0] = (uint8_t)(clamped * 255.0f + 0.5f);
        texel[1] = 0u;
        texel[2] = 0u;
        texel[3] = 255u;
    }
    return mem;
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
        sr_cloud_atlas_key_equal(&job->params, &state->requested) &&
        job->split == state->split_requested) {
        if (job->detail && !BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex)) {
            const uint32_t bytes = JCE_CLOUD_DETAIL_DIM * JCE_CLOUD_DETAIL_DIM * JCE_CLOUD_DETAIL_DIM * 4u;
            const bgfx_memory_t *detail_mem = bgfx_copy(job->detail, bytes);
            if (detail_mem) sr->cloud_detail_tex = bgfx_create_texture_2d(512u, 512u, false, 1,
                BGFX_TEXTURE_FORMAT_RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, detail_mem, 0);
            if (BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex))
                LOG_INFO(LOG_TAG, "sky: cloud detail resident (generation=%llu, %ux%ux%u, bytes=%u, handle=%u, bands=3)",
                    (unsigned long long)job->generation, JCE_CLOUD_DETAIL_DIM, JCE_CLOUD_DETAIL_DIM,
                    JCE_CLOUD_DETAIL_DIM, (unsigned)bytes, (unsigned)sr->cloud_detail_tex.idx);
        }
        const bool split_ready = !job->split || BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex);
        const bgfx_texture_format_t format = job->split ? BGFX_TEXTURE_FORMAT_RG16F : sr_cloud_atlas_format(sr);
        const bgfx_memory_t *mem = split_ready ? sr_cloud_atlas_stage(job, format) : NULL;
        bgfx_texture_handle_t next = {UINT16_MAX};
        if (mem) {
            next = bgfx_create_texture_2d(
                (uint16_t)job->width, (uint16_t)job->height, false, 1, format,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP, mem, 0);
        }

        if (BGFX_HANDLE_IS_VALID(next)) {
            if (BGFX_HANDLE_IS_VALID(sr->cloud_atlas_tex))
                bgfx_destroy_texture(sr->cloud_atlas_tex);
            sr->cloud_atlas_tex = next;
            sr->cloud_atlas_ready = true;
            sr->cloud_shape_params[0] = job->split ? 1.0f : 0.0f;
            sr->cloud_shape_params[1] = job->params.detail_strength;
            sr->cloud_shape_params[2] = job->params.curl_metres;
            sr->cloud_shape_params[3] = job->params.period_y;
            state->resident = job->params;
            sr->cloud_atlas_dims[0] = (float)job->dim_x;
            sr->cloud_atlas_dims[1] = (float)job->dim_y;
            sr->cloud_atlas_dims[2] = (float)job->dim_z;
            sr->cloud_atlas_dims[3] = (float)SR_CLOUD_TILES_X;
            state->retry_at_ms = 0u;
            LOG_INFO(LOG_TAG,
                     "sky: cloud atlas ready (async generation=%llu, "
                     "bias=%.3f, erosion=%.3f, curl=%.1f m, %ux%ux%u, split=%d, %.1f ms)",
                     (unsigned long long)job->generation,
                     (double)job->params.weather_coverage_bias,
                     (double)job->params.detail_strength, (double)job->params.curl_metres,
                     job->dim_x, job->dim_y, job->dim_z, (int)job->split,
                     (double)job->bake_ms);
        } else {
            state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
            if (job->split) state->split_disabled = true;
            LOG_WARN(LOG_TAG, "sky: cloud atlas GPU upload failed; retrying complete density fallback");
        }
    } else if (job && task_state == JCE_ASYNC_STATE_FAILED) {
        state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
        if (job->split) state->split_disabled = true;
        LOG_WARN(LOG_TAG, "sky: cloud atlas background bake failed");
    }

    sr_cloud_atlas_job_destroy(job);
}

static void sr_cloud_atlas_start(JceSceneRenderer *sr, struct SrCloudAtlasState *state,
                                 uint32_t width, uint32_t height)
{
    SrCloudAtlasJob *job;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;
    const size_t bytes = (size_t)width * (size_t)height * sizeof(float) * (state->split_requested ? 2u : 1u);

    if (state->task || jce_time_ticks_ms() < state->retry_at_ms)
        return;

    job = (SrCloudAtlasJob *)JCE_CALLOC(1, sizeof(*job));
    if (job)
        job->pixels = (float *)JCE_MALLOC(bytes);
    if (job && state->split_requested && !BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex))
        job->detail = (uint8_t *)JCE_MALLOC(JCE_CLOUD_DETAIL_DIM * JCE_CLOUD_DETAIL_DIM * JCE_CLOUD_DETAIL_DIM * 4u);
    if (!job || !job->pixels || (state->split_requested && !BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex) && !job->detail)) {
        sr_cloud_atlas_job_destroy(job);
        state->retry_at_ms = jce_time_ticks_ms() + SR_CLOUD_RETRY_MS;
        if (state->split_requested) state->split_disabled = true;
        LOG_WARN(LOG_TAG, "sky: cloud atlas allocation failed");
        return;
    }

    job->params = state->requested;
    job->split = state->split_requested;
    job->width = width;
    job->height = height;
    job->dim_x = state->dim_x;
    job->dim_y = state->dim_y;
    job->dim_z = state->dim_z;
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

bool sr_cloud_atlas_update(JceSceneRenderer *sr, const JceScene *scene, bool enabled)
{
    struct SrCloudAtlasState *state;
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

    /* The modern split field resolves 32 m horizontal / 12.5 m vertical
     * intervals for an 800 m layer. Its RG16F base occupies 4 MiB; detail is
     * unchanged. Low/medium and capability fallbacks retain 32 vertical cells.
     * Every job owns all three dimensions, so a tier or fallback transition
     * cannot reinterpret an in-flight or resident atlas. */
    const bool high = !sr->sky_legacy_gl21 && (int)jce_renderer_get_tier() >= 2;
    const bgfx_caps_t *split_caps = bgfx_get_caps();
    const bool split = high && sr->sky_mode == 4 && sr_cloud_cumulus_enabled() && !state->split_disabled && split_caps &&
        (split_caps->formats[BGFX_TEXTURE_FORMAT_RG16F] & BGFX_CAPS_FORMAT_TEXTURE_2D) &&
        (split_caps->formats[BGFX_TEXTURE_FORMAT_RGBA8] & BGFX_CAPS_FORMAT_TEXTURE_2D) &&
        split_caps->limits.maxTextureSize >= 1024u && split_caps->limits.maxTextureSamplers >= 8u;
    const uint32_t dim_x = high ? SR_CLOUD_HIGH_DIM : SR_CLOUD_DIM_X;
    const uint32_t dim_y = split ? SR_CLOUD_HIGH_DIM_Y : SR_CLOUD_DIM_Y;
    const uint32_t dim_z = high ? SR_CLOUD_HIGH_DIM : SR_CLOUD_DIM_Z;
    JceCloudNoiseParams wanted;
    sr_cloud_noise_for_scene(scene, sr->cloud_coverage, &wanted);
    if (!state->requested_valid || !sr_cloud_atlas_key_equal(&wanted, &state->requested) ||
        state->dim_x != dim_x || state->dim_y != dim_y || state->dim_z != dim_z || state->split_requested != split) {
        state->requested_valid = true;
        state->requested = wanted;
        state->dim_x = dim_x;
        state->dim_y = dim_y;
        state->dim_z = dim_z;
        state->split_requested = split;
        state->generation++;
        if (state->task)
            (void)jce_async_task_cancel(state->task);
    }

    sr_cloud_atlas_poll(sr, state);

    if (state->capability < 0) {
        const bgfx_caps_t *caps = bgfx_get_caps();
        const bgfx_texture_format_t format = sr_cloud_atlas_format(sr);
        state->capability =
            caps && (caps->formats[format] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0 ? 1 : 0;
        if (state->capability == 0) {
            LOG_WARN(LOG_TAG, "sky: %s unavailable; cloud density atlas disabled",
                sr->sky_legacy_gl21 ? "RGBA8" : "R32F");
        }
    }
    if (state->capability == 0)
        return false;

    if (!BGFX_HANDLE_IS_VALID(sr->u_sky_clouds)) {
        sr->u_sky_clouds = bgfx_create_uniform("s_sky_clouds",
                                               BGFX_UNIFORM_TYPE_SAMPLER, 1);
    }
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_detail))
        sr->u_cloud_detail = bgfx_create_uniform("s_cloud_detail", BGFX_UNIFORM_TYPE_SAMPLER, 1);
    if (!BGFX_HANDLE_IS_VALID(sr->u_cloud_shape))
        sr->u_cloud_shape = bgfx_create_uniform("u_cloud_shape", BGFX_UNIFORM_TYPE_VEC4, 1);
    if (state->split_requested && (!BGFX_HANDLE_IS_VALID(sr->u_cloud_detail) ||
        !BGFX_HANDLE_IS_VALID(sr->u_cloud_shape))) {
        state->split_disabled = true;
        return false;
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

    if (!jce_cloud_noise_atlas_size(dim_x, dim_y,
                                    dim_z, SR_CLOUD_TILES_X,
                                    &width, &height))
        return false;

    if (!state->task &&
        (!sr->cloud_atlas_ready || !sr_cloud_atlas_key_equal(&state->requested, &state->resident) ||
         sr->cloud_atlas_dims[0] != (float)dim_x || sr->cloud_atlas_dims[1] != (float)dim_y ||
         sr->cloud_atlas_dims[2] != (float)dim_z ||
         (sr->cloud_shape_params[0] > 0.5f) != split)) {
        sr_cloud_atlas_start(sr, state, width, height);
    }

    return BGFX_HANDLE_IS_VALID(sr->cloud_atlas_tex) &&
        ((sr->cloud_shape_params[0] > 0.5f) == state->split_requested) &&
        (!state->split_requested || (BGFX_HANDLE_IS_VALID(sr->cloud_detail_tex) &&
            BGFX_HANDLE_IS_VALID(sr->u_cloud_shape) && BGFX_HANDLE_IS_VALID(sr->u_cloud_detail)));
}

bool sr_cloud_atlas_is_pending(const JceSceneRenderer *sr)
{
    return sr && sr->cloud_atlas_state && sr->cloud_atlas_state->task;
}
