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

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <sstream>
#include <string>

extern "C" {
#include <jce/renderer/jce_renderer.h>
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

void ft_reset()  { s_ft_count = 0; s_ft_head = 0; s_warmup = FT_WARMUP; }
void ft_push(float ms)
{
    if (ms <= 0.0f) return;
    if (s_warmup > 0) { --s_warmup; return; }   /* skip warm-up frames */
    s_ft[s_ft_head] = ms;
    s_ft_head = (s_ft_head + 1) % FT_CAP;
    if (s_ft_count < FT_CAP) ++s_ft_count;
}

/* ── Baseline persistence (regression detection across builds) ───────────────
 * Plain text, one workload per line:
 *   "<kind> <count> <avg_fps> <low1_fps> <p99_ms>".  Keyed by kind+count so a
 * rebuilt binary can be compared against a baseline captured on a prior commit.
 * Baselines are scene+machine specific, so the file is PROJECT-scoped
 * (<root>/.jce/benchmark_baseline.txt, explicit root — the editor never
 * chdirs); the legacy per-user ~/.jce path remains the fallback when no
 * project is open, and is copied forward once per project. */
struct Baseline { double avg_fps, low1_fps, p99_ms; bool found; };

void baseline_path(char *out, size_t cap)
{
    if (!s_current_project_root[0]) {   /* no project: legacy per-user file */
        if (!jce_editor_dotjce_path("benchmark_baseline.txt", out, cap))
            out[0] = '\0';
        return;
    }
    std::snprintf(out, cap, "%s/.jce/benchmark_baseline.txt",
                  s_current_project_root);

    /* One-time forward-migration per root: seed the project baseline from
     * the legacy global file.  The global file is kept as the no-project
     * fallback and as a seed for other not-yet-migrated projects. */
    static char s_migrated_root[512] = {0};
    if (std::strcmp(s_migrated_root, s_current_project_root) == 0) return;
    std::snprintf(s_migrated_root, sizeof(s_migrated_root), "%s",
                  s_current_project_root);
    if (jce_fs_host_exists_file(out)) return;   /* project copy wins */
    char legacy[1024];
    if (jce_editor_dotjce_path("benchmark_baseline.txt", legacy,
                               sizeof(legacy)) &&
        jce_fs_host_exists_file(legacy)) {
        char dir[1024];
        std::snprintf(dir, sizeof(dir), "%s/.jce", s_current_project_root);
        jce_fs_host_create_directory(dir);
        jce_fs_host_copy_file(legacy, out);
    }
}

void baseline_save(int kind, int count, double avg_fps, double low1_fps, double p99_ms)
{
    char path[1024]; baseline_path(path, sizeof path);
    if (!path[0]) return;
    /* Project .jce/ dir may not exist yet (fresh project). */
    if (s_current_project_root[0]) {
        char dir[1024];
        std::snprintf(dir, sizeof(dir), "%s/.jce", s_current_project_root);
        jce_fs_host_create_directory(dir);
    }
    /* read existing, drop the matching key, append the new line */
    char lines[64][160]; int n = 0;
    uint64_t raw_size = 0;
    char *raw = (char *)jce_fs_host_read_all(path, &raw_size);
    if (raw) {
        std::istringstream input(std::string(raw, raw + raw_size));
        jce_fs_buffer_free(raw);
        int k, c;
        std::string line;
        while (n < 64 && std::getline(input, line)) {
            if (sscanf(line.c_str(), "%d %d", &k, &c) == 2 &&
                k == kind && c == count) {
                continue;
            }
            std::snprintf(lines[n++], sizeof lines[0], "%s\n", line.c_str());
        }
    }
    std::string output;
    for (int i = 0; i < n; ++i) output += lines[i];
    char current[160];
    int current_len = std::snprintf(current, sizeof current,
                                    "%d %d %.3f %.3f %.4f\n",
                                    kind, count, avg_fps, low1_fps, p99_ms);
    if (current_len > 0) {
        size_t written = (size_t)current_len;
        if (written >= sizeof current) written = sizeof current - 1;
        output.append(current, written);
    }
    jce_fs_host_write_all(path, output.data(), output.size());
}

Baseline baseline_load(int kind, int count)
{
    Baseline b = {0,0,0,false};
    char path[1024]; baseline_path(path, sizeof path);
    if (!path[0]) return b;
    uint64_t raw_size = 0;
    char *raw = (char *)jce_fs_host_read_all(path, &raw_size);
    if (raw) {
        std::istringstream input(std::string(raw, raw + raw_size));
        jce_fs_buffer_free(raw);
        int k, c; double a, l, p;
        std::string line;
        while (std::getline(input, line)) {
            if (sscanf(line.c_str(), "%d %d %lf %lf %lf",
                       &k, &c, &a, &l, &p) == 5 &&
                k == kind && c == count) {
                b.avg_fps = a; b.low1_fps = l; b.p99_ms = p; b.found = true; break;
            }
        }
    }
    return b;
}

Baseline s_baseline = {0,0,0,false};   /* cached for the active workload */

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
    if (spawned != s_ft_last_spawned) {            /* fresh workload */
        ft_reset();
        s_ft_last_spawned = spawned;
        s_baseline = baseline_load(spawned_kind, (int)spawned);
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

    JceGpuStats st;
    if (jce_renderer_get_gpu_stats(&st) && st.valid) {
        const double frame_ms = st.cpu_frame_ms > st.gpu_ms ? st.cpu_frame_ms : st.gpu_ms;
        const double fps = frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0;
        ft_push((float)frame_ms);
        stat_row("benchmark.fps",      "%.0f",    fps);
        stat_row("benchmark.cpuFrame", "%.2f ms", st.cpu_frame_ms);
        stat_row("benchmark.gpuFrame", "%.2f ms", st.gpu_ms);
        stat_row("benchmark.draws",    "%.0f",    (double)st.num_draw);
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("benchmark.noGpuStats"));
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
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("benchmark.reset"))) ft_reset();
        stat_row("benchmark.avg",      "%.0f FPS", ft.avg_fps);
        stat_row("benchmark.onePctLow","%.0f FPS", ft.low1_fps);
        stat_row("benchmark.p99",      "%.2f ms",  ft.p99_ms);
        ImGui::TextUnformatted(jce_editor_i18n("benchmark.minmax"));
        ImGui::SameLine(190.0f);
        ImGui::Text("%.2f / %.2f ms", ft.min_ms, ft.max_ms);

        /* Baseline + regression: save the current distribution, compare to a
         * baseline captured (possibly on a previous build) for this workload. */
        if (spawned) {
            if (ImGui::SmallButton(jce_editor_i18n("benchmark.saveBaseline"))) {
                baseline_save(spawned_kind, (int)spawned, ft.avg_fps, ft.low1_fps, ft.p99_ms);
                s_baseline = baseline_load(spawned_kind, (int)spawned);
            }
            ImGui::SameLine();
            if (s_baseline.found) {
                const double dfps = s_baseline.avg_fps > 0 ? (ft.avg_fps - s_baseline.avg_fps) / s_baseline.avg_fps * 100.0 : 0.0;
                const double dp99 = s_baseline.p99_ms  > 0 ? (ft.p99_ms  - s_baseline.p99_ms)  / s_baseline.p99_ms  * 100.0 : 0.0;
                const ImVec4 col = (dfps >= -2.0) ? ImVec4(0.5f, 0.9f, 0.5f, 1.0f)   /* within noise / better */
                                                  : ImVec4(1.0f, 0.5f, 0.4f, 1.0f);  /* regressed */
                ImGui::TextColored(col, "%s: %+.0f%% FPS, %+.0f%% p99",
                                   jce_editor_i18n("benchmark.vsBaseline"), dfps, dp99);
            } else {
                ImGui::TextDisabled("%s", jce_editor_i18n("benchmark.noBaseline"));
            }
        }
    }

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
