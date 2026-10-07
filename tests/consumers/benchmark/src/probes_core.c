#include "probes.h"
#include <jce/api_scene.h>
#include <jce/api_resource.h>
#include <math.h>
#include <string.h>

#define LOG_TAG "benchmark_probe"
#define PROBE_WARMUP 2u

static uint64_t job_value(uint32_t index)
{
    uint64_t value = (uint64_t)index + 1u;
    for (uint32_t i = 0; i < 64u; ++i) {
        value ^= value >> 12;
        value ^= value << 25;
        value ^= value >> 27;
        value *= UINT64_C(2685821657736338717);
    }
    return value;
}

static void job_range(uint32_t begin, uint32_t end, void *user)
{
    uint64_t *output = user;
    for (uint32_t i = begin; i < end; ++i) output[i] = job_value(i);
}

static bool jobs(const BenchProbeConfig *config)
{
    uint64_t *output = jce_malloc((size_t)config->count * sizeof(*output));
    JceThreadPool *pool = jce_thread_pool_create((int)config->concurrency);
    if (!output || !pool) {
        jce_free(output);
        if (pool) jce_thread_pool_destroy(pool);
        return false;
    }
    uint64_t expected = 0;
    for (uint32_t i = 0; i < config->count; ++i) expected += job_value(i);
    LOG_INFO(LOG_TAG, "benchmark-workers: participants=%u background=%d chunk=256",
             config->concurrency, jce_thread_pool_worker_count(pool));
    bool correct = true;
    for (uint32_t round = 0; round < config->rounds + PROBE_WARMUP; ++round) {
        /* A missing task cannot pass by reading the preceding iteration. */
        memset(output, 0, (size_t)config->count * sizeof(*output));
        uint64_t started = jce_time_perf_counter();
        jce_thread_pool_parallel_for(pool, config->count, 256u, job_range, output);
        uint64_t ended = jce_time_perf_counter();
        uint64_t checksum = 0;
        for (uint32_t i = 0; i < config->count; ++i) {
            correct = correct && output[i] == job_value(i);
            checksum += output[i];
        }
        correct = correct && checksum == expected;
        if (round >= PROBE_WARMUP)
            bench_probe_sample("jobs", config, round - PROBE_WARMUP,
                               started, ended, config->count, checksum, correct);
        if (!correct) break;
    }
    jce_thread_pool_destroy(pool);
    jce_free(output);
    return correct;
}

static JceTransform transform_at(float x)
{
    JceTransform transform = {0};
    transform.position = jce_v3(x, 0, 0);
    transform.rotation = jce_q_identity();
    transform.scale = jce_v3(1, 1, 1);
    return transform;
}

static bool transforms(const char *name, const BenchProbeConfig *config)
{
    JceScene *scene = jce_scene_create();
    JceEntity *entities = jce_malloc((size_t)config->count * sizeof(*entities));
    const bool hierarchy = !strcmp(name, "hierarchy");
    bool correct = scene && entities;
    JceEntity root = scene ? jce_scene_create_entity(scene, "ProbeRoot") : 0;
    correct = correct && root != 0;
    for (uint32_t i = 0; correct && i < config->count; ++i) {
        entities[i] = jce_scene_create_entity(scene, "ProbeTransform");
        correct = entities[i] != 0;
        JceTransform transform = transform_at(1);
        jce_scene_set_transform(scene, entities[i], &transform);
        if (hierarchy)
            jce_scene_set_parent(scene, entities[i], i % config->depth == 0
                                 ? root : entities[i - 1]);
    }
    for (uint32_t round = 0; correct && round < config->rounds + PROBE_WARMUP; ++round) {
        uint64_t started = jce_time_perf_counter();
        if (hierarchy) {
            JceTransform root_transform = transform_at((float)round);
            jce_scene_set_transform(scene, root, &root_transform);
        } else {
            for (uint32_t i = 0; i < config->count; ++i) {
                JceTransform transform = transform_at((float)round + 1);
                jce_scene_set_transform(scene, entities[i], &transform);
            }
        }
        double sum = 0;
        uint64_t completed = 0;
        for (uint32_t i = 0; i < config->count; ++i) {
            jce_mat4 matrix = jce_scene_get_world_matrix(scene, entities[i]);
            const float x = JCE_M4_PTR(matrix)[12];
            const float expected = (float)round + (hierarchy ? i % config->depth + 1u : 1u);
            correct = correct && isfinite(x) && fabsf(x - expected) < 0.0001f;
            sum += x;
            ++completed;
        }
        uint64_t ended = jce_time_perf_counter();
        if (round >= PROBE_WARMUP)
            bench_probe_sample(name, config, round - PROBE_WARMUP, started,
                               ended, completed, (uint64_t)sum, correct);
    }
    jce_free(entities);
    if (scene) jce_scene_destroy(scene);
    return correct;
}

static bool scene_load(const BenchProbeConfig *config)
{
    JceScene *source = jce_scene_create();
    if (!source) return false;
    bool correct = true;
    for (uint32_t i = 0; i < config->count; ++i) {
        JceEntity entity = jce_scene_create_entity(source, "StreamedTransform");
        if (!entity) { correct = false; break; }
        JceTransform transform = transform_at((float)i);
        jce_scene_set_transform(source, entity, &transform);
    }
    size_t length = 0;
    char *text = correct ? jce_scene_serial_save(source, &length) : NULL;
    JceJson *json = text ? jce_json_parse(text, length) : NULL;
    jce_scene_serial_free(text);
    jce_scene_destroy(source);
    if (!json) return false;
    for (uint32_t round = 0; correct && round < config->rounds + PROBE_WARMUP; ++round) {
        JceScene *scene = jce_scene_create();
        int total = 0;
        uint64_t started = jce_time_perf_counter();
        JceSceneLoadStream *stream = scene ? jce_scene_load_stream_begin(scene, json, &total) : NULL;
        correct = stream && total == (int)config->count;
        uint64_t slices = 0;
        if (stream) {
            while (correct && !jce_scene_load_stream_done(stream)) {
                int before = jce_scene_load_stream_processed(stream);
                int after = jce_scene_load_stream_step(stream, 256);
                correct = after > before && after - before <= 256;
                ++slices;
            }
            uint32_t completed = jce_scene_load_stream_new_entities(stream, NULL, 0);
            int finalized = jce_scene_load_stream_finalize(stream);
            correct = correct && completed == config->count && finalized == total;
            uint64_t ended = jce_time_perf_counter();
            if (round >= PROBE_WARMUP)
                bench_probe_sample("scene-load", config, round - PROBE_WARMUP,
                                   started, ended, completed, slices, correct);
        }
        if (scene) jce_scene_destroy(scene);
    }
    jce_json_free(json);
    return correct;
}

bool bench_probe_core(const char *name, const BenchProbeConfig *config)
{
    if (!strcmp(name, "jobs")) return jobs(config);
    if (!strcmp(name, "scene-load")) return scene_load(config);
    return transforms(name, config);
}
