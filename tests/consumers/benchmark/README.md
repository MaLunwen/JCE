# JCE Benchmark Player

This is a C99 application built against the installed SDK, independently of the
editor. It exercises the same scene renderer and runtime used by editor Play.
Results from this Player and the editor remain separate measurement tracks.

Build through `python scripts/jce.py app tests/consumers/benchmark --arch x64 --variant release`.
The runner supplies an authored scene with `JCE_SCENE` and an optional diagnostic
workload with `JCE_BENCH_AUTOSPAWN=kind,count`. Kind values are 0 draw calls,
1 instancing, 2 triangles, 3 moving entities and 4 dynamic physics bodies.
An absent workload is the negative control. Malformed input aborts initialization.

`JCE_BENCH_PLAYER_SHOT` requests one offscreen PNG at the twentieth displayed
frame. Formal timing runs leave it unset. The viewport is fixed at 1280 by 720.
GPU tests run on an unmapped window with `JCE_WINDOW_HIDDEN=1`; `JCE_HEADLESS=1`
instead exercises CPU services without creating a GPU device.

Owned source follows the repository license. Generated primitive fixtures use
no external assets and do not establish cross-engine canonical-mesh equivalence.

The separate `JceBenchmarkDefault` target delegates to the installed SDK's
default application host, including its SSR/fog/post-processing composition.
Build it with `python scripts/jce.py app tests/consumers/benchmark --variant release
--target JceBenchmarkDefault --exe jce_benchmark_default.exe` (one command).
`tools/postfx_suite.py --exe <binary> --backend <backend> --out <scratch>` checks
a first-party PBR room with empty/repeated controls and one enabled effect.
Submitted view records alone cannot pass; each effect must change real pixels.
This is a correctness preflight, excluding TAA motion/convergence and timing.
Generate/check the scene inputs with `tools/postfx_fixture.py --out <directory>`.
`tools/postfx_transition_suite.py` takes the same explicit binary/backend/output
arguments and compares warm SSAO/TAA/base-to-SSR and SSR disable with independent
steady controls. The test policy selects a transition at displayed frame 30;
ordinary runs never change gates. `tools/report_postfx.py --suite <suite.json>`
(four occurrences) `--exe <archived-binary> --scenes <retained-inputs>
--out <new-directory>` rehashes the
completed matrix and plots actual pictures and pixel differences, not timings.
For intermediate diagnostics, the custom Player accepts `JCE_BENCH_RT_TARGET`
as `gbuffer.normal`, `gbuffer.albedo` or `reflection.ssr`; this opt-in enables
SSR and samples the selected public intermediate into the compatible HDR capture.
The stock target instead accepts that name and `JCE_BENCH_RT_OUTPUT=<prefix>`
to observe RGBA8 through the typed readback queue, preserving row origin and
request/completion frames. Neither observer uses private renderer/host state.

The CPU-only probe runner exercises the public job scheduler, transform and
hierarchy APIs, sliced scene loader, graph A*, behavior trees, contact/raycast
physics, skeletal sampling/blending, file streaming and Canvas layout:

```bash
python tests/consumers/benchmark/tools/probe_suite.py --exe PATH_TO_PLAYER --probe all
python tests/consumers/benchmark/tools/probe_suite.py --exe PATH_TO_PLAYER --matrix
```

The default retains ten measured rounds. Core workloads use two warm-up rounds;
contacts settle for 600 fixed steps and streaming warms one complete route.
Initialization is excluded. Each result declares its own interval scope and
whether validation is excluded. A round must complete the exact requested work and pass
its independent result checks. Integer clock ticks/frequency, checksums, CPU/RSS
samples and attributable logs are retained under ignored reports/. Job concurrency
counts scheduler participants including the creator; the actual background worker
count is logged separately. Scene loading measures incremental parsing/fixups of
an already decoded document; it does not claim disk I/O or a streaming-world route.

