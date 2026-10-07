#include "probes.h"
#include <jce/api_resource.h>
#include <jce/api_scene.h>
#include <jce/api_streaming.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_probe"
#define CHUNK_SPACING 100.0f
#define MAX_CHUNKS 256u
#define POINT_TIMEOUT_MS 10000u

typedef struct StreamWitness {
    JceScene *scene;
    uint32_t expected_chunk;
    uint32_t entities_per_chunk;
    uint64_t spawned;
    uint64_t despawned;
    bool correct;
} StreamWitness;

static void on_spawn(const uint64_t *ids, uint32_t count, void *user)
{
    StreamWitness *witness = user;
    witness->correct = witness->correct && count == witness->entities_per_chunk;
    for (uint32_t i = 0; witness->correct && i < count; ++i) {
        const char *name = jce_scene_entity_name(witness->scene, ids[i]);
        unsigned chunk = MAX_CHUNKS, entity = UINT32_MAX;
        witness->correct = name && sscanf(name, "Cell%uEntity%u", &chunk, &entity) == 2 &&
                           chunk == witness->expected_chunk && entity < count;
        const JceTransform *transform = jce_scene_get_transform(witness->scene, ids[i]);
        witness->correct = witness->correct && transform &&
             fabsf(transform->position.x - (CHUNK_SPACING * chunk + 0.01f * entity)) < 0.01f;
    }
    witness->spawned += count;
}

static void on_despawn(const uint64_t *ids, uint32_t count, void *user)
{
    (void)ids;
    StreamWitness *witness = user;
    witness->despawned += count;
}

static bool write_chunk(const char *root, uint32_t chunk, uint32_t count)
{
    JceScene *scene = jce_scene_create();
    if (!scene) return false;
    bool correct = true;
    for (uint32_t i = 0; correct && i < count; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "Cell%uEntity%u", chunk, i);
        JceEntity entity = jce_scene_create_entity(scene, name);
        correct = entity != 0;
        JceTransform transform = {0};
        transform.position = jce_v3(CHUNK_SPACING * chunk + 0.01f * i, 0, 0);
        transform.rotation = jce_q_identity();
        transform.scale = jce_v3(1, 1, 1);
        if (correct) jce_scene_set_transform(scene, entity, &transform);
    }
    size_t length = 0;
    char *document = correct ? jce_scene_serial_save(scene, &length) : NULL;
    char path[2048];
    int path_length = snprintf(path, sizeof(path), "%s/cell_%u.scene.json", root, chunk);
    correct = document && length && path_length > 0 && (size_t)path_length < sizeof(path) &&
              jce_fs_host_write_all(path, document, length);
    jce_scene_serial_free(document);
    jce_scene_destroy(scene);
    return correct;
}

static bool drain(JceWorldStreamer *streamer, jce_vec3 camera, uint32_t resident,
                  uint32_t entities)
{
    uint64_t started = jce_time_ticks_ms();
    do {
        jce_world_streamer_update(streamer, camera);
        if (jce_world_streamer_pending_count(streamer) == 0 &&
                jce_world_streamer_loaded_count(streamer) == resident &&
                jce_world_streamer_entity_count(streamer) == entities)
            return true;
        jce_thread_sleep_ms(1u);
    } while (jce_time_ticks_ms() - started < POINT_TIMEOUT_MS);
    return false;
}

