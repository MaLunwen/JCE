/* Sample existing SDK counters; definitions remain engine-owned. */
#include "metrics.h"
#include <jce/api_core.h>
#include <jce/api_introspect.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_player"

static void optional_bytes(JceJson *object, const char *key, const char *available, int64_t value)
{
    jce_json_set_bool(object, available, value >= 0);
    if (value >= 0) jce_json_set_number(object, key, (double)value);
}

static void renderer_counters(JceSceneRenderer *renderer, JceJson *root)
{
    JceSceneCullStats cull;
    jce_scene_renderer_get_cull_stats(renderer, &cull);
    JceJson *object = jce_json_object();
    jce_json_set_bool(object, "enabled", cull.enabled);
    jce_json_set_number(object, "candidates", cull.total);
    jce_json_set_number(object, "visible", cull.visible);
    jce_json_set_number(object, "culled", cull.culled);
    jce_json_set_number(object, "grid_objects", cull.grid_objects);
    jce_json_set_number(object, "inserted", cull.inserted);
    jce_json_set_number(object, "updated", cull.updated);
    jce_json_set_number(object, "removed", cull.removed);
    jce_json_set_number(object, "disabled_skipped", cull.disabled_skipped);
    jce_json_set_child(root, "entity_culling", object);

    JceSceneLodStats lod;
    jce_scene_renderer_get_lod_stats(renderer, &lod);
    object = jce_json_object();
    jce_json_set_bool(object, "enabled", lod.enabled);
    jce_json_set_number(object, "level_count", lod.level_count);
    jce_json_set_number(object, "culled", lod.culled);
    JceJson *levels = jce_json_array();
    for (int i = 0; i < JCE_SCENE_LOD_MAX_LEVELS; ++i)
        jce_json_array_push_number(levels, lod.picks[i]);
    jce_json_set_child(object, "picks", levels);
    jce_json_set_child(root, "model_lod", object);

    JceSceneGpuDrivenStats gpu;
    jce_scene_renderer_get_gpu_driven_stats(renderer, &gpu);
    object = jce_json_object();
    jce_json_set_string(object, "scope", "model batches; not foliage dispatches");
    jce_json_set_bool(object, "enabled", gpu.enabled);
    jce_json_set_bool(object, "active", gpu.active);
    jce_json_set_bool(object, "adaptive_bypass", gpu.adaptive_bypass);
    jce_json_set_bool(object, "forced", gpu.forced);
    jce_json_set_bool(object, "supported", gpu.supported);
    jce_json_set_bool(object, "indirect_supported", gpu.indirect_supported);
    jce_json_set_bool(object, "indirect_ready", gpu.indirect_ready);
    jce_json_set_bool(object, "dispatch_succeeded", gpu.dispatch_succeeded);
    jce_json_set_bool(object, "hiz_enabled", gpu.hiz_enabled);
    jce_json_set_number(object, "candidate_records", gpu.candidate_records);
    jce_json_set_number(object, "candidate_groups", gpu.candidate_groups);
    jce_json_set_number(object, "candidate_draws", gpu.candidate_draws);
    jce_json_set_number(object, "input_records", gpu.records);
    jce_json_set_number(object, "gpu_groups", gpu.gpu_groups);
    jce_json_set_number(object, "gpu_runs", gpu.gpu_runs);
    jce_json_set_number(object, "compute_dispatches", gpu.compute_dispatches);
    jce_json_set_number(object, "indirect_submits", gpu.indirect_submits);
    jce_json_set_number(object, "fixed_count_submits", gpu.fixed_count_submits);
    jce_json_set_number(object, "cpu_fallback_submits", gpu.cpu_fallback_submits);
    jce_json_set_number(object, "buffer_growths", gpu.buffer_growths);
    jce_json_set_number(object, "upload_calls", gpu.upload_calls);
    jce_json_set_number(object, "uploaded_bytes", (double)gpu.uploaded_bytes);
    jce_json_set_number(object, "sort_ms", gpu.sort_ms);
    jce_json_set_number(object, "prepare_ms", gpu.prepare_ms);
    jce_json_set_number(object, "flush_ms", gpu.flush_ms);
    jce_json_set_child(root, "model_gpu_driven", object);
}