`--matrix --probe NAME` selects that workload's fixed scale/shape matrix. The
`world-stream` probe loads real scene files along a deterministic 16-cell route
in cooperative and asynchronous modes. It includes polling and entity checks,
uses a warm OS cache, and does not measure GPU uploads or rendered-frame delay.
Raw waypoint intervals and generated fixture hashes accompany each result.
`ui-layout` measures headless Canvas layout/raycast records at 1k/10k elements,
excluding output-rect checks, text shaping and GPU drawing.

`JCE_BENCH_TEXT=multilingual` supplies English, Chinese, Arabic and mixed labels
through the public Canvas API. `JCE_BENCH_TEXT_FONT` and optional
`JCE_BENCH_TEXT_FALLBACK` are explicit system-font paths. Fonts are not bundled.
`tools/check_text_pixels.py --exe PATH_TO_PLAYER --font PATH_TO_FONT
--fallback PATH_TO_FALLBACK --backend d3d11 --out OUTPUT_DIRECTORY` captures
the four labels against a blank control using the public post-to-bridge Canvas
composition. Pixel liveness is automatic; review the retained image for joining,
RTL order and fallback coverage. This does not certify arbitrary mixed-direction
paragraphs or isolate shaping, upload and cached-draw costs.

`tools/check_asset_pixels.py` takes an explicit GLB, clip, scale and expected
vertex/joint/clip counts plus the SDK packer. It packs the supplied asset directory
into a verified local PAK and attaches it through the existing resource fallback
chain; original asset bytes stay unchanged. It compares empty, frozen, advancing and repeated
captures while checking renderer-owned import state and an animation clock.
Every matching animator's published normalized clock and receiver layer are also
checked; a capacity warning fails the run. Multiple characters use an elevated
orthographic camera. Pixel deltas cover every RGB pixel, excluding alpha, with
a maximum channel difference threshold of 30. These revised captures must not
be pooled with the former sparse-sampling/frontal-camera results.
`--layers 0,1 --light-mask 1` authors alternating receiver layers with a light
restricted to layer 0; `--crowd off` supplies the ordinary submission control.
`--min-chroma` explicitly guards colored assets against a white silhouette.
`JCE_BENCH_SHOT_FRAME` optionally selects a capture frame (default 20).
This is visible skeletal liveness, not an independent GPU deformation oracle
or proof of separate playback state for every character. See
[source and terms](SOURCE_AND_TERMS.md) before acquiring external fixtures.

`tools/time_asset.py --exe PATH_TO_PLAYER --witness PATH_TO_RESULT_JSON --out OUTPUT`
records whole Player frames only after the same executable passes all character
pixel controls. Every projected cell must contain coverage and motion; capture,
scene and PAK hashes are rechecked. The default uses 120 warm-up intervals and
300 measured intervals for each of three repeats. Initialization/import and PNG
readback are excluded. Update, render, post/composition and KPI export remain in
scope. GPU statistics are delayed backend records, not aligned CPU/GPU pairs.

`tools/check_layer_witness.py --masked RESULT --solo RESULT --all RESULT --out JSON`
checks the 100-character alternating-layer controls. Layer 0 must remain unchanged
and every layer-1 cell must respond to the restricted light. Submission parity
uses the existing static pixel tolerance and retains exact differences explicitly;
it does not claim byte-identical rasterization between batched and ordinary paths.

`tools/culling_suite.py --exe PATH_TO_PLAYER --count 200000 --out OUTPUT`
assigns one third of SDK-created cubes to known front, offscreen and behind-camera
groups. It verifies exact public culling counts, visible pixels, on/off image
parity and repeat noise before interleaved whole Player timings. The camera is
fixed, motion and shadows are disabled, and the visible grid scales to avoid
duplicating thousands of overlapping large boxes. Non-rendering camera/light/root
records remain conservatively visible. This isolates CPU frustum policy rather
than Hi-Z or independent per-object pixel visibility. `--witness-only` is preflight.