bool bench_probe_streaming(const char *name, const BenchProbeConfig *config)
{
    const char *root = getenv("JCE_BENCH_SCRATCH");
    if (!root || !root[0] || config->count > MAX_CHUNKS ||
            (config->concurrency != 1u && config->concurrency != 4u)) return false;
    bool correct = jce_fs_host_create_directory(root);
    for (uint32_t chunk = 0; correct && chunk < config->count; ++chunk)
        correct = write_chunk(root, chunk, config->depth);
    JceFileSystem *fs = jce_fs_create();
    JceScene *scene = jce_scene_create();
    correct = correct && fs && scene && jce_fs_mount_dir(fs, "", root);
    JceWorldStreamConfig cfg = jce_world_stream_config_default();
    cfg.load_radius = 20.0f;
    cfg.unload_radius = 30.0f;
    cfg.max_pending = config->concurrency;
    cfg.budget_mb = 8u;
    cfg.single_thread = config->concurrency == 1u;
    cfg.frame_budget_ms = 2.0f;
    JceWorldStreamer *streamer = correct ? jce_world_streamer_create(&cfg, scene, fs, NULL) : NULL;
    correct = correct && streamer;
    StreamWitness witness = {0};
    witness.scene = scene;
    witness.entities_per_chunk = config->depth;
    witness.correct = true;
    if (streamer) {
        for (uint32_t chunk = 0; chunk < config->count; ++chunk) {
            char path[64];
            snprintf(path, sizeof(path), "cell_%u.scene.json", chunk);
            jce_world_streamer_register_chunk(streamer, chunk,
                        jce_v3(CHUNK_SPACING * chunk, 0, 0), 1.0f, path);
        }
        jce_world_streamer_set_entity_callbacks(streamer, on_spawn, on_despawn, &witness);
    }
    correct = correct && jce_world_streamer_chunk_count(streamer) == config->count;
    LOG_INFO(LOG_TAG, "benchmark-stream: chunks=%u entities-per-chunk=%u cooperative=%u "
        "pending-limit=%u load=20 unload=30 budget-mib=8 apply-ms=2 cache=warm scope=route-no-gpu",
        config->count, config->depth, cfg.single_thread ? 1u : 0u, cfg.max_pending);
    for (uint32_t round = 0; correct && round < config->rounds + 1u; ++round) {
        correct = drain(streamer, jce_v3(-1000, 0, 0), 0u, 0u) && witness.correct &&
                  jce_world_streamer_entity_count(streamer) == 0;
        uint64_t spawned = witness.spawned;
        uint64_t despawned = witness.despawned;
        uint32_t completed = 0;
        uint64_t point_ticks[MAX_CHUNKS] = {0};
        uint64_t started = jce_time_perf_counter();
        for (uint32_t chunk = 0; correct && chunk < config->count; ++chunk) {
            witness.expected_chunk = chunk;
            uint64_t point_start = jce_time_perf_counter();
            correct = drain(streamer, jce_v3(CHUNK_SPACING * chunk, 0, 0), 1u, config->depth) &&
                      witness.correct && jce_world_streamer_entity_count(streamer) == config->depth &&
                      jce_world_streamer_chunk_state(streamer, chunk) == JCE_CHUNK_LOADED;
            uint64_t point_end = jce_time_perf_counter();
            point_ticks[chunk] = point_end - point_start;
            if (correct) ++completed;
        }
        uint64_t ended = jce_time_perf_counter();
        correct = correct && witness.spawned - spawned == (uint64_t)config->count * config->depth &&
            witness.despawned - despawned == (uint64_t)(config->count - 1u) * config->depth &&
            jce_world_streamer_refused_loads(streamer) == 0;
        if (round > 0u) {
            for (uint32_t chunk = 0; chunk < completed; ++chunk)
                LOG_INFO(LOG_TAG, "benchmark-stream-point: round=%u chunk=%u ticks=%llu frequency=%llu",
                         round - 1u, chunk, (unsigned long long)point_ticks[chunk],
                         (unsigned long long)jce_time_perf_freq());
            bench_probe_sample(name, config, round - 1u, started, ended, completed,
                               witness.spawned, correct);
        }
        LOG_INFO(LOG_TAG, "benchmark-stream-residency: round=%u spawned=%llu despawned=%llu "
                 "resident=%u pending=%u accounted-bytes=%llu lru-evictions=%u",
                 round, (unsigned long long)witness.spawned, (unsigned long long)witness.despawned,
                 jce_world_streamer_entity_count(streamer), jce_world_streamer_pending_count(streamer),
                 (unsigned long long)jce_world_streamer_memory_used(streamer),
                 jce_world_streamer_evicted_count(streamer));
    }
    if (streamer) jce_world_streamer_destroy(streamer);
    if (scene) jce_scene_destroy(scene);
    if (fs) jce_fs_destroy(fs);
    return correct;
}
