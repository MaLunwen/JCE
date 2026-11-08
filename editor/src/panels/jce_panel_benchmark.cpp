/*
 * jce_panel_benchmark.cpp  In-editor performance benchmark (Profiler "Benchmark"
 * tab).
 *
 * A built-in, industry-standard engine stress test: spawns one of the five
 * classic benchmark workloads into the current scene (via the shared
 * jce_state_benchmark_spawn machinery) and shows live per-frame stats (bgfx
 * draw/CPU/GPU + render-queue submit/instance counts + the CPU phase
 * breakdown), so the editor exposes the same measurements as the headless
 * JCE_STRESS_* harnesses.
 *
 *   0. Draw Call    — N cubes, each a UNIQUE material → no batching → N draws.
 *   1. Instancing   — ONE scatter entity, N cube instances → 1 instanced submit.
 *   2. Triangle     — same, SPHERE primitive → GPU triangle throughput.
 *   3. Entity Count — N entities + a per-frame ECS transform integrate.
 *   4. Physics      — N dynamic rigid bodies + box colliders (Bullet).
 *
 * All user-facing strings go through jce_editor_i18n(); the keys live in
 * editor/resources/assets/i18n/en.json (the canonical table every locale falls
 * back to) with zh_cn / zh_tw translations.
 */

#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_config.h"          /* jce_editor_dotjce_path */
#include "scene/jce_editor_scene_render.h"   /* jce_editor_scene_get_camera */
#include "jce_benchmark_observation.h"
#include "jce_profiler_observation.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <string>

extern "C" {
#include <jce/application/jce_engine.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_renderer_caps.h>
#include <jce/renderer/jce_scene_renderer.h>
#include <jce/renderer/jce_camera.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_filesystem.h>      /* project-scoped baseline file */
}

/* Open-project root — owned by dialog_project.cpp (same explicit-root
 * pattern as jce_project_settings / jce_pak_key).  Declared at GLOBAL
 * scope: inside the anonymous namespace below the extern would acquire
 * internal linkage and never bind to the definition. */
extern char s_current_project_root[512];