`tools/occlusion_suite.py --exe PATH_TO_PLAYER --count 128 --out OUTPUT`
uses the public hardware-query culler with a fixed wall and equal hidden/visible
groups. Static enabled/disabled/repeat captures must match. The wall then moves
at frame 80: captures at 79, 80, 81, 82 and 90 must match disabled-query controls
at each corresponding frame. Reveal differences use a 5-unit RGB threshold
because lightly shaded cubes can differ from the background by less than 30.
Query overflow and unavailable hardware retain their real scope. Static-only
diagnostics are not full acceptance; this path is separate from GPUScene/HiZ.

`tools/gpu_model_suite.py --exe PLAYER --packer SDK_PACKER --asset STATIC_GLB
--vertices EXPECTED_VERTICES --count 6000 --backend d3d11 --out OUTPUT`
compares CPU instancing, fixed-count compute culling, compacted indirect and
indirect plus HiZ using the known front/offscreen/behind placement. Actual
uploaded inputs, dispatches and submission readiness must agree with requested
modes; captures must agree with CPU output and repeated controls. This is
static correctness preflight, excluding timings and GPU survivor readback.

`tools/gpu_history_suite.py` takes the same explicit asset/tool arguments and
defaults to 128 imported objects behind/beside the existing wall. Hardware
queries stay off. CPU and HiZ captures must agree at frames 79/80/81/82/90,
including every object's independently projected cell after the wall moves.
This certifies sampled reveal pixels, not independently counted GPU savings
or arbitrary camera, deformation and streaming history. Failed captures remain
in the output instead of being filtered from evidence.

`tools/gpu_time_suite.py --exe PLAYER --witness STATIC_RESULT --out OUTPUT`
requires same-binary static evidence for CPU, fixed-count compute, indirect and
HiZ. Original resource logs and all front cells are rechecked, then all four
modes rotate through seven repetitions of 600 intervals after 120 warm-up
intervals. No screenshot is scheduled in timed processes. Finish compiler,
source gates and plotting jobs first; retain every long frame. GPU timing fields
are delayed backend measurements and must not be paired with individual CPU
frames. This does not replace a GPU survivor-count measurement.

The asset timing runner accepts `--compare-durable-flush-interval 60` to interleave
the same executable's normal KPI export with explicit storage synchronization.
`kpi_export` records export overhead; all original spikes stay in the raw CSV.
Normal capture flushes at startup/shutdown. Only clean, complete runs are valid;
the frame CSV is not a crash-durable audit journal.

`JCE_BENCH_COUNTERS` selects the full JSONL resource log. Initialization, five-second
intervals and exit preserve engine counters, process memory and GPU resources.
Entity-culling, model LOD and model GPU-driven records retain their enabled and
supported flags. Model-batch counters do not measure foliage compute dispatches.
Tagged allocation totals are opt-in accounting, not total live heap memory.
Backend GPU usage may cover the whole adapter (D3D11/NVAPI), so its growth
alone is not an application leak verdict. Keep resource counts and process
memory alongside it, and retain the backend-specific measurement scope.

`tools/media_suite.py --exe NATIVE_VIDEO_PROBE --file MEDIA --audio-device
--resume-seconds 2 --seconds 30 --repeats 3 --out OUTPUT` retains muted real
device clocks, paused preview, exact midpoint seek and continuous resumed
picture/PCM progression. Add `--preview 1280` for a bounded-preview workload;
native resolution is the default. Run this matrix after builds and source gates,
without competing benchmarks. Source cache ownership and process peak RSS are
different measurements. The probe restores native resolution after preview
before the resume control; peak RSS therefore includes that restoration.
`tools/report_media.py --suite RESULT --out NEW_DIRECTORY` rechecks assets and
actual probe logs before producing playback, seek and resumed-clock charts.
These probes do not certify editor GPU upload or acoustic presentation latency.

