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

The default output is `build/validation/benchmark/`, which is ignored by Git.
An explicit `--out` still selects the caller's output root. Existing
`reports/benchmark/` history stays in place; this default change does not move,
recreate or delete it. See [the generated-output contract](../contracts/build-output-layout.md).
Every run
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

CPU phase names retain their compatibility semantics: `app_simulation` measures
the application update callback, `app_draw` measures its draw callback, and
`app_update` accumulates both as the legacy combined metric (plus their dispatch
measurement overhead). These are nested wall-time observations, not additive
frame components or process CPU execution time. Do not add `app_update` to either
child, or sum all phases to infer frame cost. Use the measured `frame_wall`
series for whole-frame timing. The Editor Profiler shows each phase in ms;
its bars are scaled relative to the longest phase, with no summed percentage.

Optional `JCE_FRAME_LOG_CONTEXT=1` adds a fourth `context_json` field to the
engine's private raw frame CSV. Only `frame_wall` rows contain the fixed v1
packet. Empty or `0` keeps the legacy three columns and adds no window queries;
`2` and `3` select the explicit variants below, and unknown selectors refuse
the capture before opening its file. The packet
records window presence, observed begin/end flags, cached point/drawable sizes,
native pixel sizes at render begin/end, and backend frame indices. Failed
queries are null, distinct from known zero flags. Cached dimensions are not live
native dimensions; backend indices are not GPU completion or presentation.
`read_frames` validates all rows, including unselected tails, and returns aligned
`frame_context`, `context_schema` and the actual `csv_header_bytes` separately
from the unchanged timing series. Writer admission must match the requested
schema and complete payload accounting. Preserve unknown, resized and unfocused
frames and their timing spikes; this packet alone does not certify FBO quality,
continuous OS foreground, unobscured pixels, physical GPU identity or fences.

Selector `2` retains these window facts and at most four independent CPU
viewport packets. Selector `3` adds exactly one `clock` object with domain
`jce-performance-counter` and canonical uint64 decimal strings `frequency_hz`,
`begin` and `end`; the reported schema is `window_viewports_clock_v3`. Every raw
clock interval must match its serialized `frame_wall`, use one frequency, and
not overlap its predecessor. Counter gaps remain explicit. This Engine counter
axis is not Unix time, GPU/Present time or automatic external calibration, and
does not join asynchronous backend statistics or establish continuous coverage.

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
Every Editor control and measurement uses an `editor-profile` child directory
inside its fresh evidence directory. Its visual witness and timing process share
that isolated profile. The resolved absolute `JCE_EDITOR_PROFILE_DIR` is recorded
in the intent before launch; both logs must confirm that exact selected path.
Missing, wrong or conflicting profile markers invalidate the Editor run. Player
environments remain unchanged. The tool does not change HOME or snapshot/restore
the user's settings.

Core-5 uses a fresh `process-cwd/.config/jce.ini` for every control and run.
It clones the effective repository `.config/jce.ini`, falling back to the
executable's `.config/jce.ini` or engine defaults. Every original byte is
preserved as a prefix. Final C-INI sections append `logging.level=debug` and the
fixed requested `renderer.backend`, matching the explicit backend environment
request. The engine uses the last assignment; colon-only rows stay ignored,
and duplicate sections, comments, case and literal percent signs stay intact.
The fixed backend prevents AUTO from rewriting the clean default configuration.
`--backend auto` is rejected for this diagnostic protocol. NUL input is rejected
because the C loader would stop before the appended overrides.
Scene, log, capture and Editor profile paths remain absolute. Witness and timing
processes share this isolated CWD and config; control and workload require
matching baseline and copied configuration hashes. Intents and manifests retain
those hashes, the selected log level and the diagnostic observer mode.
Actual copied config bytes must match the recorded SHA immediately before and
after each child, including an exceptional child exit. A changed or missing copy
rejects the run; the tool never silently reseeds it. Completed observer metadata
retains labelled pre/post SHA checks and the declared override dictionary under
`config_validation_policy=byte_exact_before_and_after_each_child`.

These are DEBUG-instrumented diagnostic Release measurements. They do not
represent quiet production throughput: additional DEBUG traffic may affect
frame times and must be quantified from the actual pilot logs. The exact scatter
materialization gate remains mandatory. The shared `perf_bench.run` helper's
optional `cwd` argument leaves other callers at the repository ROOT by default.

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

## Process residency and tagged memory

