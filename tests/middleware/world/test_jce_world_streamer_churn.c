/*
 * test_jce_world_streamer_churn.c  — streaming churn UAF regression (L4).
 *
 * Reproduces the rapid chunk load/unload/reload churn that crashed the editor
 * during world traversal (ACCESS_VIOLATION surfacing in cJSON_malloc = heap
 * corruption from a use-after-free / overflow elsewhere).  Drives the real
 * world streamer with its real structured executor + real filesystem against
 * synthetic on-disk chunk fragments, and — like editor Play — wires each
 * streamed cell's entities into a live JceRuntime on spawn and unwires them on
 * despawn (jce_runtime_spawn/despawn_gameplay_for_ids).  The camera ping-pongs
 * fast enough that a chunk's background load (or in-flight time-sliced apply)
 * is still alive when the chunk is unloaded, then immediately reloaded.
 *
 * Each cell carries colliders + Rigidbodies (so the runtime tracks bodies in
 * rt->bodies[], grown via rt_grow_bodies) — exercising the wire/unwire churn on
 * the per-entity gameplay arrays.  Built under ASAN this faults at the exact
 * freed/overflowed allocation + use site.
 */

#include "unity.h"

#include <jce/os/core/jce_timer.h>

#include <jce/application/jce_runtime.h>
#include <jce/resource/jce_world_streamer.h>
#include <jce/resource/jce_streaming.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── Synthetic chunk fragment authoring ─────────────────────────────── */

#define CHUNK_COUNT       16
#define ENTS_PER_CHUNK    48
#define CHUNK_SPACING     300.0f

static char g_dir[1024];

/* Write a cell fragment whose entities carry Transform + BoxCollider +
 * Rigidbody so the runtime wire path appends to rt->bodies[] (grown by
 * rt_grow_bodies) on every cell load and swap-removes on every unload —
 * the editor-Play churn corner. */
static void write_chunk_fragment(const char *dir, uint32_t id)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/cell_%u.scene.json", dir, id);

    char buf[131072];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
                  "{\"scene\":{\"version\":1,\"entities\":[");
    for (int i = 0; i < ENTS_PER_CHUNK; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"id\":%d,\"name\":\"streamed_entity_%u_%d\",\"parentId\":0,"
            "\"components\":["
            "{\"type\":\"Transform\",\"posX\":%d,\"posY\":0,\"posZ\":%d,"
            "\"rotX\":0,\"rotY\":0,\"rotZ\":0,"
            "\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
            "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
            "\"centerX\":0,\"centerY\":0,\"centerZ\":0},"
            "{\"type\":\"Rigidbody\",\"mass\":1,\"isKinematic\":false,"
            "\"useGravity\":true}]}",
            (i ? "," : ""), 100000 + (int)id * 1000 + i, id, i, i, i);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}}");
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(buf));
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, buf, (size_t)n));
}

/* ── Combined spawn/despawn callbacks: runtime wire + editor-style mirror ── *
 * Mirrors jce_editor_play.cpp play_streamer_spawn_cb / despawn_cb. */
static JceRuntime *g_rt;
static uint64_t    g_mirror[CHUNK_COUNT * ENTS_PER_CHUNK * 4];
static uint32_t    g_mirror_count;
static uint32_t    g_spawn_events;
static uint32_t    g_despawn_events;

static void spawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    (void)user;
    g_spawn_events++;
    if (g_rt) jce_runtime_spawn_gameplay_for_ids(g_rt, ids, count);
    for (uint32_t i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(g_mirror_count <
                         sizeof(g_mirror) / sizeof(g_mirror[0]));
        g_mirror[g_mirror_count++] = ids[i];
    }
}

static void despawn_cb(const uint64_t *ids, uint32_t count, void *user)
{
    (void)user;
    g_despawn_events++;
    if (g_rt) jce_runtime_despawn_gameplay_for_ids(g_rt, ids, count);
    for (uint32_t i = 0; i < count; i++)
        for (uint32_t j = 0; j < g_mirror_count; j++)
            if (g_mirror[j] == ids[i]) {
                g_mirror[j] = g_mirror[--g_mirror_count];
                break;
            }
}

/* ── Churn driver ────────────────────────────────────────────────────── */

static JceWorldStreamer *make_streamer(JceScene *scene, JceFileSystem *fs)
{
    JceWorldStreamConfig cfg = jce_world_stream_config_default();
    cfg.mode          = JCE_STREAM_RADIAL;
    cfg.load_radius   = 200.0f;
    cfg.unload_radius = 280.0f;
    cfg.max_pending   = 8;
    cfg.budget_mb     = 0;
    cfg.single_thread = false;
    /* Tiny budget so each chunk's apply spans several updates → multiple
     * apply_streams open at once and unloads land mid-apply. */
    cfg.frame_budget_ms = 0.001f;

    JceWorldStreamer *ws =
        jce_world_streamer_create(&cfg, scene, fs, NULL);
    TEST_ASSERT_NOT_NULL(ws);

    for (uint32_t id = 0; id < CHUNK_COUNT; id++) {
        char path[256];
        snprintf(path, sizeof(path), "cell_%u.scene.json", id);
        jce_vec3 center = jce_v3((float)id * CHUNK_SPACING, 0.0f, 0.0f);
        jce_world_streamer_register_chunk(ws, id, center, CHUNK_SPACING * 0.5f,
                                          path);
    }
    jce_world_streamer_set_entity_callbacks(ws, spawn_cb, despawn_cb, NULL);
    return ws;
}