`tools/media_pixel_suite.py --exe NATIVE_VIDEO_PROBE --file MEDIA --ffmpeg
REFERENCE_DECODER --ffprobe REFERENCE_INSPECTOR --out OUTPUT` independently
matches decoded PTS before comparing raw RGBA. `--oracle nearest-yuv` retains
decoded YUV and evaluates standard floating-point matrix/range equations with
nearest chroma reconstruction; default FFmpeg RGB uses its own reconstruction.
`tools/media_color_fixtures.py` creates procedural SDR controls under an explicit
scratch directory. `tools/report_media_pixels.py` requires identical assets and
reference policy for comparisons and recomputes differences from retained bytes.
These tools use external reference executables only for validation; the native
odd-size AV1/VP9 regressions use committed first-party bytes without FFmpeg.

`tools/gbuffer_asset_suite.py --exe PLAYER --packer SDK_PACKER --box UNIT_BOX_GLB
--fox ORIGINAL_FOX_GLB --out NEW_DIRECTORY` tests actual normal-buffer output on
four backends. Original inputs remain unchanged. Box must match the equivalent
primitive exactly in RGBA; Fox must pass empty, frozen, moving and repeated
captures and actual topology/clock markers. This is normal/animation liveness,
not timing or an independent skin/material fidelity oracle. Source metadata,
scene/log/capture hashes and failed outputs stay with each trial.

`tools/report_gbuffer_assets.py --suite BACKEND_SUITE --out NEW_DIRECTORY`
requires four actual backend results, rehashes original assets and the archived
executable, and revalidates backend/topology/clock logs and normal-buffer pixels
before exporting the comparison. Missing or altered evidence fails reporting.

`tools/lod_suite.py --exe PLAYER --backend d3d11 --out NEW_DIRECTORY`
uses 64 equivalent planar meshes with 2048/128/2 triangles and a fixed
outward/return camera route. Real global LOD picks must match an independent
distance/hysteresis oracle for every frame. Empty, disabled, enabled and repeated
captures validate visible geometry and unchanged equivalent-surface pixels.
This is global distance-selection correctness, not cooked per-entity LOD, GPU
triangle counters, streaming cost or formal GPU/frame timing acceptance.

`tools/report_lod.py --suite BACKEND_SUITE --archived-exe PLAYER --out NEW_DIRECTORY`
requires four distinct actual Windows backends. It rehashes the supplied tested
executable and all scene/log/capture artifacts, reparses every selection frame,
and recomputes empty/disabled/enabled/repeated pixel controls before plotting.
Raw capture orientation is retained and reported; within-backend correctness
must not be described as cross-backend display parity or a throughput result.

`tools/light_suite.py --exe PLAYER --backend d3d11 --out NEW_DIRECTORY`
checks one hundred independent point/spot receivers with disabled, ten, hundred,
omitted, repeated and reordered-sentinel controls. It retains one hundred
authored components in every case; passing establishes light participation,
not light-count throughput, shader occupancy or frame performance.

`tools/shadow_suite.py --exe PLAYER --backend d3d11 --out NEW_DIRECTORY`
checks directional shadow pixel liveness with empty, disabled, enabled, repeat
and moved-caster/unshadowed movement controls. Effective cascade/map settings,
independent shadow silhouettes and per-cascade timing remain separate acceptance
requirements. Both suites preserve raw identities and partial failure manifests.

`tools/report_lights.py --suite BACKEND_DIRECTORY/suite.json --out NEW_DIRECTORY`
requires four repeated --suite arguments with distinct actual Windows backends
and the same executable. It rehashes raw scenes/logs/captures and recomputes
every pixel control before exporting the participation heatmap and count table.
This visualization shows correctness, not engine speed or lighting throughput.

Particle evidence is explicitly opt-in through the existing observation/statistics
flags. Its finite public logger calls and actual public backend-name observation
survive DIST; ordinary DIST logging remains quiet. `tools/particle_process.py`
shares OS sampling, fresh execution-log merging and KPI parsing with maintained
tools, without requiring the stock host's unavailable aggregate perf record.
All three particle runners require a clean completed process, observed backend,
complete consecutive frame CSV and unchanged typed-count/pixel controls. These
observer-on results are correctness diagnostics, not throughput certification.