`memory_residency_probe.py --exe <application> --cwd <working-directory>
--env-file <explicit-environment.json> --out <new-directory> --warmup 15
--window 45 --timeout 90` samples the existing process sampler. The environment
file is a string-to-string JSON object; the caller specifies the workload,
normal exit after the observation window and hidden/nonfocused window policy.
The tool does not invent an engine-specific workload or log inherited secrets.
It records artifact, explicit environment and validator SHA256 identities,
rejects incomplete/gapped windows and requires a normal exit. Preserve raw
samples and logs; startup peaks and steady residency are separate measurements.

RSS/private commit are process observations, not allocator or GPU ownership.
Unsupported allocator counters remain unavailable, including their documented
zero convention; zero is not evidence of an empty heap. Voluntary tagged
accounting is another scope: `jce_mem_profile_get_total` retains the legacy sum
of individual tag peaks, while `jce_mem_profile_get_aggregate_totals` returns
the actual simultaneous tagged peak since initialization/reset. Introspection
labels this scope and preserves the legacy tag peak sum separately. Individual
snapshots are thread-safe; separate calls do not promise a shared observation
instant. None of these observations alone certifies the 512 MiB device target.

## Continuous CPU lifecycle capture

Set `JCE_TRACE_STREAM=<output.json>` before launching the application. Native
hosts default to a worker consumer; Web hosts use cooperative pumping. For an
explicit control, set `JCE_TRACE_STREAM_MODE=threaded` or `cooperative`. Capture
uses the existing bounded producer ring and per-thread IO suppression. A startup
snapshot cannot establish that shutdown or later frames were retained.

`lifecycle_trace_report.py <trace.json> --require <actual-service-phase>
--out <report.json>` validates explicit required service phases. Repeat
`--require` for the needed phases. Continuous capture must have successful IO,
a stable generation, exact event accounting, zero lost/unexported records and
no active threads/work/waits after producer and consumer joins. Preserve raw
failed attempts and cancellation diagnostics; required phases must succeed.
Use a run exceeding ring capacity to test sustained consumption and preserve
startup plus shutdown/thread-exit records in its original JSON. These are CPU
observations, not GPU completion or process/device memory reclamation. Formal
frame-rate and residency comparisons run with tracing off; enabled coverage
runs and their artifact identities remain separate.

Tracing and frame telemetry use nonempty environment flags whose first character
is not `0`. An explicit `JCE_TRACE=0` overrides automatic tracing from
`JCE_PERF_LOG` or a Debug build, while a nonempty `JCE_TRACE_STREAM` or
`JCE_TRACE_EXPORT` path explicitly requests capture. For an uninstrumented
control, set `JCE_TRACE=0` and clear both capture paths. Set `JCE_PERF_LOG=0`
to disable optional frame telemetry; KPI export has its own declared cost.
Empty strings do not enable these flags. Record effective settings, not merely
the supplied environment.

Timed raw capture must include the final completed rendered interval. The engine
omits initialization, exports the prior render at normal iteration entry and
exports the terminal current render before closing its writer. Explicit quit or
host destruction retains pending completed work. Check contiguous raw ordinals,
final rendered count and independently observed native clock coverage. Preserve
partial-window failures even when accepted/written accounting matches. A declared
capture prefix remains distinct from a whole-duration measurement.

Single cooperative Web captures can preserve producer `tid=0`. Acceptance
requires continuous cooperative mode with no worker consumer, one named thread
and all events sharing that identity. Threaded, mixed, unnamed or ambiguous
zero identities remain invalid; task IDs, outcomes and completeness checks are
unchanged. The validator reports this logical identity scope without inventing
an operating-system thread ID or rewriting the original JSON.


### Optional exact disk history for long offline traces

`lifecycle_trace_report.py` retains its default bounded memory history (200,000
identities). For a larger complete trace, explicitly supply all three arguments:

```text
python -B tools/lifecycle_trace_report.py TRACE --require engine.log_quiesce --cleanup-ownership --terminal-session-binding --out REPORT --history-db ABSOLUTE_FRESH_DB --disk-history-max-records 8000000 --disk-history-max-bytes 536870912
```

The canonical file API uses `history_db_path`, `disk_history_max_records` and
`disk_history_max_bytes`. `observe_terminal_ack` accepts the separate explicit
`trace_history_db_path`, `trace_disk_history_max_records` and
`trace_disk_history_max_bytes` triple, only with a requested trace. It still
requires its final successful cleanup/session match through that same canonical
reader. No external prevalidated report is accepted. Existing
`terminal_trace_max_history` defaults/range and all other RAM caps are unchanged;
a disk allowance does not enlarge shader-pending, thread-clock or active state.