void bench_metrics_sample(JceScene *scene, JceSceneRenderer *renderer,
                          unsigned frame, double simulation_seconds, const char *event)
{
    const char *path = getenv("JCE_BENCH_COUNTERS");
    if (!path || !path[0]) return;
    char stack[16384];
    char *bytes = stack;
    size_t capacity = sizeof(stack);
    size_t needed = jce_introspect_stats_json(scene, bytes, capacity);
    if (needed >= capacity && needed < SIZE_MAX - 1024u) {
        capacity = needed + 1024u;
        bytes = jce_malloc(capacity);
        if (bytes) needed = jce_introspect_stats_json(scene, bytes, capacity);
    }
    if (!bytes || needed >= capacity) {
        if (bytes != stack) jce_free(bytes);
        LOG_ERROR(LOG_TAG, "benchmark counters could not be sampled");
        return;
    }
    JceJson *stats = jce_json_parse_strict(bytes, needed);
    if (bytes != stack) jce_free(bytes);
    JceJson *root = jce_json_object();
    if (!stats || !root) {
        jce_json_free(stats);
        jce_json_free(root);
        LOG_ERROR(LOG_TAG, "benchmark counters could not be encoded");
        return;
    }
    jce_json_set_number(root, "frame", frame);
    jce_json_set_number(root, "simulation_seconds", simulation_seconds);
    jce_json_set_number(root, "monotonic_ms", (double)jce_time_ticks_ms());
    jce_json_set_string(root, "event", event);
    jce_json_set_child(root, "engine", stats);
    /* Introspection memory totals count explicitly tagged allocations only.
     * Zero is not evidence that the process has no heap or no leaks. */
    jce_json_set_string(root, "engine_memory_scope", "explicit tagged allocations only");
    JceMemStats memory;
    if (jce_mem_stats(&memory)) {
        JceJson *process = jce_json_object();
        jce_json_set_number(process, "rss_bytes", (double)memory.current_rss);
        jce_json_set_number(process, "peak_rss_bytes", (double)memory.peak_rss);
        jce_json_set_number(process, "committed_bytes", (double)memory.current_commit);
        jce_json_set_number(process, "peak_committed_bytes", (double)memory.peak_commit);
        jce_json_set_string(process, "scope", "mimalloc process snapshot; not live heap accounting");
        jce_json_set_child(root, "process_memory", process);
    }
    JceGpuStats gpu;
    bool available = jce_renderer_get_gpu_stats(&gpu);
    jce_json_set_bool(root, "gpu_available", available);
    if (available) {
        JceJson *counters = jce_json_object();
        jce_json_set_number(counters, "draws", gpu.num_draw);
        jce_json_set_number(counters, "textures", gpu.num_textures);
        jce_json_set_number(counters, "frame_buffers", gpu.num_frame_buffers);
        jce_json_set_number(counters, "programs", gpu.num_programs);
        jce_json_set_number(counters, "shaders", gpu.num_shaders);
        jce_json_set_string(counters, "gpu_memory_scope",
            "backend-reported; may be adapter-wide, not process-owned live allocations");
        optional_bytes(counters, "render_target_bytes", "render_target_bytes_available", gpu.rt_memory_used);
        optional_bytes(counters, "texture_bytes", "texture_bytes_available", gpu.texture_memory_used);
        optional_bytes(counters, "gpu_bytes", "gpu_bytes_available", gpu.gpu_memory_used);
        jce_json_set_child(root, "gpu", counters);
    }
    if (renderer && frame) {
        renderer_counters(renderer, root);
        JceSceneRqStats queue;
        jce_scene_renderer_get_rq_stats(renderer, &queue);
        JceJson *counters = jce_json_object();
        jce_json_set_bool(counters, "enabled", queue.enabled);
        jce_json_set_number(counters, "commands", queue.commands_in);
        jce_json_set_number(counters, "submits", queue.submits_out);
        jce_json_set_number(counters, "batches_merged", queue.batches_merged);
        jce_json_set_number(counters, "instances", queue.instances_total);
        jce_json_set_child(root, "render_queue", counters);
    }
    char *text = jce_json_print(root, false);
    if (text) {
        size_t length = strlen(text);
        /* Replace the owned terminator with LF and write the complete record
         * in one SDK operation. Logger messages have a bounded ring size. */
        text[length] = '\n';
        bool written = frame == 0 ? jce_fs_host_write_all(path, text, length + 1u)
                                  : jce_fs_host_append(path, text, length + 1u);
        if (written) LOG_INFO(LOG_TAG, "benchmark-counters: saved frame=%u event=%s", frame, event);
        else LOG_ERROR(LOG_TAG, "benchmark counters could not be written");
    } else LOG_ERROR(LOG_TAG, "benchmark counters could not be serialized");
    jce_json_free_string(text);
    jce_json_free(root);
}
