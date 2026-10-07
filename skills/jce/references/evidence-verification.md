# Visual and timing evidence

Use three steps: define the observable claim, prove the measurement path ran, then inspect and compare the result. Log output alone does not prove pixels, timing or user interaction.

Hidden-window rendering and renderer-free headless execution are different modes. Rendering validation needs an initialized backend even when no window is shown.

Use separate temporary output directories for captures and traces. Match renderer/backend, viewport, scene, effective settings, frame count and simulation clock between comparisons. A screenshot triggered at a simulation timestamp is different from one driven by wall-clock delay.

JCE_FRAME_DT_FIXED controls deterministic simulation steps; JCE_MAX_FRAMES bounds a run. JCE_WINDOW_HIDDEN hides a rendering window, while JCE_MULTI_INSTANCE avoids an existing editor lock invalidating an isolated capture. Check current source for the meaning of each switch before composing a run.

The reusable tools are tools/envshot.py, tools/visual_diff.py, tools/render_parity.py and tools/perf_bench.py. Read their --help instead of inventing arguments. Inspect the actual captured image and reject empty, stale or unrelated output before comparing metrics.

Use elapsed wall-clock measurements for playback/seek claims. For media, observe displayed video time, audio progression, pause/resume and seek completion; advancing the clock while the matching frame is unavailable is not synchronization.

For a new gate, include a failing control. A check that never sees its target, silently skips or exhausts machine resources provides no PASS evidence. Report real failures, unavailable subjects and resource failures distinctly.

## Core-5 editor and Player benchmark

Read tools/BENCHMARK.md and run `python tools/benchmark_suite.py --help`.
Select `--host editor` or `--host player` explicitly with `--exe` pointing to the
corresponding artifact. Preserve actual backend and executable SHA-256; keep
editor and Player measurements in separate groups. Shared workloads use authored
camera and fixed simulation steps. Captures use separate witness processes;
steady metrics use the full wall-clock CSV, not the live UI ring.
Use `--profile publication` for seven repeats and `--compare-backend` for paired
backend A/B. A `--profile soak --soak-seconds 600` run is short diagnostic evidence;
it does not replace the protocol's final `--profile soak --soak-seconds 1800` run.
Keep exported controls, manifests and raw samples in ignored reports/.
Do not stop a concurrent user editor without authorization or label concurrent
measurements exclusive. Ambient stability observations are permitted when their
contention and scope limits are explicit. External-engine, subsystem and visual
fidelity readiness require separate evidence; absent measurements stay unavailable.

Frame CSV export is batched and its `kpi_export` phase remains inside wall-clock
measurements. Default durability flushes occur at startup/shutdown. Use the
explicit `JCE_KPI_DURABLE_FLUSH_INTERVAL` only for a declared diagnostic control;
recurring storage synchronization must not masquerade as rendering cost.
Keep every spike and require complete raw samples plus clean process exit.

## Media picture and seek controls

Match native output PTS with an independent decoder before comparing pictures.
Declare matrix, signal range and chroma reconstruction. Different RGB resamplers
are different reference policies; rerun both before/after controls with the same
policy rather than relaxing a threshold. Retain decoded YUV and use a standard
floating-point inverse when validating fixed-point nearest-chroma CPU kernels.
Odd crop sizes need ceil-rounded 4:2:0 planes, not truncated last rows/columns.

Keep the transport pinned until the current-generation picture and PCM are
ready. Retire a streaming voice before a discontinuous seek and create a fresh
one after readiness; pausing alone preserves source/resampler prebuffering.
Measure actual post-seek picture, transport and consumed-PCM progression.
Consumed PCM is not acoustic speaker latency, and CPU publication is not GPU
upload or editor presentation. Do not infer unmeasured stages from these probes.

### Interactive picture and clock evidence

Camera variable traces alone do not prove the rendered view moved. Compare actual post-input images and picking projections, including the Editor normal event bridge. `JCE_KPI_GAME_EVENTS` is an explicit normalized synthetic QA producer through the normal editor event processor and live viewport gates; never call it physical mouse testing. Frozen WebGL probes must wait for requestAnimationFrame presentation before browser screenshot submission; save only after the rendered picture is visible. Input-image comparisons declare diagnostic overlay exclusions. Check build-card animation across selection changes and consecutive first-placement frames. Performance runs use real dt, no readback/PNG encoding, declared warmup/sample counts and independent repeats; retain CPU/GPU availability, raw spikes, host identities, OS memory samples and competing processes. Different frame rates in a per-frame original change wall-time trajectories and must be reported.

## Diagnostic SDK configuration and readiness

Strict performance probes that parse actual INFO records require a RELEASE SDK. DIST compiles INFO logging out; its compact single-application smoke remains valid delivery evidence, but it cannot supply those diagnostic records. Use the embedded delivery profile unless optional external-VM authoring behavior is explicitly under test:

```bash
python scripts/jce.py sdk --variant release --delivery-profile embedded --profiling off --patented-codecs off --no-debug
```

Keep the delivered DIST and diagnostic RELEASE artifacts and their hashes separate. Do not weaken performance validation or change production logging policy to force a missing-record run to pass. A registered native ABI is not proof that project script classes are embedded.

For asynchronous model loading, wait for actual readiness within an explicit bounded timeout, then collect exactly 120 warmup frames and 600 measured frames for each declared repeat. Import/setup latency and timeout outcomes are separate from steady-state performance; preserve them instead of treating an early readiness probe or a missing performance line as success.

Readiness of a cached model does not prove that the authored scene renders it. Validate actual scene membership, the selected mesh path and transform, and the active camera pose/projection through public SDK observations. Stock hosts expose owner-thread borrowed observations through jce_default_main_get_scene, jce_default_main_get_camera and jce_default_main_get_scene_renderer; never retain or destroy them across shutdown. jce_camera_get_fov returns degrees. Keep actual submitted topology and independent picture controls distinct from cache census.

Modern recreation may intentionally replace frame-dependent source behavior. Distinguish legacy reference captures from normal time-based gameplay; measure accepted simulation time, dropped suspension time and remainder, and verify multiple presentation rates with the same inputs. Do not require unbounded wall-time catch-up after stalls.