## Public GPU timing readout

With the refreshed installed SDK, both consumer hosts retain opt-in
`JCE_BENCH_COUNTERS=<absolute JSONL path>`. The default target now links the same
metrics source and records successful initialization, every 5 seconds of draw
callbacks, and exit before stock scene/renderer teardown. Its frame source is
`default_host_draw_count`; the unavailable stock simulation clock is null. The
custom Player update clock/event protocol is unchanged.
Use the independent `gpu_timing` object for qualified intervals. It records the
public getter's availability/reason, signed decimal-string `timer_frequency`,
`raw_begin`, `raw_end`, exact unsigned-string `context_generation`, numeric
`gpu_source_frame` and `read_host_frame_index`, and `duration_ms` or null with
`UNAVAILABLE`. Preserved `gpu` numeric fields are unqualified legacy observations.
These latched samples are not an engine/GPU frame join, fence or presentation.
Retain repeated sample identities and the raw JSONL; do not infer fresh GPU work
from a changed caller counter. Observation/serialization adds diagnostic cost.

Build the stock target through the existing command with an explicit current SDK:
`python scripts/jce.py app tests/consumers/benchmark --sdk <install> --arch x64
--variant release --target JceBenchmarkDefault --exe jce_benchmark_default.exe`.
The existing `tools/postfx_fixture.py --out <scratch>` supplies a first-party room.
For an external diagnostic, copy its `base.scene.json` into the executable's
mounted cooked root at `resources/_cooked/qa_gpu_timer/base.scene.json` (use the
project's actual `cooked_assets` root if configured). The stock host consumes
`JCE_STARTUP_SCENE=qa_gpu_timer/base.scene.json` through that runtime VFS; it does
not consume the custom Player's `JCE_SCENE` or read an absolute host scene path.
Require the startup log to confirm the mounted loose source and exact virtual
address. This loose fixture is diagnostic input, not embedded-asset closure or
Dist product-admission evidence. A bounded readout also uses
`JCE_BACKEND=d3d11`, `JCE_BENCH_POST_EFFECT=base`,
`JCE_BENCH_COUNTERS=<scratch>/gpu.jsonl` and `JCE_MAX_FRAMES=<bounded count>`.
Hidden-window rendering is optional through
`JCE_WINDOW_HIDDEN=1`; do not set `JCE_HEADLESS` for physical GPU observation.
Leave screenshot/readback/trace/KPI observers off unless a separate diagnostic
requires them. Retain the actual log/backend, clean exit, executable and SDK
hashes, consumer source identity and original JSONL; this is not a performance
benchmark or an uncontended-machine claim. The generic custom Player
`player_counters` validator intentionally rejects this distinct default draw
clock, so do not relax it to admit a null simulation clock.

The excluded targets `JceBenchmarkMetricsWire` and
`JceBenchmarkMetricsMissingWire` and `JceBenchmarkMetricsOldFrameReplace` compile
the whole actual metrics owner with typed
SDK-call ports and real SDK JSON. They link the installed SDK's sanctioned
`JCE::Core` native-test target; product targets retain `JCE::JCE`. Build them with the existing `app` target
selection or the already configured consumer build's target selector. The first
must exit zero with eight groups; both ordinary negative controls must fail
assertions. The existing `JCE_HEADLESS=1 JCE_BENCH_PROBE=metrics-selftest` custom
Player route also checks the real encoder's new precision/unavailable groups.
None of these CPU controls establishes physical GPU timer support.

Only a frame-zero explicit `initialized` or `default-initialized` event replaces
the JSONL file. A successful initialization followed by zero draw callbacks still
appends its exit record, preserving both lifetime endpoints. Public JSON child
attachment is void-returning: local object allocation failures are cleaned up,
but this transport does not certify complete successful encoding under all OOM.
