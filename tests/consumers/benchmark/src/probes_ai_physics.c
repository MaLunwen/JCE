#include "probes.h"
#include <jce/api_ai.h>
#include <jce/api_physics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_probe"
#define GRID_WIDTH 32u
#define PROBE_WARMUP 2u

static uint32_t grid_neighbors(uint32_t node, uint32_t *ids, float *costs,
                                uint32_t capacity, void *user)
{
    (void)user;
    if (capacity < 4u || node >= GRID_WIDTH * GRID_WIDTH) return 0;
    uint32_t count = 0;
    const uint32_t x = node % GRID_WIDTH, y = node / GRID_WIDTH;
    if (x) ids[count++] = node - 1u;
    if (x + 1u < GRID_WIDTH) ids[count++] = node + 1u;
    if (y) ids[count++] = node - GRID_WIDTH;
    if (y + 1u < GRID_WIDTH) ids[count++] = node + GRID_WIDTH;
    for (uint32_t i = 0; i < count; ++i) costs[i] = 1;
    return count;
}

static float grid_distance(uint32_t node, uint32_t goal, void *user)
{
    (void)user;
    const int x = (int)(node % GRID_WIDTH) - (int)(goal % GRID_WIDTH);
    const int y = (int)(node / GRID_WIDTH) - (int)(goal / GRID_WIDTH);
    return (float)(abs(x) + abs(y));
}