The fresh SQLite index stores every submitted identity/parent, completed-service
membership and terminal-session identity exactly. Its shared row budget counts
tasks plus sessions, not completion updates. Integers are exact decimal-BLOB
keys and timestamp JSON scalars retain integer/float types. Observer finish scans
stream every historical task; no IDs are trimmed or approximated. The database
must not already exist, nor may its known journal/WAL/SHM paths exist.

This is a disposable validation index, not crash-resumable/durable evidence.
Journal and mmap are off, page size is 4 KiB, the cache target is 8 MiB and writes
commit in batches of 4096. `max_page_count` enforces the page-rounded main-file
byte ceiling; fixed queries use indexed lookups/insertion-order scans without
external sorts. A small byte budget can fail while creating schema. Any storage,
budget, commit or close failure makes validation invalid, with no memory fallback
or PASS. Failed indices stay retained and cannot be reopened by this route.
Receipts distinguish SQL-accepted counts from durability and close-returned from
unverified cleanup; a configured cache target is not total process RSS.

Full EOF, all event/schema/pair/observer checks, capture-loss/generation checks,
and the existing two complete source-byte fingerprints remain mandatory. Even
rejected validation retains raw source identity evidence after a storage failure.
Disk history cannot repair an incomplete/lost trace or prove GPU completion.


### Optional reader loss diagnostics

A continuous capture may include version1 `reader_diagnostics`. The same canonical
metadata validator checks its exact typed fields; older captures without the block
keep their original path. Five read-stop counts cover caught-up,512-event batch
limit,slot busy,reserved-unpublished and representable invalid arguments. Entry
write/earliest/cursor/count and publication predicates are checked against declared
capture capacity and the shared reader's cursor/loss arithmetic. Retained adjacent
snapshots must continue the same cursor; no event history is reconstructed.

Only first/last loss observations and the last read snapshot are retained. Raw
cursor loss totals/read ordinals,exact unsigned widths,consumer/final-drain phases
and previous snapshots are validated. `observed_ns` is checked process-performance
clock time after detecting loss, not the overwrite instant; false clock validity
requires0 and remains unavailable. Stop4 is a typed diagnostic state, not an extra
completeness gate. Original loss/generation/active-tail/full-EOF/two-fingerprint
and ACK gates still apply; this block can never promote a failed partial capture.
It supplies no CPU/GPU clock join,frame execution or performance conclusion.


### Separate opt-in Windows build containment

`tools/build/windows_build_job.py` is an explicit BUILD-only wrapper, not a
benchmark/app runner or a jce.py integration. It takes exact argv/cwd and a fresh
output directory. Example shape (trusted caller supplies the real build argv):

```text
python -B tools/build/windows_build_job.py --purpose build --cwd ABSOLUTE_CWD --out FRESH_OUTPUT --timeout 1800 --cleanup-timeout 10 -- ABSOLUTE_EXECUTABLE ARGUMENTS
```

The Python API is `run_build_job(argv, cwd=..., output_dir=...,
timeout_s=1800, cleanup_timeout_s=10, poll_s=0.05, allowed_helpers=(),
max_live_processes=64, max_output_bytes=134217728, max_total_processes=8192)`.
An optional CLI `--helper ABSOLUTE_IMAGE SHA256` supplies the same exact caller
policy; at most16 image pins, each capped at256MiB. No helper is allowed by
default. The fresh unnamed nonbreakaway Job contains the initially suspended
command before resume. StdIO inheritance is restricted, outputs are retained,
and receipt.json is atomically published only after all cleanup attempts.

PASS_NATURAL and PASS_PLANNED_HELPER_CLEANUP describe this narrow containment
contract. Planned cleanup is still forced termination, never natural retirement.
Late Job members, unknown survivors, timeout/cancel, query/cleanup/close failures
remain FAIL. Receipt kernel-empty proof and sampled process metadata coverage
are independent. A missing final receipt cannot certify closure. Job containment
is not a security sandbox or evidence of logical service-client exclusivity.

Only exclusively owned process trees/services are eligible. Shared IPC brokers
are unsupported; exact path/hash/birth/Job membership cannot establish their
client isolation. MSPDBSRV/VCTIP helper policies are rejected in this version,
with no trust-me override; this conservative guard is not general broker
detection. Real MSVC builds have not been qualified or integrated. Source35/36
strict failures remain unchanged. No runtime-orphan exceptions, global helper
kill, OS setting changes or undocumented compiler flags are introduced.
Normal-token CPU fixture controls do not certify compiler completion, GPU work,
image quality, timing gains or industrial benchmark acceptance. Keep builds
excluded from formal performance arms and retain independent thermal evidence.