namespace {

enum BenchKind {
    BENCH_DRAWCALL = 0,
    BENCH_INSTANCING,
    BENCH_TRIANGLE,
    BENCH_ENTITY,
    BENCH_PHYSICS,
    BENCH_KIND_COUNT
};

const char *kKindLabelKey[BENCH_KIND_COUNT] = {
    "benchmark.kind.drawcall",
    "benchmark.kind.instancing",
    "benchmark.kind.triangle",
    "benchmark.kind.entity",
    "benchmark.kind.physics",
};

const char *kKindHelpKey[BENCH_KIND_COUNT] = {
    "benchmark.help.drawcall",
    "benchmark.help.instancing",
    "benchmark.help.triangle",
    "benchmark.help.entity",
    "benchmark.help.physics",
};

int s_kind  = BENCH_DRAWCALL;
int s_count = 10000;

void stat_row(const char *label_key, const char *fmt, double v)
{
    ImGui::TextUnformatted(jce_editor_i18n(label_key));
    ImGui::SameLine(190.0f);
    ImGui::Text(fmt, v);
}

/* ── Frame-time history → percentile stats ──────────────────────────────────
 * Industry benchmarks report the frame-time DISTRIBUTION (avg, 1% low, 99th
 * percentile, min/max), not a single instantaneous FPS — averages hide the
 * stutter that actually breaks the feel of a build.  Keep a rolling window of
 * recent frame times and derive those metrics on demand. */
constexpr int FT_CAP    = 512;
constexpr int FT_WARMUP = 30;     /* discard the first N frames (steady state) */
float s_ft[FT_CAP];
int   s_ft_count = 0;
int   s_ft_head  = 0;
int   s_warmup   = FT_WARMUP;     /* frames still to discard after a reset */
uint32_t s_ft_last_spawned = 0;   /* auto-reset the window on a fresh workload */
bool  s_isolated = false;         /* "Isolated scene" toggle */

struct BenchmarkObservationState {
    JceBenchmarkCadence cadence;
    JceBenchmarkBaseline baseline;
    JceBenchmarkProfile baseline_profile;
    bool baseline_loaded=false;
    int baseline_kind=-1, workload_kind=-1;
    uint32_t baseline_count=0;
    char baseline_root[512]{};
} s_observation;
void ft_reset()
{
    s_ft_count = 0; s_ft_head = 0; s_warmup = FT_WARMUP;
    jce_benchmark_reset(&s_observation.cadence);
}
void ft_push(float ms)
{
    if (ms <= 0.0f) return;
    if (s_warmup > 0) { --s_warmup; return; }   /* skip warm-up frames */
    s_ft[s_ft_head] = ms;
    s_ft_head = (s_ft_head + 1) % FT_CAP;
    if (s_ft_count < FT_CAP) ++s_ft_count;
}

/* v1 is a separate namespace: legacy max(CPU,GPU) baselines are never
 * imported or overwritten. This is a limited observed backend/cadence profile,
 * not certification of machine, scene, camera or presentation equivalence. */
using Baseline = JceBenchmarkBaseline;
constexpr uint64_t BASELINE_MAX_BYTES = 64 * 1024;
void baseline_path(char *out, size_t cap)
{
    constexpr const char *name="benchmark_ui_cadence_v1.txt";
    if (!s_current_project_root[0]) {
        if (!jce_editor_dotjce_path(name,out,cap)) out[0]='\0';
    } else {
        const int n=std::snprintf(out,cap,"%s/.jce/%s",s_current_project_root,name);
        if (n<0 || static_cast<size_t>(n)>=cap) out[0]='\0';
    }
}
bool baseline_read(const char *path, std::string &body)
{
    uint64_t read=0,total=0;
    char *raw=static_cast<char *>(jce_fs_host_read_capped(path,BASELINE_MAX_BYTES,&read,&total));
    if (!raw) return false;
    const bool complete=read==total;
    if (complete) body.assign(raw,static_cast<size_t>(read));
    jce_fs_buffer_free(raw);
    return complete;
}
bool baseline_save(int kind, int count, const JceBenchmarkProfile &profile,
                   double avg, double low, double p99)
{
    char current[256];
    if (!jce_benchmark_format_baseline(current,sizeof(current),s_observation.cadence,
                                     kind,count,profile,avg,low,p99)) return false;
    char path[1024]; baseline_path(path,sizeof(path));
    if (!path[0]) return false;
    std::string body;
    if (jce_fs_host_exists_file(path) && !baseline_read(path,body)) return false;
    std::istringstream input(body); std::string output,line;
    while (std::getline(input,line)) {
        const auto entry=jce_benchmark_parse_baseline(line.c_str());
        if (!jce_benchmark_baseline_matches(entry,kind,count,profile)) {
            output+=line; output+='\n';
        }
    }
    output+=current;
    if (output.size()>BASELINE_MAX_BYTES) return false;
    return jce_fs_host_write_all_atomic(path,output.data(),output.size());
}
Baseline baseline_load(int kind, int count, const JceBenchmarkProfile &profile)
{
    char path[1024]; baseline_path(path,sizeof(path));
    std::string body;
    if (!path[0] || !jce_fs_host_exists_file(path) || !baseline_read(path,body)) return {};
    std::istringstream input(body); std::string line;
    while (std::getline(input,line)) {
        const auto entry=jce_benchmark_parse_baseline(line.c_str());
        if (jce_benchmark_baseline_matches(entry,kind,count,profile)) return entry;
    }
    return {};
}
const char *gpu_reason_key(const JceGpuTimingObservation &gpu)
{
    switch (gpu.unavailable_reason) {
    case JCE_GPU_TIMING_BACKEND_UNAVAILABLE: return "benchmark.gpuBackendUnavailable";
    case JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE: return "benchmark.gpuFrequencyUnavailable";
    case JCE_GPU_TIMING_INTERVAL_UNAVAILABLE: return "benchmark.gpuIntervalUnavailable";
    default: return "benchmark.gpuContextUnavailable";
    }
}

struct FtStats { double avg_ms, min_ms, max_ms, p99_ms, low1_fps, avg_fps; int n; };

FtStats ft_stats()
{
    FtStats r = {0,0,0,0,0,0,0};
    r.n = s_ft_count;
    if (s_ft_count == 0) return r;
    float tmp[FT_CAP];
    for (int i = 0; i < s_ft_count; ++i) tmp[i] = s_ft[i];
    /* insertion sort ascending (n <= 512, runs only while the tab is visible) */
    for (int i = 1; i < s_ft_count; ++i) {
        float v = tmp[i]; int j = i - 1;
        while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; --j; }
        tmp[j + 1] = v;
    }
    double sum = 0.0;
    for (int i = 0; i < s_ft_count; ++i) sum += tmp[i];
    r.avg_ms = sum / s_ft_count;
    r.min_ms = tmp[0];
    r.max_ms = tmp[s_ft_count - 1];
    r.p99_ms = tmp[(int)((s_ft_count - 1) * 0.99)];
    r.avg_fps = r.avg_ms > 0.0 ? 1000.0 / r.avg_ms : 0.0;
    /* 1% low FPS = average of the worst (longest) 1% of frames, as FPS. */
    int worst = s_ft_count / 100; if (worst < 1) worst = 1;
    double wsum = 0.0;
    for (int i = 0; i < worst; ++i) wsum += tmp[s_ft_count - 1 - i];
    double wavg = wsum / worst;
    r.low1_fps = wavg > 0.0 ? 1000.0 / wavg : 0.0;
    return r;
}

} /* anonymous namespace */