static int compare_ticks(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static bool pathfinding(const BenchProbeConfig *config)
{
    JceGraphAstar *graph = jce_graph_astar_create();
    uint64_t *latencies = jce_malloc((size_t)config->count * sizeof(*latencies));
    if (!graph || !latencies) {
        jce_graph_astar_destroy(graph);
        jce_free(latencies);
        return false;
    }
    jce_graph_astar_set_callbacks(graph, grid_neighbors, grid_distance, NULL);
    bool correct = true;
    for (uint32_t round = 0; correct && round < config->rounds + PROBE_WARMUP; ++round) {
        uint32_t path[GRID_WIDTH * 2u];
        uint64_t completed = 0, checksum = 0, expanded = 0;
        uint64_t started = jce_time_perf_counter();
        for (uint32_t i = 0; i < config->count; ++i) {
            const uint32_t from = (i * 17u + 1u) % (GRID_WIDTH * GRID_WIDTH);
            const uint32_t to = (i * 43u + 997u) % (GRID_WIDTH * GRID_WIDTH);
            uint32_t length = 0;
            uint64_t before = jce_time_perf_counter();
            bool found = jce_graph_astar_search(graph, from, to, path,
                                                GRID_WIDTH * 2u, &length);
            latencies[i] = jce_time_perf_counter() - before;
            correct = found && length && length <= GRID_WIDTH * 2u &&
                      path[0] == from && path[length - 1u] == to &&
                      (float)(length - 1u) == grid_distance(from, to, NULL) &&
                      jce_graph_astar_last_cost(graph) == (float)(length - 1u);
            for (uint32_t p = 1; correct && p < length; ++p)
                correct = grid_distance(path[p - 1u], path[p], NULL) == 1;
            if (!correct) break;
            checksum += length;
            expanded += jce_graph_astar_last_nodes_expanded(graph);
            ++completed;
        }
        uint64_t ended = jce_time_perf_counter();
        correct = correct && completed == config->count;
        if (round >= PROBE_WARMUP) {
            bench_probe_sample("pathfinding", config, round - PROBE_WARMUP,
                               started, ended, completed, checksum, correct);
            if (correct) {
                qsort(latencies, config->count, sizeof(*latencies), compare_ticks);
                LOG_INFO(LOG_TAG, "benchmark-path-latency: round=%u p50_ticks=%llu "
                         "p99_ticks=%llu expanded=%llu",
                         round - PROBE_WARMUP,
                         (unsigned long long)latencies[(config->count - 1u) / 2u],
                         (unsigned long long)latencies[(config->count - 1u) * 99u / 100u],
                         (unsigned long long)expanded);
            }
        }
    }
    jce_graph_astar_destroy(graph);
    jce_free(latencies);
    return correct;
}

static JceBtStatus action(const char *name, void *user)
{
    (void)name;
    ++*(uint64_t *)user;
    return JCE_BT_SUCCESS;
}

static bool behavior_tree(const BenchProbeConfig *config)
{
    static const char xml[] = "<root BTCPP_format=\"4\" main_tree_to_execute=\"Probe\">"
        "<BehaviorTree ID=\"Probe\"><Sequence><ProbeAction/><ProbeAction/>"
        "<ProbeAction/></Sequence></BehaviorTree></root>";
    JceBtContext *context = jce_bt_create();
    JceBtTreeHandle *trees = jce_malloc((size_t)config->count * sizeof(*trees));
    if (!context || !trees) {
        if (context) jce_bt_destroy(context);
        jce_free(trees);
        return false;
    }
    uint64_t calls = 0;
    jce_bt_register_action(context, "ProbeAction", action, &calls);
    bool correct = true;
    for (uint32_t i = 0; correct && i < config->count; ++i) {
        trees[i] = jce_bt_load_tree(context, xml, (uint32_t)(sizeof(xml) - 1u));
        correct = jce_bt_tree_valid(trees[i]) &&
                  jce_bt_node_count(context, trees[i]) == 4u;
    }
    correct = correct && jce_bt_tree_count(context) == config->count;
    for (uint32_t round = 0; correct && round < config->rounds + PROBE_WARMUP; ++round) {
        calls = 0;
        uint64_t completed = 0;
        uint64_t started = jce_time_perf_counter();
        for (uint32_t i = 0; i < config->count; ++i) {
            if (jce_bt_tick(context, trees[i]) != JCE_BT_SUCCESS) { correct = false; break; }
            ++completed;
        }
        uint64_t ended = jce_time_perf_counter();
        correct = correct && completed == config->count && calls == 3u * completed;
        if (round >= PROBE_WARMUP)
            bench_probe_sample("behavior-tree", config, round - PROBE_WARMUP,
                               started, ended, completed, calls, correct);
    }
    jce_bt_destroy(context);
    jce_free(trees);
    return correct;
}

static bool raycast(const BenchProbeConfig *config)
{
    JcePhysicsWorldDesc world_desc = {0};
    world_desc.max_bodies = 256;
    world_desc.split_impulse = -1;
    JcePhysicsWorld *world = jce_physics_create(&world_desc);
    if (!world) return false;
    JceBodyHandle bodies[256];
    bool correct = true;
    for (uint32_t i = 0; correct && i < 256u; ++i) {
        JceBodyDesc desc = {0};
        desc.type = JCE_BODY_STATIC;
        desc.shape = JCE_SHAPE_BOX;
        desc.position = jce_v3((float)(i % 16u) * 2, 0, (float)(i / 16u) * 2);
        desc.rotation = jce_q_identity();
        desc.half_extents = jce_v3(0.5f, 0.5f, 0.5f);
        bodies[i] = jce_physics_body_create(world, &desc);
        correct = jce_body_valid(bodies[i]);
    }
    for (uint32_t round = 0; correct && round < config->rounds + PROBE_WARMUP; ++round) {
        uint64_t hits = 0, completed = 0;
        uint64_t started = jce_time_perf_counter();
        for (uint32_t i = 0; i < config->count; ++i) {
            uint32_t slot = (i / 2u) % 256u;
            bool expected_hit = (i % 2u) == 0;
            jce_vec3 origin = jce_v3((float)(slot % 16u) * 2 + (expected_hit ? 0 : 1000),
                                      10, (float)(slot / 16u) * 2);
            JceRaycastResult result = jce_physics_raycast(world, origin, jce_v3(0, -1, 0), 20);
            correct = result.hit == expected_hit && (!result.hit ||
                      (result.body.idx == bodies[slot].idx &&
                       isfinite(result.distance) && fabsf(result.distance - 9.5f) < 0.001f));
            if (!correct) break;
            hits += result.hit ? 1u : 0u;
            ++completed;
        }
        uint64_t ended = jce_time_perf_counter();
        correct = correct && completed == config->count && hits == (config->count + 1u) / 2u;
        if (round >= PROBE_WARMUP)
            bench_probe_sample("raycast", config, round - PROBE_WARMUP,
                               started, ended, completed, hits, correct);
    }
    jce_physics_destroy(world);
    return correct;
}

bool bench_probe_ai_physics(const char *name, const BenchProbeConfig *config)
{
    if (!strcmp(name, "pathfinding")) return pathfinding(config);
    if (!strcmp(name, "behavior-tree")) return behavior_tree(config);
    return raycast(config);
}
