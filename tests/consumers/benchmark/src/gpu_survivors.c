/* Consumer-only asynchronous survivor evidence, never enabled in timings. */
#include "gpu_survivors.h"
#include <jce/api_core.h>
#include <jce/api_resource.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_gpu_survivors"
#define SNAPSHOT_VIEW 250u
#define COUNTER_READBACK_VIEW 251u

bool bench_gpu_survivors_create(BenchGpuSurvivors *state, JceRenderer *renderer)
{
    state->path = getenv("JCE_BENCH_GPU_SURVIVORS");
    if (!state->path) return true;
    if (!state->path[0] || !renderer) return false;
    state->queue = jce_render_readback_create(renderer);
    return state->queue != NULL;
}

static bool write_record(BenchGpuSurvivors *state, JceJson *root)
{
    char *text = jce_json_print(root, false);
    if (!text) return false;
    size_t size = strlen(text);
    char *line = size <= SIZE_MAX - 2u ? jce_malloc(size + 2u) : NULL;
    bool ok = false;
    if (line) {
        memcpy(line, text, size);
        line[size] = '\n';
        line[size + 1u] = '\0';
        ok = state->written ? jce_fs_host_append(state->path, line, size + 1u)
                            : jce_fs_host_write_all(state->path, line, size + 1u);
        jce_free(line);
    }
    jce_json_free_string(text);
    if (ok) state->written = true;
    return ok;
}

static bool save_ready(BenchGpuSurvivors *state, BenchGpuSurvivorRequest *request,
                       unsigned completed_frame)
{
    JceRenderReadbackInfo info;
    if (!jce_render_readback_get_info(state->queue, request->ticket, &info) ||
        info.byte_size > SIZE_MAX || info.format != JCE_RENDER_FORMAT_RGBA32F ||
        info.width != request->snapshot.width || info.height != request->snapshot.height ||
        info.byte_size != (uint64_t)info.width * info.height * 16u ||
        request->snapshot.groups > (uint64_t)info.width * info.height)
        return false;
    float *words = jce_malloc((size_t)info.byte_size);
    if (!words) return false;
    bool ok = jce_render_readback_copy(state->queue, request->ticket,
                                       words, (size_t)info.byte_size);
    uint64_t total = 0;
    JceJson *root = jce_json_object();
    JceJson *counts = jce_json_array();
    if (!root || !counts) ok = false;
    for (uint32_t i = 0; ok && i < request->snapshot.groups; ++i) {
        float low = words[4u * i], high = words[4u * i + 1u];
        if (!isfinite(low) || !isfinite(high) || low < 0 || high < 0 ||
            low > 65535 || high > 65535 || floorf(low) != low || floorf(high) != high) {
            ok = false;
            break;
        }
        uint32_t count = (uint32_t)low | ((uint32_t)high << 16);
        total += count;
        jce_json_array_push_number(counts, count);
    }
    if (total > request->snapshot.records) ok = false;
    char binary[1024], serial[32];
    int length = snprintf(binary, sizeof(binary), "%s.frame%u.bin", state->path, request->frame);
    if (length <= 0 || (size_t)length >= sizeof(binary)) ok = false;
    if (ok) ok = jce_fs_host_write_all(binary, words, (size_t)info.byte_size);
    jce_free(words);
    if (ok) {
        snprintf(serial, sizeof(serial), "%llu", (unsigned long long)request->snapshot.dispatch_serial);
        jce_json_set_string(root, "dispatch_serial", serial);
        jce_json_set_number(root, "source_frame", request->frame);
        jce_json_set_number(root, "completed_frame", completed_frame);
        jce_json_set_number(root, "ready_engine_frame", info.ready_frame);
        jce_json_set_number(root, "input_records", request->snapshot.records);
        jce_json_set_number(root, "groups", request->snapshot.groups);
        jce_json_set_number(root, "survivors", (double)total);
        jce_json_set_number(root, "width", info.width);
        jce_json_set_number(root, "height", info.height);
        jce_json_set_number(root, "byte_size", (double)info.byte_size);
        const uint16_t native_word = 1u;
        jce_json_set_string(root, "byte_order",
            *(const unsigned char *)&native_word ? "little" : "big");
        jce_json_set_string(root, "raw_words", binary);
        jce_json_set_child(root, "group_counts", counts);
        counts = NULL;
        ok = write_record(state, root);
    }
    jce_json_free(counts);
    jce_json_free(root);
    return ok;
}

static void survivor_tick(BenchGpuSurvivors *state,
                          JceSceneRenderer *renderer, JceGpuScene *scene, unsigned frame)
{
    if (!state->queue || state->failed) return;
    BenchGpuSurvivorRequest *free_slot = NULL;
    for (unsigned i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; ++i) {
        BenchGpuSurvivorRequest *request = &state->requests[i];
        if (request->ticket) {
            JceRenderReadbackState ready = jce_render_readback_poll(state->queue, request->ticket);
            if (ready == JCE_RENDER_READBACK_READY) {
                if (!save_ready(state, request, frame)) goto failed;
                jce_render_readback_release(state->queue, request->ticket);
                memset(request, 0, sizeof(*request));
            } else if (ready != JCE_RENDER_READBACK_PENDING) goto failed;
        }
        if (!request->ticket && !free_slot) free_slot = request;
    }
    bool motion = getenv("JCE_BENCH_CAMERA_MOTION") || getenv("JCE_BENCH_OCCLUSION_MOTION");
    if (frame != 20 && frame != 50 && frame != 79 && frame != 80 &&
        frame != 81 && frame != 82 && frame != 90 &&
        !(motion && (frame == 51 || frame == 66))) return;
    if (!free_slot) goto failed;
    bool captured = scene
        ? jce_gpu_scene_snapshot_survivors(scene, SNAPSHOT_VIEW, &free_slot->snapshot)
        : jce_scene_renderer_snapshot_gpu_survivors(renderer, SNAPSHOT_VIEW, &free_slot->snapshot);
    if (!captured) goto failed;
    JceRenderReadbackDesc desc = jce_render_readback_desc_default();
    desc.source = free_slot->snapshot.texture;
    desc.format = JCE_RENDER_FORMAT_RGBA32F;
    desc.view_id = COUNTER_READBACK_VIEW;
    desc.width = free_slot->snapshot.width;
    desc.height = free_slot->snapshot.height;
    free_slot->frame = frame;
    if (!jce_render_readback_submit(state->queue, &desc, &free_slot->ticket)) goto failed;
    return;
failed:
    state->failed = true;
    LOG_ERROR(LOG_TAG, "benchmark GPU survivor snapshot/readback failed frame=%u", frame);
}

void bench_gpu_survivors_tick(BenchGpuSurvivors *state,
                              JceSceneRenderer *renderer, unsigned frame)
{
    survivor_tick(state, renderer, NULL, frame);
}

void bench_gpu_survivors_tick_raw(BenchGpuSurvivors *state,
                                  JceGpuScene *scene, unsigned frame)
{
    survivor_tick(state, NULL, scene, frame);
}

void bench_gpu_survivors_destroy(BenchGpuSurvivors *state)
{
    for (unsigned i = 0; i < JCE_RENDER_READBACK_QUEUE_CAPACITY; ++i)
        if (state->requests[i].ticket)
            LOG_ERROR(LOG_TAG, "benchmark GPU survivor request incomplete source_frame=%u",
                      state->requests[i].frame);
    jce_render_readback_destroy(state->queue);
    memset(state, 0, sizeof(*state));
}
