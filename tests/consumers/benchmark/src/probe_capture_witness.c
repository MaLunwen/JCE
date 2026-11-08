/* Consumer policy only: public capture/trace, no private GPU owner or loop. */
#include "probe_capture_witness.h"
#include <jce/api_core.h>
#include <jce/api_graphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PC_NONE, PC_HDR, PC_LDR, PC_CANCEL_READY, PC_CANCEL_EXIT };
enum { PC_BASELINE, PC_CAPTURE, PC_RETIRE, PC_BAKE, PC_FINISHED };
enum { PC_SERVICE, PC_ATTACHED, PC_RETIRING, PC_DESTINATIONS, PC_BYTES,
       PC_STOP_RELEASES, PC_LEDGER_COUNT };
static const char *const ledger_names[PC_LEDGER_COUNT] = {
    "readback.cpu_service_open", "readback.cpu_attached_queues",
    "readback.cpu_retiring_queues", "readback.cpu_owned_destinations",
    "readback.cpu_owned_destination_bytes", "readback.backend_stop_cpu_releases"
};
static struct {
    int mode, phase;
    bool failed, complete, cancelled, retained_pending, collected_ready, worker_ended;
    unsigned probe_count, turns, delegated, polls, worker_submits, worker_ends;
    uint32_t cancel_frame, current_frame, seen;
    uint64_t started_ms, worker_task;
    int64_t ledger[PC_LEDGER_COUNT];
    JceEntity probe;
    JceTraceCursor cursor;
    char cube[512], irradiance[512], cancelled_cube[512], cancelled_irr[512];
    char events[512], report[512];
    const char *reason;
} pc;
static const char *pc_mode(void)
{
    switch (pc.mode) {
    case PC_HDR: return "hdr";
    case PC_LDR: return "ldr";
    case PC_CANCEL_READY: return "cancel-ready";
    case PC_CANCEL_EXIT: return "cancel-exit";
    default: return "inactive";
    }
}
static void pc_fail(const char *reason)
{
    if (!pc.failed) pc.reason = reason;
    pc.failed = true;
    jce_engine_request_quit();
}
static void pc_record(const char *event, uint64_t a, uint64_t b, uint64_t c)
{
    char line[768];
    int n = snprintf(line, sizeof line,
        "{\"event\":\"%s\",\"frame\":%u,\"a\":\"%llu\",\"b\":\"%llu\",\"c\":\"%llu\","
        "\"service\":%lld,\"attached\":%lld,\"retiring\":%lld,\"destinations\":%lld,"
        "\"bytes\":%lld,\"stop_releases\":%lld}\n", event, pc.current_frame,
        (unsigned long long)a, (unsigned long long)b, (unsigned long long)c,
        (long long)pc.ledger[PC_SERVICE], (long long)pc.ledger[PC_ATTACHED],
        (long long)pc.ledger[PC_RETIRING], (long long)pc.ledger[PC_DESTINATIONS],
        (long long)pc.ledger[PC_BYTES], (long long)pc.ledger[PC_STOP_RELEASES]);
    if (n <= 0 || (size_t)n >= sizeof line ||
        !jce_fs_host_append(pc.events, line, (size_t)n)) pc_fail("event_write_failed");
}
static void pc_read_trace(void)
{
    JceTraceEvent events[128];
    /* Bounded diagnostic cursor; never reset producers or steal another cursor. */
    for (unsigned batch = 0; batch < 32u; ++batch) {
        uint32_t count = jce_trace_read(&pc.cursor, events, 128u);
        if (pc.cursor.lost_events) { pc_fail("observer_trace_lost"); return; }
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *e = &events[i];
            if (e->type == JCE_TRACE_EVENT_COUNTER) {
                for (unsigned k = 0; k < PC_LEDGER_COUNT; ++k) {
                    if (strcmp(e->name, ledger_names[k])) continue;
                    pc.ledger[k] = (int64_t)e->value_u64;
                    pc.seen |= 1u << k;
                    pc_record("ledger", e->sequence, k, e->value_u64);
                    break;
                }
            }
            if (strcmp(e->name, "renderer.reflection-probe.bake")) continue;
            if (e->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
                if (pc.worker_task || e->value_u32[0] != JCE_TRACE_TASK_ASYNC)
                    pc_fail("unexpected_bake_submit");
                pc.worker_task = e->id;
                ++pc.worker_submits;
                pc_record("bake_submit", e->id, e->parent_id, e->value_u32[0]);
            } else if (e->type == JCE_TRACE_EVENT_TASK_END) {
                if (!pc.worker_task || e->parent_id != pc.worker_task ||
                    e->value_u32[0] != JCE_TRACE_TASK_SUCCEEDED || pc.worker_ended)
                    pc_fail("bake_worker_unsuccessful_or_unowned");
                pc.worker_ended = true;
                ++pc.worker_ends;
                pc_record("bake_end", e->id, e->parent_id, e->value_u32[0]);
            }
        }
        if (count < 128u) return;
    }
    pc_fail("observer_turn_budget_exceeded");
}
static bool pc_zero(void)
{
    return pc.seen == (1u << PC_LEDGER_COUNT) - 1u &&
        pc.ledger[PC_SERVICE] == 1 && pc.ledger[PC_ATTACHED] == 0 &&
        pc.ledger[PC_RETIRING] == 0 && pc.ledger[PC_DESTINATIONS] == 0 &&
        pc.ledger[PC_BYTES] == 0 && pc.ledger[PC_STOP_RELEASES] == 0;
}
static bool pc_path(char *out, size_t cap, const char *directory, const char *leaf)
{
    int n = snprintf(out, cap, "%s/%s", directory, leaf);
    return n > 0 && (size_t)n < cap && !jce_fs_host_exists_file(out);
}
static void pc_probe(JceScene *scene, JceEntity entity, void *user)
{
    (void)scene; (void)user;
    ++pc.probe_count;
    pc.probe = entity;
}
bool bench_probe_capture_prepare(JceScene *scene)
{
    const char *mode = getenv("JCE_BENCH_PROBE_CAPTURE");
    if (!mode || !mode[0]) return true;
    if (!strcmp(mode, "hdr")) pc.mode = PC_HDR;
    else if (!strcmp(mode, "ldr")) pc.mode = PC_LDR;
    else if (!strcmp(mode, "cancel-ready")) pc.mode = PC_CANCEL_READY;
    else if (!strcmp(mode, "cancel-exit")) pc.mode = PC_CANCEL_EXIT;
    else return false;
    static const char *const incompatible[] = {
        "JCE_BENCH_PROBE", "JCE_BENCH_GPU_SURVIVORS", "JCE_BENCH_PARTICLE_OBSERVE",
        "JCE_BENCH_PARTICLE_STATS", "JCE_BENCH_PARTICLE_GPU_STATS", "JCE_BENCH_HIZ_NPOT",
        "JCE_BENCH_RT_TARGET", "JCE_BENCH_PLAYER_SHOT", "JCE_SHOT_FRAME", "JCE_SHOT_PATH",
        "JCE_KPI_GAME_SHOTS", "JCE_WINCAP_PATH"
    };
    for (size_t i = 0; i < sizeof incompatible / sizeof incompatible[0]; ++i) {
        const char *value = getenv(incompatible[i]);
        if (value && value[0]) return false;
    }
    const char *directory = getenv("JCE_BENCH_PROBE_CAPTURE_DIR");
    if (!scene || !jce_trace_enabled() || !directory || !directory[0] ||
        !jce_fs_host_exists_dir(directory) ||
        !pc_path(pc.cube, sizeof pc.cube, directory, "probe.ktx") ||
        !pc_path(pc.irradiance, sizeof pc.irradiance, directory, "probe.irr.ktx") ||
        !pc_path(pc.cancelled_cube, sizeof pc.cancelled_cube, directory, "cancelled.ktx") ||
        !pc_path(pc.cancelled_irr, sizeof pc.cancelled_irr, directory, "cancelled.irr.ktx") ||
        !pc_path(pc.events, sizeof pc.events, directory, "probe-events.jsonl") ||
        !pc_path(pc.report, sizeof pc.report, directory, "probe-host.json")) return false;
    jce_scene_each_reflection_probe(scene, pc_probe, NULL);
    JceReflectionProbeComponent *probe = jce_scene_get_reflection_probe(scene, pc.probe);
    if (pc.probe_count != 1u || !probe || probe->resolution != 16 ||
        probe->mode != JCE_REFLECTION_PROBE_BAKED || probe->hdr != (pc.mode != PC_LDR))
        return false;
    pc.started_ms = jce_time_ticks_ms();
    pc.reason = "unfinished";
    /* Explicit diagnostic cadence; these runs are never throughput samples. */
    jce_engine_set_target_fps(60);
    jce_trace_cursor_init(&pc.cursor);
    return true;
}
static bool pc_begin(JceScene *scene)
{
    const char *path = (pc.mode == PC_CANCEL_READY || pc.mode == PC_CANCEL_EXIT) &&
                       !pc.cancelled ? pc.cancelled_cube : pc.cube;
    if (!jce_scene_probe_capture_begin(scene, pc.probe, 16u, path)) {
        pc_fail("real_capture_begin_refused"); return false;
    }
    pc.phase = PC_CAPTURE;
    pc_record("capture_begin", pc.cancelled, 16u, pc.mode != PC_LDR);
    return true;
}
bool bench_probe_capture_draw(const JceServices *services, JceScene *scene,
                              JceSceneRenderer *renderer, unsigned frame)
{
    if (!pc.mode) return false;
    pc.current_frame = frame;
    ++pc.turns;
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bench.probe.turn", pc.turns);
    if (!services || !services->renderer || !renderer ||
        jce_renderer_is_headless(services->renderer)) pc_fail("real_gpu_owner_required");
    pc_read_trace();
    if (jce_time_ticks_ms() - pc.started_ms > 45000u) pc_fail("capture_watchdog");
    if (pc.failed || pc.phase == PC_FINISHED) return true;
    if (pc.phase == PC_BASELINE) {
        if (!pc_zero()) { pc_fail("readback_zero_baseline_unavailable"); return true; }
        pc_record("zero_baseline", 0, 0, 0);
        if (!pc_begin(scene)) return true;
    }
    if (pc.phase == PC_RETIRE) {
        if (pc_zero()) {
            uint32_t now = jce_renderer_get_frame_index(services->renderer);
            if (now == pc.cancel_frame) { pc_fail("retire_without_central_advance"); return true; }
            pc.collected_ready = true;
            pc_record("collected_ready", pc.cancel_frame, now, 0);
            if (!pc_begin(scene)) return true;
        } else { ++pc.delegated; return false; }
    }
    if (pc.phase == PC_CAPTURE) {
        uint32_t before = jce_renderer_get_frame_index(services->renderer);
        JceProbeCaptureStatus status = jce_scene_probe_capture_poll(renderer, scene, services->renderer);
        uint32_t after = jce_renderer_get_frame_index(services->renderer);
        ++pc.polls;
        pc_record("capture_poll", before, after, status);
        if (before != after) pc_fail("poll_submitted_an_extra_frame");
        if (status == JCE_PROBE_CAPTURE_FAILED || status == JCE_PROBE_CAPTURE_IDLE)
            pc_fail("real_capture_failed");
        if (!pc.cancelled && (pc.mode == PC_CANCEL_READY || pc.mode == PC_CANCEL_EXIT) &&
            status == JCE_PROBE_CAPTURE_RENDERING) {
            /* First poll submitted a face readback; cancel before Engine end_frame. */
            jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bench.probe.cancel", before);
            jce_scene_probe_capture_cancel();
            pc.cancel_frame = before;
            pc.cancelled = true;
            pc_read_trace();
            pc.retained_pending = pc.ledger[PC_SERVICE] == 1 &&
                pc.ledger[PC_ATTACHED] == 0 && pc.ledger[PC_RETIRING] > 0 &&
                pc.ledger[PC_DESTINATIONS] > 0 && pc.ledger[PC_BYTES] > 0 &&
                pc.ledger[PC_STOP_RELEASES] == 0;
            pc_record("cancel_pending", before,
                      jce_renderer_get_frame_index(services->renderer), pc.retained_pending);
            if (jce_scene_probe_capture_in_flight() ||
                before != jce_renderer_get_frame_index(services->renderer)) pc_fail("cancel_did_not_remain_non_submitting");
            if (!pc.retained_pending) pc_fail("pending_retention_not_observed");
            if (pc.mode == PC_CANCEL_EXIT && !pc.failed) {
                pc.complete = true; pc.reason = "pending_cancel_exit_requested";
                pc.phase = PC_FINISHED; jce_engine_request_quit();
            } else pc.phase = PC_RETIRE;
            return true;
        }
        if (status == JCE_PROBE_CAPTURE_DONE) {
            pc.phase = PC_BAKE;
            pc_record("capture_faces_done_bake_submitted", 0, 0, 0);
        }
        /* DONE/FAILED/cancel still owns this frame's view band. */
        return true;
    }
    if (pc.phase == PC_BAKE && pc.worker_ended && !pc.failed) {
        uint64_t cube_bytes = 0, irradiance_bytes = 0;
        if (pc.worker_submits != 1u || pc.worker_ends != 1u ||
            !jce_fs_host_get_size(pc.cube, &cube_bytes) || cube_bytes <= 64u ||
            !jce_fs_host_get_size(pc.irradiance, &irradiance_bytes) || irradiance_bytes <= 64u ||
            jce_fs_host_exists_file(pc.cancelled_cube) || jce_fs_host_exists_file(pc.cancelled_irr)) {
            pc_fail("successful_worker_without_fresh_pair"); return true;
        }
        pc.complete = true; pc.reason = "captured_faces_worker_success_and_pair_observed";
        pc.phase = PC_FINISHED;
        pc_record("complete", cube_bytes, irradiance_bytes, pc.worker_task);
        jce_engine_request_quit();
    }
    ++pc.delegated;
    return false;
}
void bench_probe_capture_exit(void)
{
    if (!pc.mode || !pc.report[0]) return;
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "bench.probe.exit", pc.turns);
    if (jce_scene_probe_capture_in_flight()) jce_scene_probe_capture_cancel();
    pc_read_trace();
    if (!pc.complete) { pc.failed = true; pc.reason = "exit_before_witness_completion"; }
    char report[1536];
    int n = snprintf(report, sizeof report,
        "{\"schema\":1,\"case\":\"%s\",\"complete\":%s,\"failed\":%s,\"reason\":\"%s\","
        "\"pending_retained\":%s,\"collected_ready\":%s,\"turns\":%u,\"ordinary_draw_delegated\":%u,"
        "\"polls\":%u,\"worker_submits\":%u,\"worker_ends\":%u,\"worker_task\":\"%llu\",\"trace_lost\":\"%llu\"}\n",
        pc_mode(), pc.complete ? "true" : "false", pc.failed ? "true" : "false", pc.reason,
        pc.retained_pending ? "true" : "false", pc.collected_ready ? "true" : "false",
        pc.turns, pc.delegated, pc.polls, pc.worker_submits, pc.worker_ends,
        (unsigned long long)pc.worker_task, (unsigned long long)pc.cursor.lost_events);
    if (n <= 0 || (size_t)n >= sizeof report ||
        !jce_fs_host_write_new(pc.report, report, (size_t)n))
        jce_log_write(JCE_LOG_LEVEL_ERROR, "benchmark_probe_capture", __FILE__, __LINE__,
                      "probe witness receipt write failed");
}