/* Ping-pong the camera in a TIGHT band (≈ one chunk's load/unload hysteresis)
 * so chunks flip load↔unload nearly every update while their async load /
 * sliced apply is still in flight — the exact editor repro (step≈pp≈hysteresis). */
static void churn(JceWorldStreamer *ws, JceRuntime *rt, int iters)
{
    const float lo = 0.0f;
    const float hi = 2.0f * CHUNK_SPACING;     /* sweep ~2 cells */
    float x = lo, dir = 1.0f;
    const float step = CHUNK_SPACING * 0.5f;   /* > hysteresis → flips per tick */
    for (int i = 0; i < iters; i++) {
        x += step * dir;
        if (x >= hi) { x = hi; dir = -1.0f; }
        else if (x <= lo) { x = lo; dir = 1.0f; }
        jce_world_streamer_update(ws, jce_v3(x, 0.0f, 0.0f));
        if (rt) jce_runtime_step(rt, 1.0f / 60.0f);
    }
}

static void test_world_streamer_async_churn(void)
{
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = getenv("TMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(g_dir, sizeof(g_dir), "%s/jce_ws_churn_%lu",
             tmp, (unsigned long)(uintptr_t)&g_dir);
    for (char *p = g_dir; *p; ++p) if (*p == '\\') *p = '/';

    for (uint32_t id = 0; id < CHUNK_COUNT; id++)
        write_chunk_fragment(g_dir, id);

    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);

    JceFileSystem *fs = jce_fs_create();
    TEST_ASSERT_NOT_NULL(fs);
    jce_fs_mount_dir(fs, "", g_dir);

    /* Live runtime with physics — wires/unwires streamed cell gameplay, exactly
     * like editor Play (play_streamer_spawn_cb / despawn_cb). */
    JceRuntimeDesc rd;
    memset(&rd, 0, sizeof(rd));
    rd.scene          = scene;
    rd.enable_physics = true;
    rd.fixed_timestep = 1.0f / 60.0f;
    rd.gravity_y      = -9.81f;
    g_rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(g_rt);

    g_mirror_count = g_spawn_events = g_despawn_events = 0;

    JceWorldStreamer *ws = make_streamer(scene, fs);

    /* Heavy tight-band churn: thousands of load/unload/reload edges with the
     * runtime tracking/untracking each cell's bodies. */
    churn(ws, g_rt, 4000);

    /* SETTLE ON a chunk first and pump until the streamer is quiescent.
     *
     * The churn above runs on frame_budget_ms = 0.001 deliberately, so that
     * applies overlap and unloads land mid-apply -- but that makes "did
     * anything load?" a question about how much CPU this process got, and
     * under ctest -j 8 the answer was sometimes no.  The assertions below then
     * failed on a machine-load fact, roughly one parallel run in five, while
     * the same binary passed 6/6 alone.
     *
     * Pumping to quiescence gives them something deterministic to observe:
     * the 1-microsecond budget SLICES each apply, it does not prevent it, so
     * with the camera parked and enough updates a chunk must finish.  Bounded
     * by wall time as well as iterations -- an unbounded "until idle" loop
     * turns a hang into a 60-second timeout with no message. */
    {
        const uint64_t t0 = jce_time_perf_counter();
        int settled = 0;
        for (int i = 0; i < 20000; i++) {
            jce_world_streamer_update(ws, jce_v3(0.0f, 0.0f, 0.0f));
            jce_runtime_step(g_rt, 1.0f / 60.0f);
            /* Yield: this loop pins a core while the streamer's LOAD THREADS
             * do the work it is waiting for.  Under `ctest -j 8` eight such
             * processes starve each other and the wait expires with the loads
             * still queued -- the quiescence check alone was not enough. */
            jce_thread_sleep_ms(0);
            if (jce_world_streamer_pending_count(ws) == 0 &&
                jce_world_streamer_loaded_count(ws) > 0) { settled = 1; break; }
            if (jce_time_perf_to_ms(t0, jce_time_perf_counter()) > 20000.0)
                break;
        }
        TEST_ASSERT_TRUE_MESSAGE(settled,
            "parked on a chunk and pumped to quiescence, the streamer must "
            "have loaded something -- the tiny frame budget SLICES the apply, "
            "it does not prevent it, so this is a fact about the streamer and "
            "not about how much CPU this process was given");
    }

    /* Settle off the row so everything unloads. */
    for (int i = 0; i < 64; i++) {
        jce_world_streamer_update(ws, jce_v3(-2000.0f, 0.0f, 0.0f));
        jce_runtime_step(g_rt, 1.0f / 60.0f);
    }

    /* Teardown mirrors editor Play: streamer first (joins its executor and
     * despawns all, unwiring runtime), then runtime, fs, scene. */
    jce_world_streamer_destroy(ws);
    jce_runtime_destroy(g_rt);  g_rt = NULL;
    jce_fs_destroy(fs);
    jce_scene_destroy(scene);

    TEST_ASSERT_TRUE(g_spawn_events > 0);
    TEST_ASSERT_TRUE(g_despawn_events > 0);
    TEST_PASS_MESSAGE("async churn + runtime wire/unwire survived");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_world_streamer_async_churn);
    return UNITY_END();
}