/* Headless / scripted entry (JCE_BENCH_AUTOSPAWN editor hook) — same path as the
 * UI button, so the benchmark is runnable from the CLI for CI / QA. */
extern "C" void jce_editor_benchmark_run(int kind, int count) { jce_state_benchmark_spawn(kind, count); }

/* ── Public entry point (Profiler "Benchmark" tab) ──────────────────────── */

extern "C" void jce_editor_panel_benchmark_content(void)
{
    ImGui::TextDisabled("%s", jce_editor_i18n("benchmark.desc"));
    ImGui::Separator();

    /* Workload picker. */
    if (ImGui::BeginCombo(jce_editor_i18n("benchmark.workload"), jce_editor_i18n(kKindLabelKey[s_kind]))) {
        for (int i = 0; i < BENCH_KIND_COUNT; ++i)
            if (ImGui::Selectable(jce_editor_i18n(kKindLabelKey[i]), s_kind == i)) s_kind = i;
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("%s", jce_editor_i18n(kKindHelpKey[s_kind]));

    /* Count: presets + custom. */
    ImGui::Spacing();
    ImGui::TextUnformatted(jce_editor_i18n("benchmark.count"));
    const int kPresets[] = { 1000, 5000, 10000, 50000, 100000, 1000000 };
    for (int i = 0; i < (int)(sizeof kPresets / sizeof kPresets[0]); ++i) {
        ImGui::SameLine();
        char b[16];
        if (kPresets[i] >= 1000000) std::snprintf(b, sizeof b, "%dM", kPresets[i] / 1000000);
        else                        std::snprintf(b, sizeof b, "%dk", kPresets[i] / 1000);
        if (ImGui::SmallButton(b)) s_count = kPresets[i];
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputInt("##bench_count", &s_count, 1000, 10000);
    if (s_count < 1)       s_count = 1;
    if (s_count > 2000000) s_count = 2000000;

    /* Spawn / Clear. */
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("benchmark.spawn"), ImVec2(120, 0))) {
        jce_state_benchmark_spawn(s_kind, s_count);
        ft_reset();
        if (s_isolated) jce_state_benchmark_isolate(1);   /* re-hide the scene around the new workload */
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("benchmark.clear"), ImVec2(120, 0))) {
        jce_state_benchmark_clear();   /* also restores any isolated scene */
        ft_reset();
        s_isolated = false;
    }

    /* Test-fidelity controls: isolate the scene + frame the view deterministically. */
    if (ImGui::Checkbox(jce_editor_i18n("benchmark.isolated"), &s_isolated))
        jce_state_benchmark_isolate(s_isolated ? 1 : 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("benchmark.isolatedHelp"));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("benchmark.frameCam"))) {
        if (JceCamera *cam = jce_editor_scene_get_camera()) {
            const bool scat = (s_kind == BENCH_INSTANCING || s_kind == BENCH_TRIANGLE);
            const float r = scat ? sqrtf((float)s_count) + 6.0f
                                 : cbrtf((float)s_count) * 1.6f + 6.0f;
            jce_camera_set_position(cam, jce_v3(r * 0.75f, r * 0.55f, r * 0.75f));
            jce_camera_look_at(cam, jce_v3(0.0f, 0.0f, 0.0f));
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", jce_editor_i18n("benchmark.frameCamHelp"));

    const bool grid_kind = (s_kind == BENCH_DRAWCALL || s_kind == BENCH_ENTITY || s_kind == BENCH_PHYSICS);
    if (grid_kind && s_count > 100000) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), jce_editor_i18n("benchmark.spawnHint"), s_count);
    }

    const uint32_t spawned = jce_state_benchmark_spawned();
    const int      spawned_kind = jce_state_benchmark_kind();
    if (spawned != s_ft_last_spawned || spawned_kind != s_observation.workload_kind) {            /* fresh workload */
        ft_reset();
        s_ft_last_spawned = spawned;
        s_observation.workload_kind = spawned_kind;
        s_observation.baseline_loaded = false;
    }
    if (spawned) {
        ImGui::SameLine();
        const bool inst = (spawned_kind == BENCH_INSTANCING || spawned_kind == BENCH_TRIANGLE);
        ImGui::Text("[ %u %s ]", spawned,
                    jce_editor_i18n(inst ? "benchmark.units.instances" : "benchmark.units.entities"));
    }

    /* ── Live stats ─────────────────────────────────────────────────────── */
    ImGui::Separator();
    ImGui::SeparatorText(jce_editor_i18n("benchmark.liveStats"));

    const JceRenderer *renderer=jce_editor_get_renderer();
    JceBenchmarkProfile profile;
    profile.backend=renderer?static_cast<int>(jce_renderer_get_backend(renderer)):0;
    profile.tier=static_cast<int>(jce_renderer_get_tier());
    profile.target_fps=jce_engine_get_target_fps();
    profile.play=static_cast<int>(jce_state_get_play_state());
    profile.vsync=renderer && jce_renderer_get_vsync(renderer); profile.workload_isolated=s_isolated;
    const bool blocked=ImGui::GetIO().AppFocusLost || profile.play==JCE_PLAY_PAUSED ||
        ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    JceGpuTimingObservation gpu{};
    const auto cadence = jce_benchmark_poll(&s_observation.cadence, ImGui::GetFrameCount(), blocked, profile, &gpu);
    if (cadence.accepted) ft_push(static_cast<float>(cadence.ms));
    if (cadence.accepted) stat_row("benchmark.fps","%.1f Hz",1000.0/cadence.ms);
    else ImGui::TextDisabled("%s: %s",jce_editor_i18n("benchmark.fps"),
                             jce_editor_i18n("profiler.value.unavailable"));
    const JceFrameStats empty_stats{};
    const auto *backend_stats=jce_gfx_stats_capture();
    const auto timing=jce_profiler_observe(backend_stats?backend_stats:&empty_stats,&gpu);
    if (timing.cpu_available) stat_row("benchmark.cpuFrame","%.2f ms",timing.cpu_interval_ms);
    else ImGui::TextDisabled("%s: %s",jce_editor_i18n("benchmark.cpuFrame"),
                             jce_editor_i18n("profiler.value.unavailable"));
    if (timing.gpu_available) stat_row("benchmark.gpuFrame","%.2f ms",timing.gpu_ms);
    else ImGui::TextDisabled("%s: %s (%s)",jce_editor_i18n("benchmark.gpuFrame"),
        jce_editor_i18n("profiler.value.unavailable"),jce_editor_i18n(gpu_reason_key(gpu)));
    JceGpuStats st{};
    if (jce_renderer_get_gpu_stats(&st) && st.valid)
        stat_row("benchmark.draws","%.0f",static_cast<double>(st.num_draw));
    ImGui::TextWrapped("%s",jce_editor_i18n("benchmark.observationHelp"));
    ImGui::Text("%s: %s | %llu / %llu / %llu",jce_editor_i18n("benchmark.coverage"),
        jce_editor_i18n(s_observation.cadence.complete ? "benchmark.coverageComplete" : "benchmark.coverageIncomplete"),
        static_cast<unsigned long long>(s_observation.cadence.interruptions),
        static_cast<unsigned long long>(s_observation.cadence.missed_ui),
        static_cast<unsigned long long>(s_observation.cadence.missed_host));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",jce_editor_i18n("benchmark.coverageHelp"));
    if (ImGui::SmallButton(jce_editor_i18n("benchmark.reset"))) ft_reset();
    if (!s_observation.baseline_loaded || s_observation.baseline_kind!=spawned_kind || s_observation.baseline_count!=spawned ||
        !jce_benchmark_same_profile(s_observation.baseline_profile,profile) ||
        std::strcmp(s_observation.baseline_root,s_current_project_root)!=0) {
        s_observation.baseline=baseline_load(spawned_kind,static_cast<int>(spawned),profile);
        s_observation.baseline_profile=profile; s_observation.baseline_kind=spawned_kind; s_observation.baseline_count=spawned;
        std::snprintf(s_observation.baseline_root,sizeof(s_observation.baseline_root),"%s",s_current_project_root);
        s_observation.baseline_loaded=true;
    }

    /* Frame-time distribution (industry-standard: average hides stutter). */
    const FtStats ft = ft_stats();
    if (s_warmup > 0) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s (%d)", jce_editor_i18n("benchmark.warmup"), s_warmup);
    } else if (ft.n > 0) {
        ImGui::Spacing();
        ImGui::Text("%s  (%d %s)", jce_editor_i18n("benchmark.frameStats"),
                    ft.n, jce_editor_i18n("benchmark.samples"));
        stat_row("benchmark.avg",      "%.1f Hz", ft.avg_fps);
        stat_row("benchmark.onePctLow","%.1f Hz", ft.low1_fps);
        stat_row("benchmark.p99",      "%.2f ms",  ft.p99_ms);
        ImGui::TextUnformatted(jce_editor_i18n("benchmark.minmax"));
        ImGui::SameLine(190.0f);
        ImGui::Text("%.2f / %.2f ms", ft.min_ms, ft.max_ms);

        /* Baseline + regression: save the current distribution, compare to a
         * baseline captured (possibly on a previous build) for this workload. */
        if (spawned) {
            ImGui::BeginDisabled(!s_observation.cadence.complete);
            if (ImGui::SmallButton(jce_editor_i18n("benchmark.saveBaseline"))) {
                if (baseline_save(spawned_kind,static_cast<int>(spawned),profile,ft.avg_fps,ft.low1_fps,ft.p99_ms))
                    s_observation.baseline=baseline_load(spawned_kind,static_cast<int>(spawned),profile);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (!s_observation.cadence.complete) {
                ImGui::TextDisabled("%s",jce_editor_i18n("benchmark.coverageIncomplete"));
            } else if (s_observation.baseline.found) {
                const double dfps = s_observation.baseline.avg_fps > 0 ? (ft.avg_fps - s_observation.baseline.avg_fps) / s_observation.baseline.avg_fps * 100.0 : 0.0;
                const double dp99 = s_observation.baseline.p99_ms  > 0 ? (ft.p99_ms  - s_observation.baseline.p99_ms)  / s_observation.baseline.p99_ms  * 100.0 : 0.0;
                const ImVec4 col = (dfps >= -2.0) ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)   /* within noise / better */
                                                  : ImVec4(1.0f, 0.5f, 0.4f, 1.0f);  /* regressed */
                ImGui::TextColored(col, "%s: %+.0f%% Hz, %+.0f%% p99",
                                   jce_editor_i18n("benchmark.vsBaseline"), dfps, dp99);
            } else {
                ImGui::TextDisabled("%s", jce_editor_i18n("benchmark.noBaseline"));
            }
        }
    }

    ImGui::TextWrapped("%s",jce_editor_i18n("benchmark.baselineScope"));

    /* Render-queue: how many submits the N objects collapsed to (instancing). */
    JceSceneRenderer *sr = jce_editor_get_scene_renderer();
    if (sr) {
        JceSceneRqStats rq = {};
        jce_scene_renderer_get_rq_stats(sr, &rq);
        if (rq.enabled) {
            ImGui::Spacing();
            ImGui::Text(jce_editor_i18n("benchmark.renderQueue"),
                        rq.commands_in, rq.submits_out, rq.batches_merged, rq.instances_total);
        }
    }

    if ((spawned_kind == BENCH_ENTITY || spawned_kind == BENCH_PHYSICS) && spawned) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), jce_editor_i18n("benchmark.ticksUnderPlay"),
                           jce_editor_i18n(spawned_kind == BENCH_PHYSICS ? "benchmark.physics"
                                                                         : "benchmark.ecsUpdate"));
    }

    /* CPU phase breakdown (per frame), so the workload's true cost is isolated:
     * draw_submit / sr_loop (Draw Call), ecs_move (Entity Count, in Play),
     * physics (Physics, in Play).  Captured only while a benchmark is active
     * (jce_state_benchmark_spawn enables the perf-phase accumulator); this panel
     * is the sole consumer in the editor, so each report is one frame's slots. */
    if (spawned) {
        /* Non-destructive per-frame snapshot: report() RESETS the window and
         * fought the JCE_PERF_LOG consumer (each corrupted the other). */
        char phases[1024] = {0};
        {
            int pos = 0, n = jce_perf_phase_count();
            for (int i = 0; i < n && pos < (int)sizeof phases - 32; ++i) {
                const char *nm = NULL; double ms = 0.0;
                if (!jce_perf_phase_peek_frame(i, &nm, &ms) || !nm) continue;
                if (ms < 0.005) continue;
                int w = std::snprintf(phases + pos, sizeof phases - (size_t)pos,
                                      "%s=%.2f ", nm, ms);
                if (w <= 0 || w >= (int)(sizeof phases - (size_t)pos)) break;
                pos += w;
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText(jce_editor_i18n("benchmark.cpuPhases"));
        if (phases[0]) ImGui::TextWrapped("%s", phases);
        else           ImGui::TextDisabled("%s", jce_editor_i18n("benchmark.accumulating"));
    }
}
