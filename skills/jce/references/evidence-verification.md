# Visual and timing evidence

Use three steps: define the observable claim, prove the measurement path ran, then inspect and compare the result. Log output alone does not prove pixels, timing or user interaction.

Hidden-window rendering and renderer-free headless execution are different modes. Rendering validation needs an initialized backend even when no window is shown.

Use separate temporary output directories for captures and traces. Match renderer/backend, viewport, scene, effective settings, frame count and simulation clock between comparisons. A screenshot triggered at a simulation timestamp is different from one driven by wall-clock delay.

JCE_FRAME_DT_FIXED controls deterministic simulation steps; JCE_MAX_FRAMES bounds a run. JCE_WINDOW_HIDDEN hides a rendering window, while JCE_MULTI_INSTANCE avoids an existing editor lock invalidating an isolated capture. Check current source for the meaning of each switch before composing a run.

The reusable tools are tools/envshot.py, tools/visual_diff.py, tools/render_parity.py and tools/perf_bench.py. Read their --help instead of inventing arguments. Inspect the actual captured image and reject empty, stale or unrelated output before comparing metrics.

Use elapsed wall-clock measurements for playback/seek claims. For media, observe displayed video time, audio progression, pause/resume and seek completion; advancing the clock while the matching frame is unavailable is not synchronization.

For a new gate, include a failing control. A check that never sees its target, silently skips or exhausts machine resources provides no PASS evidence. Report real failures, unavailable subjects and resource failures distinctly.
