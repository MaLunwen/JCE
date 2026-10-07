# Benchmark methodology

`benchmark_suite.py` measures the five editor stress families already exposed
by Profiler > Benchmark. It does not redefine the workloads. Run it against a
release editor built from the same source revision being evaluated:

```sh
python scripts/jce.py editor --arch x64 --variant release
python tools/benchmark_suite.py --profile quick --kind all --count 1000 --backend d3d11
python tools/benchmark_suite.py --profile standard --kind all --count 1000 --backend d3d11
python tools/benchmark_suite.py --profile publication --kind all --count 1000 --backend d3d11
python tools/benchmark_suite.py --profile soak --soak-seconds 600 --kind instancing --count 65536 --backend d3d11
python tools/benchmark_suite.py --profile standard --kind all --count 1000 --backend d3d11 --compare-backend vulkan
python tools/benchmark_suite.py --profile standard --kind physics --count 10000 --exe NEW_EXE --compare-exe BASELINE_EXE --compare-manifest BASELINE_RUN_MANIFEST
```

The default output is `reports/benchmark/`, which is ignored by Git. Every run
has a unique directory containing the engine log, full long-format per-frame
KPI CSV, OS process samples, manifest, summary and separate stdout/stderr.
Negative controls are also unique and retained. Generated scenes are
content-addressed and immutable. A supplied `--scene` is hashed in each
manifest. The suite file aggregates independent run medians and records A/B
pairs when requested; it never replaces the underlying data.
Executable A/B keeps the backend fixed and alternates AB/BA. The prior manifest
must match the baseline binary hash before its source identity is reused; an
unsupplied baseline source is explicitly unavailable. Backend and executable
comparisons cannot be combined in one pair.

Profiles: Quick has 30 warm-up and 150 measured frames, one repeat. Standard
has 120 warm-up and 300 measured frames, three repeats. Publication has 120
warm-up and 600 measured frames, seven repeats. Every profile uses a fixed
1280x720 Game View and retains its capture for each run/control. Soak has 120
warm-up frames and records every subsequent frame
until its requested wall-clock duration, plus sampled process memory trend.
A/B alternates AB then BA order for each repeat. The ratios describe that
machine and run only; no significance or cross-engine claim is inferred.

The harness isolates inherited benchmark environment variables, runs in a
hidden GPU window, enters Play so the authored camera applies, pins fixed
timestep and TAA phase, requests an explicit renderer and checks the renderer
actually reported by the engine. It requires a clean process exit, fresh
workload marker, contiguous finite per-frame data, target-phase activity and
a negative-control delta. All profiles also require screenshot
liveness across the Game View. This liveness check only shows that
the workload visibly changed the image; it does not certify visual fidelity.
A failed or unavailable run never contributes a performance number.
Captures run in separate short witness processes at the same simulation time,
with matching scene, workload, backend and 1280x720 render size. Timing processes
retain the fixed size without performing readback/PNG encoding. The witnesses
prove initial visual liveness, not pixel correctness throughout the timing run.
OS memory/CPU/IO samples require the optional `psutil` package; its absence is
recorded as unavailable. `--exe` selects a locally built editor on other hosts.

Select `--host player --exe <SDK-built executable>` for the separate Player
track. `tests/consumers/benchmark` consumes the installed SDK and shared diagnostic
scene API; its captures and fixed-size timing runs use the same guards. A
Player marker cannot satisfy the editor gate, or vice versa. Results retain
their host and must not be pooled across tracks.

The generated fixture uses JCE built-in cube and
sphere meshes and is not the shared imported-asset fixture needed for Unity,
Unreal or Godot comparison. The current suite does not produce an official
cross-engine comparison or full
animation/AI/streaming/media coverage. Those gates must be reported as
UNAVAILABLE or BLOCKED, never silently counted as passes. Overall readiness
also requires subsystem coverage and standalone Player parity beyond this
Core-5 measurement suite. Official public sources and reproducible external
scenarios may supply reference comparisons when local runs are out of scope.
Only matched measured workloads support cross-engine speed ratios.
Reference scenarios include [Unity ECS Physics samples](https://github.com/Unity-Technologies/EntityComponentSystemSamples/tree/master/PhysicsSamples),
[Unreal City Sample](https://dev.epicgames.com/documentation/en-us/unreal-engine/city-sample-project-unreal-engine-demonstration)
and [Godot's official benchmark repository](https://github.com/godotengine/godot-benchmarks).
Pin both engine and scene revisions before attempting matched local timings;
sample assets' own licences apply and are not blanket cross-engine permissions.
The physics fixture measures free fall without a floor; its bodies can leave
the camera frustum during a long Soak. It is not a sustained contact/solver or
sleeping-stack measurement. Soak time windows and OS resource counts are
exported; missing engine heap/queue/object counters remain unavailable.
Quick is a fixed-frame smoke profile and may be shorter than two seconds on
fast hardware. Publication here means seven-repeat capture/export, not an
approved external-engine publication or a complete readiness verdict.
Frame times include KPI export and editor overhead. Optional CPU/GPU values
are the renderer's latest asynchronous statistics; exact alignment with the
wall-clock frame has not been established. Whole-window size and automatic
quality settings can still depend on the host; effective pipeline log lines
are retained in each manifest. Thermal, power, GPU-driver and queue/heap
metadata need independent collection before a cross-engine publication.
Primitive cube/sphere scatter retains a 65536-instance limit per component.
The diagnostic API splits larger fixtures into deterministic bounded chunks;
the runner requires complete exact renderer-count groups, including remainder
chunks, for each viewport. Model-scatter tiled streaming is a different
workload and needs its own controls and asset provenance.

The SDK Player's project-specific CPU matrix is available through
`python tests/consumers/benchmark/tools/probe_suite.py --help`. It records jobs,
transforms, hierarchy, graph A*, behavior trees, raycasts and incremental scene
loading separately from frame timings. Sustained contact and deliberately
disabled physics populations are separate controls. Analytic 64-joint chain
sampling/blending at 10/100/500/1000 characters verifies every output matrix;
its CPU interval excludes pose checks, GPU upload and rendering. These probes do not certify hardware
utilization, editor UI or a world-streaming camera route. Player frame runs also
sample public engine and GPU resource counters at initialization, exit and
five-second monotonic intervals; unsupported counters retain availability flags.

Scatter fixture tint is intentionally red to distinguish subpixel instances
from a neutral background without weakening the image-control threshold.
The primitive scale is two units, matching the scatter cell spacing, to retain
raster coverage under antialiasing at large counts and fixed resolution.
This fixture revision has a different source identity from earlier measurements.
Transform-independent renderer metadata is keyed separately from world/bounds
updates. Compare `JCE_DISABLE_ECULL_METADATA=1` when diagnosing this path;
the consumer's warmed opaque-to-blend witness also requires equal cached and
uncached images, both different from the unchanged scene control.
Introspection memory totals count explicitly tagged allocations only; zero
does not prove zero heap use. Player JSONL additionally preserves separately
scoped process RSS and committed-memory snapshots.

`benchmark_report.py --suite <suite.json> --references <references.json> --out <new-directory>`
revalidates PASS frame summaries against their raw CSV, exports PNG/SVG plots
and a Markdown report, and retains failures without fabricated zero bars.
Suites, hosts, scene hashes and binary hashes remain separate groups. External
reference cards and coverage use a separate labelled table/image; they cannot
be substituted for actual matched timings. Matplotlib is required for plotting.
Scoped CPU suites are revalidated from completed-work counts and integer clocks,
and appear on separate charts with per-result interval definitions. File route
waypoint latencies are distinct from frame stalls and GPU upload timing.
