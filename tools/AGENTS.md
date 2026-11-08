# tools/ — reusable automation

Scene restoration evidence normalizes full handles only at known serialized
reference sites. Script.params entries with integer kind JCE_SCRIPT_PARAM_ENTITY
use the same bijective scene handle map as the owner and hierarchy. Keep zero
as none, reject unresolved nonzero targets, preserve every other parameter
field, and do not infer script actions from a matching restored scene.

`--renderer-submissions` preserves the four identity fields in
`renderer.submit.*`. Its optional `renderer.submit_clock.*` packet holds three
ordered exact integers (frequency, begin, end), measured around the real
backend API call. The matching `engine.renderer_tail_clock.*` packet belongs
to the broader engine tail and records the wrapper return. The engine checks
device admission separately after the wrapper. A nested
`renderer.submit_cpu` service and its same-thread admitted API child identify
the pair. Require matching clock frequency and raw interval containment before
reporting a join. Missing clocks or a missing child leave the join unavailable.
Preserve the prior report for old traces. Raw clocks are CPU API intervals;
a separate OS trace must establish scheduler activity, GPU completion, Present,
or composition. Keep loss, thread and process ownership checks authoritative.

`readback_operation_observation.py` owns opt-in `--readback-operations` /
`readback_operations=True` in the existing canonical lifecycle trace reader.
It validates thirteen ordered `readback.op.*` fields inside the submitted
`readback.operation` CPU service and joins operation/context/ticket only within
one capture. Submit, observed promised-frame readiness, cancellation intent,
actual destination release and release after backend stop are distinct.
Zero and UINT32_MAX are valid frame serials; only READY uses half-range ordering.
Unlinked captures retain unavailable identity; unknown context never becomes zero.
Active packets plus pending operations share the existing max_active budget.
Outstanding entries are capture observations, not global leaks. Backend stop does
not invent ready evidence, a fence, device-idle or Present completion. Defaults
retain their output shape; the existing lint aggregate runs its memory/stream/CLI
and ordinary disconnected-consumer controls in tools/tests/.

Shader stock profiles extend the existing contracts/shader-keywords.json
and tools/shadergen/gen_shader_variants.py owner. Generated C acquisition data
and engine/shaders/stock_profiles.json must agree. Full cooking stays explicitly
unrestricted; its stock-acquire table is not an exhaustive list of optional
subsystem shaders. The compatibility cook list includes current Editor init
acquisitions, not proof those programs run on every GL2.1 device. Preserve the
existing modern generated shader/public-header bytes when changing only a
compatibility selector; the registered shader-stock controls enforce this.

`lifecycle_trace_report.py --shader-provenance` (Python
`shader_provenance=True`) feeds `shader_provenance_observation.py` through the
same canonical event/B/E/EOF/loss/generation/fingerprint authority. Defaults do
not import this observer or add its result key. The ordered render-category
create/load_link/creator_release/shared-link packets have16/8/6/6 exact typed
fields; uint32 words reconstruct exact IDs/context and digest-order SHA256.
Service B/E is self-owned; counter parent is the submitted caller. A load_link
requires its same-thread nested, already closed actual link child, whose real
handles, result, creator-release count and outcome remain authoritative.

Only prior closed successful create receipt IDs with matching capture/thread/
caller/handle and available context/payload can be consumed; two acquisitions
may share a handle, and no global handle incarnation is inferred. Known stage
receipts must have the same context and distinct acquisition IDs. Consumption
deletes pending state; active and pending state use existing active/history
budgets. Missing packets stay UNAVAILABLE, nonzero reasons require zero receipt
IDs and retain gaps, and unresolved successes are observations, never leaks.
Legacy shared links do not acquire eager-loader provenance. Finite service/
outcome/reason counters and first/last witnesses validate all later packets.
`tools/tests/test_shader_provenance_observation.py` covers the actual memory,
stream and CLI wiring plus an ordinary disconnected-consumer assertion control.
These synthetic CPU controls do not certify native buffer hashing, actual bgfx
calls, final program references, GPU execution/completion or presentation.

The existing renderer-cleanup vsync fixture retains actual public entry bodies
and all guard/argument assertions. It includes the actual private launch state
and extracts both complete launch/presentation resolvers; eligibility is never
reimplemented by a fixture stub. The reset observation dependency is a typed
backend-forwarding port using the actual private reason enum; complete window
observation and trace production belong to the Native real-ring renderer fixture.
The old unconditional-stats wrapper must compile before its ordinary runtime
count assertion fails. Supplied constraint state does not certify native window
eligibility, a fence fix, presentation, trace publication or GPU performance.

The Play presentation source fixture keeps its counted operation-observation
port typed to the public trace task state. This port does not replace business
results or produce trace evidence; real private scopes and bounded-ring closure
belong to the dedicated Native Editor controls. All presentation/rollback wires
and ordinary runtime-failure controls remain intact.

Automated build, asset generation, analysis and validation implementations live
here. They take explicit inputs, derive repository paths from their own location,
produce generated work under ignored build/ and final deliveries under dist/,
and carry no secrets. Existing explicit output overrides and legacy reports/
history stay valid. The prospective layout and retention boundaries are owned by
contracts/build-output-layout.md. Core-5 defaults to build/validation/benchmark/;
its existing unique run/control/suite identities and immutable fixture checks
remain unchanged. tools/tests/test_generated_output_defaults.py exercises the
actual Python owner paths and old-default/ignored-override assertion controls;
these CPU ports do not execute a native benchmark.
Windows build-Job tests create temporary work under build/tests/windows-build-job/
and clean only their own fresh directory. JCE_TEST_WINDOWS_BUILD_JOB_RECEIPTS
keeps its explicit retained-root behavior; process containment policy is unchanged.

The trace-capture contract helper derives its full-tail input from the actual
ring capacity declaration. Its short-budget control changes only the extracted
production drain bound back to 32 batches; an ordinary executed assertion must
reject the incomplete final drain. Keep independent fixed-capacity reader ports
as declared inputs rather than silently rebasing them to the production default.
Its typed clock/parent/sequence-validity ports retain exact production consumer
and conversion functions. Balanced zero-loss counters with an exhausted sequence
still refuse publication, and a separate ordinary mutation removes only that
final admission predicate. These ports do not certify native counter atomics/TLS.

`tests/test_game_view_backend_selection.py` compiles strict extracts of the actual
Game View backend helpers and complete selector/banner block with the maintained
startup resolver and public renderer enums/declarations. Counted typed UI/config
ports observe canonical selection, unknown previews, persistence and genuine
restart hints. Old string selection, restart and save controls compile and fail
ordinary assertions; neither those ports nor a compiler run certify physical UI
input, backend startup or hot swapping.

- build/: build driver, installed-SDK acceptance and pristine source acquisition.
- lint/ and audit/: source, ABI, dependency, format and policy gates.
- scriptgen/ and shadergen/: generators from maintained contracts.
- provenance/: generic content hashes and externally supplied signatures.
- benchmark_suite.py / benchmark_scene.py: separate guarded Core-5 editor/Player runs and
  deterministic fixtures. process_sampler.py owns engine-neutral OS sampling;
  perf_bench.py owns process/backend/log validation and derives native artifact
  paths from the build driver's target matrix. See BENCHMARK.md. The SDK-only
  Player host and its scenario/capture policy belong to tests/consumers/benchmark.
  Optional named live markers record readiness while the child is running;
  stale files and exit-only observations cannot become startup latency.
  Every run retains its own PID and optional psutil creation time (Unix seconds,
  not an exact native birth token). Per-thread cumulative CPU is opt-in and
  caps retained output at 1..1024 rows per poll; default runs never query it.
  Missing or denied thread observations retain aggregate process samples and
  explicit availability. Observed TIDs do not certify complete thread lifetimes,
  account for exited threads, identify thread names or prevent TID reuse.
  Windows GPU rates are separately opt-in via --sample-gpu in
  memory_residency_probe.py / run_process(sample_gpu=True). perf_bench.run also
  forwards sample_gpu=False, gpu_limit=256 and gpu_peer_pids=() through that same
  owner. Its shared validate_gpu_options call runs before log/CSV deletion,
  directory creation or child launch; keep the default result shape unchanged.
  The existing process receipt retains actual GPU output paths and availability,
  not derived FPS. test_perf_bench.py checks counted forwarding, invalid-before-
  touch admission and an executed omitted-forwarder ordinary assertion control.
  These CPU controls do not query PDH or prove native GPU work. The lazy private
  windows_gpu_observation.py owner uses an English PDH wildcard and formatted
  array with two collections at least one second apart. Only the launched PID
  and explicit --gpu-peer-pid subjects with stable observed psutil births are
  retained. Preserve priming, unavailable/denied/invalid, measured zero, raw
  status, PID/LUID/engine identity, query duration and output truncation. A row
  cap does not cap OS enumeration. Do not sum engines or derive FPS, GPU memory,
  presentation, fences, cross-adapter copies or DWM causality. Close the query
  on every exit; only the launched child is owned for kill/reap. Non-Windows or
  disabled runs never load PDH; typed DLL controls do not certify real counters.
  An empty OS engine label with a complete PID/LUID/physical/engine structure
  is nullable metadata (engine_type=null, EMPTY_OS_LABEL), not an invalid rate
  or an inferred engine category. Preserve its original counter qualification;
  structurally incomplete selected names retain bounded diagnostic witnesses.
  perf_bench.run retains strict periodic-perf admission by default. Explicit
  JCE_PERF_LOG=0 controls may select raw CPU CSV admission; the existing parser,
  exact requested frame window (or timed-limit marker) and writer record/payload
  accounting must agree. Missing periodic metrics are unavailable, never zero.
  This is a combined periodic-telemetry policy and cannot certify GPU execution.
  jce_determinism.parse_perf owns both modern MiB/N/A and legacy MB parsing.
  Unavailable memory stays None with raw token/availability retained; modern
  zero is known, while legacy clamped zero is LEGACY_ZERO_AMBIGUOUS. Both formats
  used binary conversion. Preserve declared source scopes without inventing
  scopes for historical logs. test_perf_bench.py exercises this actual parser
  and ordinary same-owner Python failures; it launches no application or GPU.
- benchmark_report.py: explicit suite inputs, raw-CSV revalidation, separate
  identity groups, static plot export and public-reference coverage. References
  are labelled separately from measurements and cannot become timing bars.
  benchmark_probe_report.py validates scoped CPU workloads from integer clocks
  and completed work. CPU batches retain their scope and never become frames.
- render/media/asset CLIs: general measurements, cooking and inspection.
  media_preview_probe.py runs real hidden-window editor playback under an
  explicit JCE_EDITOR_PROFILE_DIR. Its default fresh profile lives beside the
  output; --profile supplies an intentional reusable profile. It never snapshots
  or restores the user's HOME settings and requires actual profile confirmation.
  Playback cadence intervals end at actual seek-generation events or paused
  snapshots. Clock-delta heuristics must not join seek waits or split away slow
  playback. Keep failed controls and report short unmeasured intervals honestly.

A dedicated game's generator, palette, fixture or packaging default belongs to
its consumer project. Reproduction tools and engine regression fixtures remain
under tests/consumers/<project>/ or the corresponding native test module.
Unpublished AI workflows live under private/tools and are optional; reusable
SDK/editor capabilities never depend on their presence. No automated recipe
may depend on scratch docs, a user's home configuration or a copied vendor tree.

SDK binary embedding uses the existing jce_bin2obj host tool for both PAKs
and bundles; the packer only produces archives. JCESDKHelpers activates ASM
at directory scope for native non-MSVC consumers, including prebuilt assembly.
The wrapper owns COFF C-symbol decoration and preprocessed assembly target
sections, symbol prefixes and size_t width. Recipe mocks do not certify a
platform linker; retained native SDK-consumer evidence is still required.

`tests/test_renderer_owned_cleanup.py` extracts the actual callback factory,
device-loss fatal handler and atomic consume owner for its adversarial CPU
interleave control. Unrelated capability publication is a typed, explicitly
isolated port; the actual factory reset calls remain intact. The complete-vtbl
Native renderer fixture owns capability metadata, observed begin counts and
publisher behavior. Its factory-terminal port consumes the actual private
attempt-result enum, preserving the defensive call without pretending to test
terminal publication. This Python fixture proves neither those facts nor GPU
device loss, backend completion or native thread scheduling.

The material cache-admission source fixture extracts actual private custom
trace types and uses typed isolated observation ports. Its cache/link/cleanup
and warning-reset assertions remain authoritative for that CPU subset; the
Native custom-source fixture owns actual ring publication and FS/PAK provenance.

Shader compilation keeps each native tree's generated output isolated.
compile_shaders.cmake routes through shader_cache.py when its existing Python
interpreter is available; otherwise the direct compiler path remains. The cache
uses the shared build/cache_lock.py kernel lock and verifies each blob checksum.
Keys include compiler bytes, complete arguments except the output destination,
and declared source/varying/include bytes. A verified byte-identical local output
keeps its timestamp, including when a conservative input change recompiles to
the same bytes. Ninja restat can then avoid downstream packing. Compiler failure or changed inputs
must preserve the previous local output. Never seed it from unverified old
shader binaries or copy native build trees; JCE_SHADER_CACHE_DIR selects an
explicit alternative cache location.

Offline shader payload export uses `shader_payload.py --inspector <native-tool>
--input <existing.bin> --out <new-payload>`. It never builds an inspector or
shader. The opt-in `jce_shader_inspect --json --payload-hex` obtains its range
from the sole public `jce_shader_reflect` parser and emits ASCII hex, preserving
default JSON and text-only `--code` behavior. The wrapper verifies the bounded
container, inspector identity and returned byte range before exclusive `xb`
creation. Failed writes remove only a still-identical file created by this call;
pre-existing paths and replacements are preserved. Unknown file identity retains
the partial file conservatively. Native controls require an explicit
JCE_TEST_SHADER_INSPECT, use the actual CLI, and compare legacy output with
JCE_TEST_SHADER_INSPECT_BASELINE when supplied. CPU export/reflection and DXBC
disassembly do not certify an application's active draw, shader cost or GPU time.

The C99 jce_bundle_pack CLI only forwards public JceBundlePackOptions to the
existing resource packer. Explicit cooking warnings are failures: raw fallback
is never claimed as successful cooking. Missing selected scenes fail early.

Private tools/cli/jce_tool_main_sdl.c is the sanctioned CLI entry bridge. Only
this tool bridge includes SDL_main; consumers implement SDL-free jce_tool_main.
It does not initialize SDL subsystems, renderer, engine or an application loop.
bundle/pak/cook/bin2obj share the UTF-8 entry. Legacy host path streams use
private jce_tool_io_sdl.c, preserving malloc buffer ownership and bounded archive
inspection. Directory enumeration and atomic replacement reuse public host FS;
unchanged packed bytes retain their mtimes. No platform path API appears in tools.
The object target is linked into the host binary, not an installed public API.

Host asset tools share cmake/JCEHostAssetSources.cmake offline implementations.
The bundle CLI must not link renderer/scene/resource layer targets merely to
reach the public packer; retain real cooking and VFS behavior without shaders.

Web SDK archive embedding uses --embed-file plus SINGLE_FILE so code and MEMFS assets live in one HTML executable. Verify deployment with only that artifact; preloaded companion data is not single-file delivery.

SDK embedding packs authored .jce/project-settings.json under settings/project-settings.json beside the runtime boot contract. Configure dependencies refresh edits, and absence removes stale generated settings; host working-directory settings are not a shipping dependency.

Lifecycle trace acceptance uses lifecycle_trace_report.py with explicit required
CPU phase names. `--require NAME` requires a successful submitted service task
(category task, kind 2, matching service owner); it never accepts a same-name
wait. `--require-wait NAME` independently requires a complete category wait B/E
pair through the same full event validator. Wait observations do not certify
successful target work. Both controls are repeatable; required task failures and
cancellations still fail when waits complete. Ring overwrite, reset and missing
outcomes cannot certify a complete lifecycle. Keep raw input SHA and report the
measured subset; it is not
a GPU completion, all-service or resource-leak certificate.

Known `sequence_exhausted` metadata must be the boolean false. Exhausted or
mistyped state rejects acceptance even when counts balance and the producer
claims completeness. Legacy absence remains absent rather than being upgraded
to an observed false; both bounded-file and in-memory readers share this gate.

The canonical lifecycle reader additionally retains each raw TID's last event
timestamp and rejects a decrease, including between completed services or after
thread-name metadata. Equal ticks and globally out-of-order events from different
threads remain valid. Thread-clock identities reuse the explicit history cap;
they are never evicted to hide rollback. This gate does not relax B/E ownership,
discard restart prefixes or normalize invalid captures. The ordinary omitted
clock-gate control and real SDL restart evidence distinguish producer clock
correctness from successful pair/count/footer validation.

The explicit `--cleanup-ownership` / `cleanup_ownership=True` gate consumes
`cleanup_ownership_observation.py` through that same event and B/E authority.
It requires a complete continuous capture and exact kind-2
`engine.cleanup_attempt` services. Each attempt has exactly one core
`cleanup.reason` then `cleanup.completed` counter, bound to its active
id=parent/TID and interval. Integers are strict: reason0..5, completed0..1;
reason0/completed1 requires succeeded state0, reasons1..5/completed0 failed
state1. Earlier refusals remain diagnostics; the final captured scope-close
must succeed, with no submitted attempt left without its pair. Nested reentry
is associated per attempt, not by a global last counter. Active packets reuse
the existing limit; all submitted identities stay under canonical history
admission. Output retains fixed reason counts and first/last closed witnesses.
Generic `--require engine.cleanup_attempt` is unchanged and still rejects any
failed attempt. Defaults do not certify cleanup. Full EOF/loss/footer/source
identity checks remain mandatory. This marker has no stable Engine/session
identity and precedes the capture/SDL/allocation tail: it proves neither all
owner retirement nor process exit, leaks or GPU completion. Actual analyzer
CPU controls cover recovery, nested/interleaved ownership, malformed packets,
full-stream failure and the ordinary omitted-final-gate negative control.

The source-extracted Vsync guard/count control in
tools/tests/test_renderer_owned_cleanup.py keeps launch-policy resolution and
observation as typed non-subject ports. It retains every actual setter/reset
call and prior assertion. The separate Native real-owner/reset/ring fixture is
required for launch-policy behavior; these source ports do not certify it.

The explicit `--renderer-submissions` / `renderer_submissions=True` option feeds
`renderer_submission_observation.py` from the same canonical streaming event and
active B/E owner. It requires at least one successful submitted
renderer.frame_advance_cpu packet. Each of the four renderer.submit fields must
occur exactly once, in producer order, within that same task id/parent/TID and
wall interval, with render-category counters. Generation -1 remains unknown;
the other generation values are positive int64, while backend serials and
ordinal are uint32 including zero and wrap. Never coerce floats or booleans.
Only closed packets contribute; retain bounded active fields and first/last
output witnesses, with wide integers encoded as decimal strings. Default mode
does not consume or certify these packets. The CPU controls in
tools/tests/test_renderer_submission_observation.py exercise the complete
canonical reader, missing/malformed data, EOF/loss and the ordinary old-unwired
owner. These CPU boundaries cannot establish GPU completion, presentation or a
join to independently latched GPU observations by themselves.

The opt-in `--gpu-submission-joins` / `gpu_submission_joins=True` consumes those
validated CPU packets through the existing canonical reader and requires an
exact match to a complete scoped global snapshot from the pinned D3D11
provider. The snapshot's nested CPU advance must match its epoch, returned
API serial and host ordinal (before + 1). Its independent GPU source serial
must identify one accepted earlier/current advance in that epoch. A first
host advance can follow provider initialization frames. Subsequent observed
serial and ordinal history must be contiguous. Reject wrap, epoch reentry,
conflicting repeats, malformed times and ambiguous ownership. Allow zero
serial and count pre-capture source misses, unavailable intervals, unknown
epochs and unverified backends separately. The active scope and retained CPU
identity budgets each follow max-active. No silent eviction. Retain first/last
distinct joined witnesses, with repeated identical latch samples counted
separately. A match identifies sampled provider-reported ticks. GPU query
disjoint/device-lost validity, hardware completion, OS Present and DWM
attribution stay unavailable. Do not align this result with CSV or ETW by
ordinal, clock guess or current API frame. The opt-in emits no CPU submission
summary unless separately requested and leaves default reports unchanged.

The opt-in `--render-pipeline-observations` / `render_pipeline_observations=True`
validates successful `render_pipeline.publish` service scopes and their
ordered `rp.*` counters through the same reader. It requires a complete
publication and recognizes one exact packet shape per capture. Legacy packets
report their missing new fields as unavailable. Full packets supply recorded
policy inputs and decisions. Counts and first/last witnesses describe policy
publication, without certifying pass execution, visual quality or performance.

The opt-in --offscreen-configurations / offscreen_configurations=True feeds
offscreen_configure_observation.py from that same canonical event/B/E owner.
Each complete successful packet has 22 ordered render counters with resource
serial as id and configure service as parent. Exact signed-int64 creation-serial
wire bits reconstruct uint64; wide output integers remain decimal strings.
Other offscreen resource scopes retain their own identity observations. Empty
renderer.view_configure scopes also cover stock 3D setup and stay unclassified
by outcome; they do not prove missing or complete offscreen coverage. Partial
packets, failed scopes with fields and orphan fields reject; an explicit request
with no complete packet fails rather than reporting NO_ACTIVITY acceptance.
The consumer keeps bounded active fields, finite counts and first/last packets,
reusing canonical submitted/completed identities without another resource ledger.
Full EOF/loss/generation/source checks remain mandatory. Raw format enums and
CPU configuration tuples do not establish backend acceptance, GPU completion or
the final displayed texture. tools/tests/test_offscreen_configure_observation.py
owns focused canonical controls and the ordinary missing-admission-wire failure.

The opt-in `--screenshot-requests` / `screenshot_requests=True` consumes
`screenshot_request_observation.py` through the same complete event reader.
The renderer.screenshot.admit/terminal names and optional cpu_pixels marker
require render counters,
nonzero exact uint64 file identities, parent zero, mode0/1 and the actual
SUCCEEDED/FAILED/CANCELLED wire outcomes0/1/2. Strictly increasing admission
serials and a max-active bounded unclosed map reject replay, missing, duplicate,
orphan and per-identity reversed edges without retaining closed-ID history.
An optional cpu_pixels marker belongs to one active mode-0 request and records
its callback thread, timestamp, raw index and uint16 framebuffer. Old captures
may omit it. File terminal status remains independent from pixel readiness,
GPU completion and presentation.
Terminals may follow later admissions or close in another thread/order; no
global nonoverlap, timestamp sort or task lifetime is invented. Output retains
fixed mode/outcome counts and first/last terminal witnesses; wide identities
use the existing exact-integer encoding. A complete continuously enabled
capture and all canonical footer/loss/generation/source checks remain required.
No matching requests reports NO_ACTIVITY and activity_observed=false, never
screenshot acceptance. Defaults do not consume or certify the ledger.

`tools/tests/test_screenshot_request_observation.py` is registered in the public
source aggregate and exercises memory/stream/CLI paths plus the ordinary
unwired-owner failure. Its explicit --native-trace FILE mode reuses the same
observer for the small real Core writer protocol output and deliberate mutated
edges; it starts no native process or compiler. The separate renderer Native
fixture owns real PNG callback outcomes, and product runtime captures must
close the actual request-to-consumer chain. Protocol/writer controls establish
neither an end-to-end PNG callback, GPU completion nor file durability.

The separate opt-in `--gpu-view-observations` / `gpu_view_observations=True`
feeds `gpu_view_observation.py` through that same canonical event reader and
active B/E owner. Its actual producer is the successful `renderer.submit_cpu`
service, not `renderer.frame_advance_cpu`. Each `gpu.view.<uint16>.` record
requires exactly nine ordered fields, matching task id/parent/TID, render
category, scope interval, int64 clocks/epoch and uint32 API/submission/source
identities. A per-view source frame is never joined to the current CPU/CSV frame
or forced equal to other views' source frames. The interval formula is the
producer's positive frequency, nonnegative begin, end>=begin and nonzero pair:
positive equal endpoints are measured zero; an all-zero pair is unavailable.
Reported ticks must agree, and unavailable durations/sums remain null.

Only known `(device_epoch, view_id)` groups retain aggregates and one previous
raw signature. Raw source frame, begin/end and frequency distinguish adjacent
repeats; CPU/API advancement alone does not create a new GPU interval. Duration
statistics omit adjacent repeated signatures only, never claim globally unique
intervals or per-frame means, and do not sum different views. Epoch -1 samples
are counted but not grouped or compared, and break comparison continuity.
First/last global witnesses preserve bounded raw identity and exact integers
beyond53 bits as decimal strings. Active scopes/records reuse `--max-active`;
retained epoch/view groups reuse `--max-names`. Exceeding either refuses the
capture rather than truncating or evicting history. Explicitly requested missing
view packets fail; complete packets with no available intervals explicitly
report timing unavailable. Default captures retain their previous contract.
The CPU controls in `tests/test_gpu_view_observation.py` exercise real canonical
memory/stream/CLI paths and ordinary missing-wire assertions, not physical GPU
execution, presentation, fences, all-frame coverage or hardware completion.

The lifecycle report's explicit `--require-counter-zero NAME` checks every
matching actual C event; `--require-counter-last-zero NAME` checks the last in
exported stream order. Missing counters and boolean, string, null or nonfinite
values fail. Defaults do not infer counter requirements. Keep raw indices and
values in the report; these controls cannot replace a resource identity ledger
or hardware completion proof. Real scoped counter IDs may be nonzero.

instance_upload_trace_report.py validates the captured common instance-owner
resource subset, matching token/allocation identity and epochs, committed slices,
bindings, retirement/destruction and staging ownership. No successful reservation
is NOT_EXERCISED (CLI 2); cancelled reservations may form a closed subset without
successful uploads, so retain committed/bound counts separately. An explicitly
required but absent dynamic route fails. This is not an all-renderer resource,
GPU-completion or physical-memory certificate.

The bundle command accepts an explicit --shader-root for installed SDK stock binaries. It is a fallback after project/VFS sources and only resolves canonical shaders/*.bin addresses inside that root. SDK shipment admission validates the stock catalog against its actual embedded-pack provenance before passing the root; never admit unknown programs or substitute arbitrary filenames for absent dependencies.

shader_stock.py stages compiled shader bytes from the current engine PAK BOM;
its content checksum is XXH32(seed=0), with SHA256 for installed identities.
Only verified owned stale stock files may be retired, and unknown binary files
reject the operation. editor_lifecycle_probe.py observes real hidden Play/Stop
phases and full scene restoration independently of performance INFO logs; it
never converts missing timing evidence into a performance PASS. Optional --trace
requests a continuous CPU lifecycle stream in the probe's UUID evidence directory, without
forcing worker mode. Artifact hashes include the saved stream; actual required
phase coverage is checked separately by lifecycle_trace_report.py, not inferred
from requesting a trace or from Play/Stop success.

Lifecycle scheduling stays in that probe: --frames admits 240..10000;
--play-at, --observe-at and --shot-at use editor ticks, while --stop-at retains
the existing PLAYING-tick count. --observe-at admits 3..16 increasing ticks:
first BEFORE, last AFTER, and every middle tick DURING. One strict validator
runs before IO and derives earliest_stop_editor_tick=play_at+stop_at; DURING
and the shot precede it, and AFTER must be strictly later. Deferred viewport
admission can delay Play/Stop, so actual positive phase rows remain authoritative.
The plan binds environment, phase checks, snapshot names and result metadata;
every middle scene uses the existing restoration/mutation comparison. Multiple
middles add scene_controls in that order and retain the first as scene_control.
The fixed 0.016666 simulation step and whole-window capture remain diagnostic
controls, not native FPS evidence. Pure Python controls exercise extended CLI
plans through actual readers/validators with only the owned child port replaced.

Process residency diagnostics reuse process_sampler.py. memory_residency_probe.py
requires a covered warmup/observation window, normal exit and stable artifact
identity; missing sampling is unavailable. RSS/private commit never imply
allocator, GPU ownership or low-memory device certification. Explicit caller
environment owns workload and hidden-window policy; inherited secrets are not
written to evidence.

memory_commit_probe.py and windows_memory_reader.py retain one process handle,
identity checks and the full VirtualQueryEx walk. Explicit --mapped-file-names
adds bounded K32GetMappedFileNameW metadata for at most 32 complete MEM_MAPPED
allocation groups. QUERY_INFORMATION | SYNCHRONIZE remains the access mask;
VM_READ and process-content reads are unnecessary. One fixed 32768-character
buffer and a 0.5-second inter-call budget bound query work; an entered OS call
cannot be interrupted. Missing, truncated and failed names remain explicit,
with Win32 errors where available. Name status is separate from base walk
certification and totals, and never establishes heap, driver or GPU ownership.

Host PAK tools link existing core trace/timer/logger ownership for filesystem
observation. They do not initialize an engine, renderer or async logger; core
logging retains its synchronous fallback. Never shadow jce_log_write or allow
duplicate linker symbols to bypass that owner.

The existing jce_pak argument owner accepts at most 16 --exclude-suffix values,
each at most 31 UTF-8 bytes excluding its NUL terminator. Excess count or length
fails with a specific diagnostic before any pack output is published. Matching
remains case-sensitive; empty suffix values retain their existing no-op behavior.
Real CLI boundary controls use tools/tests/test_host_tool_unicode.py and explicit
host binaries, separately from source contracts, SDK delivery and GPU evidence.

Lifecycle reports retain the submitted task parent separately from scope ownership.
Only a self-owned SERVICE task span must match its submitted name; an owned wait
may have a distinct label. Wait endpoints must still match identity, thread and
owner, and required SERVICE phases must remain successful.

Repository hygiene inspects tracked and visible source by default. Its optional
whole-owned-tree mode protects original dependency roots, nested repositories,
links and the root-only local _workspace reference area. Ignored reference
copies remain immutable; do not repair their line endings to pass a source
gate. First-party owned text and Git index normalization still require checks.

Explicit periodic-log-off performance admission validates the declared raw writer
window and count/byte completion. A timed-limit log marker does not independently
prove that the CSV covers the entire requested duration; callers validate that
horizon separately. Invalid opt-out duration arguments fail before prior outputs
are removed, including when an environment override shadows the argument.

Dist project content is the public Bundle resolver's actual cooked startup-scene
closure, including strict bundle_roots and recursive .deps declarations. The CLI
passes that immutable Bundle to jce_pak --selected-archive; it never regex-prunes
or serializes a second dependency graph and never packs the entire project cooked
folder as a closure. Stock resource trees remain a labelled safe superset until
separately verified. Runtime boot/settings policy is an explicit generated input.
Unknown boot files fail admission instead of joining the shipment silently.

The selected PAK importer admits at most 16 repeatable --selected-archive inputs
through the public Bundle contract loader and Archive reader, sharing one primary
snapshot per input. Read/contract/header/payload failures and unsafe or duplicate
addresses within an input fail closed. Equal addresses across inputs merge only
after byte equality; selected provenance wins stock and the first selected source
is retained across equal selected inputs. Different bytes fail with both labels.
The reserved Bundle manifest is not copied. selected_sources records each input's
identity and imported count before cross-input deduplication; selected_source
retains the single-input form only. Do not sum imported counts as the union size.

Inspect JSON retains jce.pakbom.v1 and adds bundle metadata only after the public
Bundle consumer validates the actual embedded manifest. That embedded object is
the typed authority. A sidecar is optional; when present the public file loader
must admit it, and v1.1 size/hash is bound to the primary snapshot with stable
sidecar bytes. Absent and legacy v1.0 sidecars are not reported as verified.
compatible records public file-loader acceptance for present sidecars; absence
sets it false because no sidecar admission was attempted. Compatible v1.0 does
not prove archive hash binding, and is not rejected solely for verified=false.
Legacy compatibility remains separate from modern graph proof. Inspect/pack
callers must check the process result and deep-verify totals before admitting
output; semantic admission is not runtime mounting, input, or pixels evidence.


`jce_pak --inspect <archive> --read-entry <canonical-path> --out <new-file>`
reads one entry through the existing public Archive/Bundle admission and reader.
The decoded payload is limited to16MiB; this is not a whole-archive IO/memory cap.
The existing typed Bundle target and optional sidecar policies still apply.
Canonical path and unique index identity, header/authentication and recorded entry
CRC must pass before exclusive `jce_fs_host_write_new` publication. Existing
outputs are never replaced; a failed new-file IO/close may retain partial evidence.
Payload and key bytes are not console output. Empty reads require an independently
admitted plain stored mapping; ambiguous codec/encrypted empty records refuse
because the legacy public read result cannot distinguish success0 from error0.


Pack-mode --json emits actual input provenance and content identities, binds every
final PAK entry to its source bytes, and performs the existing key/authentication
and deep checksum verification before generating an embed wrapper. When debug
paths are stripped, entry.path in this build-only BOM is recovered only from the
verified current source-to-Archive-entry mapping. The shipped PAK keeps no debug
path table, and inspect-only without that source ledger keeps path null. Stock
staging still validates each named current-build shader's size/XXH32/verified flag;
it never substitutes SDK/cache inputs or scans filenames to recover opaque names. Missing or
skipped verification is failure. The driver binds fresh graph/sidecar/PAK/BOM/embed
SHA-256 identities in project_assets.receipt.json; this proves a build-time asset
transaction, never Editor Play/EXE/Bundle pixels or performance. Encryption remains
the existing ChaCha20/HMAC-SHA-256 owner, with client-held keys; do not label it AES
or claim that client assets cannot be extracted.

PresentMon QPC-ms capture review uses presentation_report.py. It pins actual v2.6.0
tool bytes, normal target/observer exits, exact PID/name and declared output,
keeps swapchains separate, and refuses malformed/reversed/over-budget CSV.
NA metrics remain unavailable. It deliberately ignores the legacy TimeInSeconds
header under --qpc_time_ms. Presentation cadence and GPU event durations are not
optical refresh, hardware utilization, ETW loss absence or whole-lifecycle proof.
Native-v1 admits an absent msSinceInput only with one canonical standalone
--no_track_input. The disabled collector omits that metric; conflicting columns,
duplicate or noncanonical flags and flag-shaped option values refuse admission.
The explicit observer command selects the CSV schema: a single --v2_metrics
uses CPUStartQPCTime and the native FrameTime/CPUBusy/CPUWait/GPU columns;
otherwise the existing strict legacy/default headers remain required. Conflicting
metrics or QPC/date-time flags reject admission, and an optional receipt
metrics_mode must agree with the command. The pinned upstream CsvOutput.cpp and
README-ConsoleApplication.md define --qpc_time_ms as milliseconds. Preserve source
column names and metric definitions: v2 FrameTime is a CPU-start interval, not
MsBetweenPresents; CPUWait and GPUWait are separate. Legacy metrics absent from
v2 remain UNAVAILABLE/null instead of aliases. CPU-start timestamp cadence is
separate from Present()/display cadence. NA and measured zero stay distinct.
Observed metric summaries use every qualified row for mean, P50/P95/P99/P99.9
and maximum; unavailable summaries explicitly retain null for every statistic.
Tail interpolation uses the same sorted-sample percentile owner in both modes.
Every admitted report, including the default mode, qualifies GPU timing as
unchanged event-derived estimates and leaves HWS state unknown. The CSV alone
is no independent HWS witness. The pinned upstream v2.6.0 README documents
possible late/inflated GPU execution timing with hardware-accelerated GPU
scheduling; retain that source and precision boundary without querying hardware,
correcting raw values or certifying true hardware busy time. Tests cover both
metrics modes, mixed NA/valid zero and unchanged statistics and units. HybridPresent
is not summarized here: a CSV zero without swapchain creation/resize event
coverage cannot alone prove the absence of cross-adapter copies.
tools/tests/test_presentation_report.py exercises the actual parser and three
whole-owner omitted header/mode/unit gates with ordinary assertion failures;
these CPU controls do not certify an actual ETW capture or performance target.

Editor consumption follows explicit public `.inc.h`/`.inc` implementation definitions as well as direct editor tokens. Ordinary public-header declarations do not become consumption; missing/escaping includes cannot supply call evidence. Query-only game-module scene access retains an explicit exemption while Editor Play owns the setter and shared host policy. This remains static reachability, not GUI or runtime proof. Include discovery strips C/C++ comments at both root and transitive implementation edges. It does not evaluate #if 0 or other preprocessing conditions; conditional presence is unknown, and the remaining identifier scan is a weak name signal rather than compiled execution.

Exact scene-canvas failure controls link each mutation in a distinct directory. Never overwrite a just-executed child image or retry a failed compilation. Negative controls must return the known ordinary ownership refusal (40, or 60 for retry Canvas release); a crash, timeout or absent executable is not acceptance.

Bgfx C99 bgfx_set_view_name uses INT32_MAX for NUL-terminated names, not -1. Verify the installed primary API contract before adding raw RHI annotations. SDK Noop smoke does not exercise native startup Splash/ImGui; require an actual normal native startup after these changes, preserving crashes and publication cohorts.

WindowsReader working_set_pages is a separate explicit opt-in on the same query/synchronize handle. K32QueryWorkingSetEx accepts at most 4096 ordered unique aligned page addresses per call and returns raw ULONG_PTR flags. Default-off does not resolve or call that API. Fixed x64 structure layout, closed/missing/error/mutated responses and query bounds remain explicit. Interpret valid-page fields only when Valid is set; AWE/large pages may be outside the ordinary working set. Callers retain selected-address coverage, stable identity and non-atomic QPC windows. This is metadata, with no VM_READ, target-content reads, RSS subtraction or heap/driver/GPU ownership certificate.

`memory_commit_probe.py --working-set-pages` consumes that same reader once.
Only opt-in walks retain committed spans, bounded by the region-query limit;
reserved/free holes never become page requests. Ascending selection defaults
to 262144 pages and a 0.5-second inter-call query budget, with no retry and no
interrupt promise for an entered OS call. Raw flags use a bounded histogram;
valid, invalid, unknown-attempted and unattempted pages remain separate, as do
walk, selection, query, histogram and native-identity/window completeness.
Types label the earlier VQE observation, including IMAGE/MAPPED copy-on-write;
valid-page bytes are not RSS. OS RSS/private commit remain independent before
and after samples. Partial requested page metadata makes the CLI return 2,
while preserving the separate base-walk result and all observed prefixes.
The existing deterministic fixture consumes the actual page method and complete
capture owner; omitted-wire and invalid-as-valid controls must fail ordinary
unchanged assertions. Neither typed controls nor metadata certify heap or GPU
ownership, device memory capacity or an atomic address-space snapshot.

Lifecycle trace required-service aggregates retain min/max/sum of exported elapsed milliseconds and exact integer measured nanoseconds over every paired actual service. Fixed required-name/history limits and full EOF/outcome checks remain authoritative. Keep both clocks distinct; these are inclusive service wall intervals, nested services overlap, and neither can substitute for OS CPU execution or GPU completion. Nonfinite elapsed accumulation refuses the capture.

Bounded-stream lifecycle reports add nonrequired_task_outcomes.aggregates for
all paired failed/cancelled task scopes, grouped by canonical name and state.
Each row retains the full count, elapsed-ms and exact measured-ns min/max/sum,
and only first/last endpoint witnesses in exported event order. Existing name
admission bounds rows to two states per retained name; history/active/name
budgets reject overflow instead of dropping outcomes. Nonfinite diagnostic
elapsed totals also refuse. Keep the legacy in-memory scope lists, CLI status,
required outcomes, complete JSON EOF and all-event pairing checks unchanged.
The existing lifecycle reporter tests exercise mixed services/states, late
repeated failures, budgets, CLI serialization and an executed omitted-wire
variant that must fail ordinary behavioral assertions.

Requested Editor benchmark spawn reports use the common runtime logger in DIST and retain requested versus actual spawned counts. Internal INFO removal still applies; absent/partial workload witnesses cannot yield benchmark performance PASS. tools/tests/test_editor_benchmark_log.py compiles the complete actual Editor spawn body with typed scene-generator/logger ports and an ordinary old-macro failure; Core-5 pixels and measurements remain independent native runs.

The source aggregator accepts `tools/lint/run_all.py --stock-wrapper-exe PATH` for the existing native stock CMake controls. The operator supplies the actual native jce_bin2obj wrapper; no build/dist candidate is guessed. Without that input the specific subject is NOT_MEASURED/SKIP without launching argparse or CMake. Invalid explicit paths/options fail before dispatch, and an explicit child exit 2 is a failed invocation, not SKIP. `tools/tests/test_lint_stock_wrapper_input.py` exercises actual runner argument/dispatch paths with typed child ports; it does not prove native wrapper/CMake behavior. The operator must run the real existing controls with a valid wrapper and CMake, plus the separate architecture suite.

The source runner explicitly registers test_scene_renderer_cache_recovery.py.
It compiles actual scene renderer lookup/poll/invalidate and grass shader/reset
bodies with counted CPU ports; explicit ordinary exit and assertion markers
are required after each removed-wire mutant compiles. The separate native
scene diagnostics fixture uses the real core trace/logger. Neither counted
ports nor CPU lifecycle records certify shader IO, GPU execution or pixels.

Its grass/reset body retains the complete current production reset, including
its typed counted prepass-release dependency and an ordinary removed-wire
control. The real prepass allocation/retirement owner is covered separately by
the native scene fixture; counted dependency calls are not GPU teardown proof.
Its lite grass state declares each field touched by that reset, including
`resource_prepared_valid`; it is a typed port, not the full renderer ABI layout.
Memory source contracts follow the actual split snapshot encoder/publication
caller and localized unavailable display. Distinct simultaneous/tag peak values,
partial tag coverage and unavailable heap data retain real-source mutations;
JSON/filesystem publication remains the separate native Editor snapshot suite.

Selected Bundle packaging and inspection bind --bundle-target to the existing
delivery.bundle_platform target mapping. It is an exact windows/linux/macos/
android/ios/web/auto token, separate from --platform content categories. Web
without an explicit token defaults to web; legacy desktop without one retains
compiled-host admission. Typed mobile/console inputs without an exact token
refuse. Explicit category/token disagreement refuses before input processing;
requested auto is portable-only, never an arbitrary-target bypass. The public
non-mounting inspection loader shares runtime manifest/dependency/hash checks;
game runtime admission remains fixed to its compiled target. BOM records the
requested target separately from validated embedded metadata. The actual tool
fixture includes real self-child CLI disagreement refusals and a same-source
former-host-target wire that must fail ordinary unchanged assertions.

Inspection's borrowed Pak remains readable for verified extraction, but its
immutable owner mark refuses standard SDK named/legacy FS mounting and fallback
attachment. Engine and Bundle register one private canonical provider; the
public three-function provider ABI remains unchanged. Custom opaque providers
and independently reopening bytes are outside that same-handle contract. Do not
claim inspection is a security sandbox or certify runtime execution from it.
NULL-provider FS mounts also refuse; later canonical restoration cannot make
an earlier unregistered inspection mount readable. Pak admission detects cyclic
chains with a finite pointer walk; fallback push refuses shared tails in linear
time before mutation, preserving ordinary disjoint raw-Pak fallback behavior.

## Optional capture writer diagnostics

`terminal_ack_observation.py` is the opt-in host-tail packet consumer used by
`perf_bench.run(require_terminal_ack=True)` and the opt-in
`editor_lifecycle_probe.probe(require_terminal_ack=True)` /
`--require-terminal-ack` paths. The lifecycle probe injects its fresh token only
after environment normalization and validates the exact owned stderr. Requested
continuous traces reuse this same complete cleanup/session binding and report;
resource observations use the existing explicit observer. Exit zero alone cannot
replace a missing or invalid acknowledgement. Default lifecycle output and
Play/Stop/restoration checks remain unchanged. The probe parses frame CSV once
through its existing phase validator and reuses validated stderr/trace SHA256
for its artifact inventory, retaining an unchanged size/mtime guard.
Only this opt-in generates a fresh 32-lowercase-hex request token. The protocol
is read from the exact launched child's stderr path, never merged, ANSI-repaired
or deduplicated logs. Bounded line reads require exactly one complete schema-1
record, matching token, successful application result, confirmed Engine tail
and logger return, and no incomplete trace or failed log-file close. Preserve
the sampler's PID and birth availability without inferring an unavailable birth.
The process sampler stays engine-neutral; normal performance runs are unchanged.

The opt-in perf runner saves a small exclusive `terminal-ack-request-*.json`
before native launch. It retains the expected nonce and owned stderr/trace
paths through wrapper or child failure. It proves the request only. Recover
cleanup by checking that independently retained nonce against the actual
owned process result and canonical ACK; never derive it from stderr alone.

Explicit trace intent requires a continuous JCE_TRACE_STREAM path absent before
launch, disjoint from the other output files; old evidence is not deleted to
manufacture freshness. The existing complete bounded lifecycle reader validates
every event and cleanup packet. Optional `terminal_session_binding=True`
requires cleanup ownership and collects exact `engine.terminal_session`
witnesses during that same canonical event/B/E pass. The same analysis binds
the acknowledgement to its final successful cleanup id and unique session;
only the existing second complete byte fingerprint rereads the trace.
Earlier refused attempts remain admissible. An earlier successful session,
foreign parent/thread, outside-interval marker or duplicate cannot substitute.
Session/cleanup ids stay exact decimal strings. Packet/trace SHA evidence and
raw process outputs remain available; this is not a new trace-pairing owner.

`terminal_trace_max_history` is an explicit optional Python caller budget for
the same complete validator, shared by `perf_bench.run` and the terminal
observer. Omission keeps 200000 retained historical identities. Overrides are
strict integers in 1..1000000 and require both acknowledgement and an actual
requested continuous trace; run rejects bad configuration before deleting old
outputs or launching the child. Trace observations record the actual canonical
validation limits. Only history changes: active/name/value/read bounds and all
EOF/loss, host token, cleanup/session and full-source identity gates remain.
No prefix, eviction, monkey patch or unconditional default increase is allowed.

`observe_terminal_ack` accepts explicit `trace_analysis_options` for unique
required/wait/counter-name lists and existing observation booleans including
`shader_provenance`, plus an explicit list/tuple `scope_prefixes` validated by
the canonical prefix authority. The lifecycle probe accepts repeatable
`--scope-prefix` / Python `scope_prefixes` only with tracing, validates before
IO/launch and forwards to its original ACK or direct canonical decode. Defaults
keep the environment, required phases and output shape; unmatched requests stay
NO_ACTIVITY in that same retained report, with no extra event scan or hash pass.
The log-quiesce phase, cleanup/session gates and existing
history budget remain mandatory and cannot be overridden through options.
`return_trace_report=True` returns that same canonical report as `trace_report`
for an E2E caller; defaults preserve the old ACK result shape. Options or a
requested report without an actual trace refuse, and boolean flags are strict.
All named session markers validate owner/thread/interval/type even when earlier
or unrelated to the final session. Historical session uniqueness is bounded by
the existing history budget; only first/last/final marker witnesses are exported.
Malformed/duplicate/orphan markers and a marker on refused cleanup reject.
The disabled canonical option retains no session set/witness or output field.
No external prevalidated report, second parser, event scan or trusted sealed
input bypass is accepted. Reader controls count actual decoder calls and all
real file bytes, including malformed tails and the ordinary extra-scan mutant.

`unobserved` log close remains unknown, `no_file` claims no file, and `api_ok`
observes only the close API result. None proves disk durability. A host-returned
tail plus observed child exit does not certify OS resource reclamation, GPU
completion, all descendants or uninstrumented owners. Python controls cover
protocol admission and mocked-launch wiring, including ordinary omitted-token,
omitted-trace-binding and omitted-consumer gates; Native emission is separate.

lifecycle_trace_report.py validates optional writer_diagnostics version 1 in
its existing metadata authority and bounded JSON stream parser. Old captures
without this block remain valid under the original completeness contract.
Present blocks must describe payload-write only, use unsigned 64-bit integer
fields and fixed 6-size/7-wall bucket lengths, agree with append counts and
accepted payload bounds, and bind the slowest monotonic interval to the maximum.
Threaded flush policy is 16ms from first pending encode start; cooperative is
immediate. No report infers IO/GPU cause or a hard write-completion guarantee.
Header/footer/close are outside these payload metrics. Native timing ports and
actual application captures remain distinct evidence.

The optional `clock_domain` field accepts only `process-perf-ns`, identifying
the same checked performance-counter axis used by current Trace events (whose
Chrome `ts` is microseconds). The derived report field `writer_clock_domain`
is explicitly `unknown` for older untagged records or absent diagnostics;
validation never adds a tag to the source metadata. Present unsupported or
mistyped tags are refused. This marker identifies an axis, not IO/GPU causality.

## Explicit lifecycle resource recording

`editor_lifecycle_probe.py --resource-trace` is default-off and requires the
existing `--trace`. The same pure guard rejects an invalid combination before
CLI launch, path/profile creation or normalized-environment construction. That
normalized environment discards inherited JCE flags; explicit resource capture
adds only `JCE_RESOURCE_TRACE=1`, keeping the engine's default1000ms cadence.
No independent interval parser, capture loop or performance-comparison policy
is created. Existing240-frame KPI continuity, Play/Stop ordering, authored-field
restoration, clean exit, image and error admission remain unchanged. The report
records requested resource observation, never observed consumption by request
alone; complete trace/footer and resource scope acceptance remain separate.
CPU tests verify off/explicit on, exact environment delta and invalid prelaunch
refusal. Synthetic QA Play/Stop and any normalized input replay are not physical
Editor gestures, GPU timing/completion or external-analyzer attach proof.


The lifecycle report's explicit --resource-observations (API
resource_observations=True) consumes schema-1 engine.app_resources counters
through the same canonical event parser and successful service-pair authority.
Exact uint32 halves reconstruct uint64 and signed raw GPU clocks; wide report
integers beyond JS's exact range are decimal strings. The resource consumer
retains at most eight active samples, fixed schema fields, first/last witnesses
and sampled min/max/known/unknown counts. Overflow, duplicate/missing fields,
wrong owner/thread and invalid availability reject. Unknown RSS/commit or
Async/GPU timing never becomes zero measured cost. Tagged totals cover only
instrumented allocations. Repeated previous available GPU tuples are counted
without dropping observations or inferring frame rates. Default old captures
stay unchanged; the opt-in requires actual resource samples and retains all
required-service, capture loss, full EOF and file-identity gates. Sampling is
not whole-lifetime coverage, OS thread rates, CPU/GPU joining, selected-adapter
proof, a fence or GPU completion. memory_residency_probe.py --sample-threads
exposes process_sampler's bounded optional TID counters through the same
validation owner; raw samples remain beside the process identity receipt.

Resource samples preserve raw TID zero. The resource consumer does not impose a
second positive-only identity rule: canonical finish first validates continuous
cooperative capture, no worker, no positive TIDs and exactly one nonempty zero
thread name before publishing resource observations. Schema owner halves must
still equal the raw event TID. Named-zero positive and missing/ambiguous-name,
threaded/worker/mixed/resident/owner-mismatch controls consume the full canonical
parser; a complete old positive-only consumer fails the unchanged positive test
through an ordinary unittest assertion, not a crash or parser bypass.

The Play presentation source probe consumes its actual camera admission and
create-failure/Stop release wires through typed counted ports. Runtime creation
requires an admitted camera; stats close and rollback require prior release.
Capture refusal never begins a camera, allocation refusal preserves failed
rollback state without constructing Runtime, and repeated Stop cannot double
release. The existing presentation controls remain unchanged; omitted actual
camera admission/cleanup controls must compile and fail with the declared
ordinary assertion exit, not a linker error. This source probe's camera ports
are not real clone/allocation or native UI/GPU evidence: the camera owner and
installed SDK/native Editor acceptance remain separate.

The presentation probe consumes the same config-read transaction returning
initial cadence rates. Its omitted-apply and unresolved-settings controls retain
a valid rates value and must still fail ordinary runtime assertions; an obsolete
void-return marker or uncompilable mutation is not a control witness.

Optional JCE_FRAME_LOG_CONTEXT=1 frame captures use the exact fourth column
context_json. Only frame_wall carries the fixed v1 JSON packet; other phases
retain an empty fourth field. benchmark_suite.read_frames validates every row,
including warmup and unselected tails, and preserves selected packets separately
from all timing samples. Missing query results remain null; known zero window
flags are valid. Cached admitted dimensions and native begin/end observations
retain distinct names. Backend frame indices do not certify GPU completion.
Legacy captures stay three columns. perf_bench admission requires the requested
schema and subtracts the actual validated header bytes from writer payload
accounting. Metadata cannot excuse timing spikes or silently exclude a frame.

Explicit frame-context selector 3 uses the selector-2 window/viewport facts plus
an exact `clock` object: domain `jce-performance-counter`, with canonical uint64
decimal strings `frequency_hz`, `begin` and `end`. The shared reader preserves
these strings as `window_viewports_clock_v3`, validates all raw row durations,
constant domain/frequency and nonoverlapping intervals, including warmup/tails.
Counter gaps remain explicit; this Engine axis is not Unix/GPU/Present time,
external calibration, continuous coverage or a join to latched backend stats.
Selectors 0/1/2, timing summaries and the existing raw zero-wall policy remain.

Optional `frame_backend_latched_cpu_submit`, `frame_backend_latched_wait_submit`
and `frame_backend_latched_wait_render` rows stay in `backend_latched_ms`, with
separate summary medians and explicit asynchronous, nonadditive semantics.
They never enter `phase_ms` or change existing frame metrics. Missing fields
remain unavailable (NaN in internal arrays, null in summaries); finite zero is
retained with timer availability ambiguity. CSV ordinals locate observations
only and do not join these backend timers to context backend-frame IDs.
Warmup and unselected tails still receive value, schema and duplicate checks.

The Scene Canvas unwind source fixture registers all fatal init exits by unique
actual production blocks. Its original three partial-acquisition paths and
actual/missing-unwind/missing-Canvas/missing-sentinel controls retain their
ordinary ownership assertions. The added lifecycle registration-refusal block
must dispatch the complete existing shutdown signature exactly once and log
the refusal. Its typed counted shutdown boundary does not reproduce rollback;
the native Editor TSR fixture owns complete shutdown/registry evidence. An
ordinary missing-shutdown control must compile and return its declared dispatch
assertion failure. Each covered block has one fatal return, and their total
must match all fatal init returns; an unregistered exit remains a source-gate
failure. Compilation failures, crashes and timeouts are not negative witnesses.

The cache recovery source fixture consumes the complete production model
diagnostics translation unit, including its aggregation macros and hash,
identity, synchronization and flush helpers. Only its four dependency includes
are supplied by actual private declarations and typed CPU trace-generation/
counter/scope observation ports. The model fixture extracts the actual growable
pointer-table state and complete prepare-new-key helper, fills the production
entry ceiling and checks that the first entry survives rehash at its address.
Keep all original model/cache admission, capacity, ownership, async completion
and repair assertions. The capacity negative removes only the diagnostic on
the actual prepare-new-key refusal path; reset still removes its actual reset
wire. Both controls must compile and return ordinary code 4 with the original
cache-recovery failure marker. These ports are not real-ring,
physical-resource, GPU or whole-frame evidence; the Native diagnostic owner
fixture independently covers actual ring records and bounded occupancy reads.


## Optional filesystem read detail observation

`lifecycle_trace_report.py --filesystem-read-details` (Python
`filesystem_read_details=True`) feeds `filesystem_read_observation.py` through
its existing complete canonical event/B/E owner. The runtime already emits the
15 `fs.read.*` counters for read_all/read_capped; no new producer, parser,
runtime API or second trace analysis is added. Defaults omit this result key.

Each active read packet has exactly15 unique typed fields; counter field order
is not required. Counter id is the read service identity, but counter parent
is its submitted caller, unlike the service self-owned B/E parent. Raw thread
zero follows canonical cooperative metadata admission. Wrong owner/thread,
partial/duplicate/out-of-scope fields and invalid types/ranges reject opt-in.
Whole missing packets stay explicitly unknown; no activity is NO_ACTIVITY.

Active packet state uses the existing max-active budget and retires at canonical
End. Diagnostics retain at most min(history,256) first N-1/last witnesses, with
all eligible/retained/omitted counts; finite aggregates never key by path hash.
All later packets still validate. Available zero bytes differ from unknown null.
Request hash halves preserve every bit, including zero/empty-request cases;
they are not canonical paths or path validity. Raw paths and error strings are
never emitted. Optional/required intent and errno remain unavailable: OPEN is
not ENOENT, and isolated VFS refusal is not proof of missing content.

Preserve original CPU task outcomes. A successful buffer return can carry
failed close or capped short-read observations; retain these anomalies without
inventing full-read/durability success. No-detail older captures do not become
successful IO. Required lifecycle phases and every existing EOF/loss/generation/
clock/fingerprint gate remain authoritative. Focused source controls are
`tools/tests/test_filesystem_read_observation.py` in the public source aggregate.
They use the actual canonical memory/file/CLI owner and ordinary malformed-input
controls; synthetic trace fixtures do not certify an application's filesystem.

The CTest domain-registration fixture retains its actual inventory command,
return code and complete stdout/stderr in one structured ordinary assertion on
nonzero exit, invalid JSON or missing/wrong tests-array shape. It does not retry,
skip or replace invalid inventory with an empty list. Independent typed
CompletedProcess ports cover rc0 empty/malformed/wrong-shape responses and valid
lists, including empty. Later successful executions do not explain an earlier
unrecorded malformed CTest response or certify a fix for its unknown cause.

The existing cleanup-ownership helper recognizes reason 6 (`audio_master`);
unknown 7 remains invalid. Canonical begin/end dispatch also forwards the
actual `engine.audio_master_retire` service to that same bounded helper.
Its ordered exact-integer 0/1 had-device/after-device/retired triple is scoped
by the canonical active owner and checked against the actual service outcome.
Partial, duplicate and contradictory triples refuse acceptance. A completely
absent older triple remains explicitly UNAVAILABLE/null, retaining generic CPU
lifecycle admission without inventing native uninit. No-device success is a
separate known observation, never a device-retirement certificate. Existing
continuous/EOF/loss/generation/clock gates remain authoritative; only scalar
aggregates and first/last witnesses are retained, with no second trace parser.

The lifecycle probe exposes explicit `--shader-provenance` and
`--filesystem-open-failures` opt-ins. Both require `--trace` and exact Python
bool values before path resolution or output creation. They enable the existing
renderer resource diagnostic latch or FS open-failure detail after the normal
JCE environment filter, without inheriting arbitrary diagnostic configuration.
ACK analysis merges requested options into its existing canonical read; without
ACK, only these explicit new observers introduce one canonical analysis. Reuse
validated trace hashes/fingerprints for inventory rather than decoding again.
Defaults, scene restoration and the actual host loop retain their owners.

FS open-failure diagnostics remain normal bounded WARN artifacts. The existing
Engine-session owner reserves at most64path/error groups; each records task,
parent, generation, exact-request hash, operation, total byte length and at most
128UTF8bytes as hex with truncation metadata. Error text is an immediate SDL
error copy, not a structured errno/domain. Optionality remains unknown and the
summary's delivery remains unavailable. Typed FS-read observations do not decode
these text artifacts or prove all failures were delivered. A requested observer
without records remains unavailable, never an all-module/GPU lifetime certificate.

`process_sampler.py` uses one `perf_counter` origin for process deadlines,
markers, thread queries and its injected Windows GPU provider. Poll sleeps
are capped by the remaining deadline. Termination waits at most five
seconds for the owned child; unobserved exit fails the capture. This
bounds the exit wait when the OS is scheduling the observer. It records
`get_clock_info` once and preserves the origin and aggregate OS-query bracket
in decimal-string nanoseconds. These are not Unix timestamps or atomic frame
samples. Standalone GPU-provider defaults remain unchanged. Resolution metadata
is not a cross-process clock certificate: verify the actual producer, packages,
provider epoch and query bounds before joining data. JCE resource.sample_ns uses
SDL tick-relative time, which differs from raw-QPC trace time. Missing legacy
anchors remain unavailable; file-visibility markers are not rendered-frame or
startup timestamps. Native/backend performance and query overhead require
separate measurements. Regression coverage stays in tests/test_process_sampler.py.

## Required-service interval witnesses

The opt-in `--scope-prefix PREFIX` / `scope_prefixes=(...)` lifecycle observer
uses the same canonical close-scope owner and timing accumulator. Up to128
unique printable prefixes of1..128 characters are validated before input or
output; surrounding whitespace is refused. Matching complete CPU scopes retain
count, inclusive timing sum/min/max and first/last actual B/E witnesses per
(name, thread), including failed/cancelled task outcomes. Selected names and
aggregate rows use the existing name/history limits (4096/200000 for memory).
Known rows remain updatable at capacity; overflow refuses instead of truncating.
Overlapping prefixes never duplicate rows. Each request reports NO_ACTIVITY
when unmatched; observed activity does not certify backend success, GPU duration
or OS CPU execution. Never sum these durations across threads as wall time.
Defaults omit this observation and retain the existing validation/report shape.

lifecycle_trace_report.py --scope-witnesses retains first/last matched CPU
intervals for explicitly required services in its existing canonical reader.
Default output remains unchanged. At most128 required names are admitted;
active scopes retain their original owner tuple plus one raw begin index.
Every event and matched scope still passes the same complete validation. Samples
retain begin/end trace timestamps, raw indices, thread, service/parent identity,
outcome and observed count. First/last follow exported completion order; they
are not a complete repeated timeline or OS/GPU clock alignment. The ACK and
Editor lifecycle probe forward this option through that same parser and reuse
its fingerprint. No producer flag, second parse or resource registry is added.
Compose shader/FS observers through actual packet fixtures; source tuple-shape
inspection alone is not composition or native application evidence.

The lifecycle probe defaults to its existing hidden window. The optional
`--visible-not-focusable` / `visible_not_focusable=True` request changes only
HIDDEN to0 and NOT_FOCUSABLE to1 after inherited-JCE filtering; exact bool
validation precedes IO. Its receipt records these requests and excludes physical
UI interaction or presentation proof. Keep vsync, cadence, scene and timeout
unchanged for an occlusion comparison; CPU forwarding controls do not prove it.
The optional --vsync on|off / vsync="on"|"off" injects only JCE_LAUNCH_VSYNC=1|0
AFTER inherited-JCE sanitization. Exact option validation precedes outputs; None
keeps the default environment/result shape. An inherited parent JCE_LAUNCH_VSYNC
is stripped, not a child request. Receipts retain vsync_requested and the actual
child environment, never an inferred effective reset/presentation policy. Counted
child controls exercise both values and omitted CLI/API forwarding failures.

The lifecycle probe accepts only explicit normalized backend, GPU-vendor and
quality-tier selectors, including d3d12 with the saved D3D12 renderer choice.
Validate before output or process creation. Inherited
JCE diagnostics remain filtered; only selected allowlisted values are injected.
Its fresh preferences/session mirror backend/vendor requests at startup, while
omitted options retain existing bytes and result shape. Request metadata also
survives failed runs and never substitutes for actual device/backend, effective
quality or rendered extent. Python protocol tests do not exercise GPU selection.

The lifecycle probe forwards an explicitly requested exact disk-history tuple
through the existing terminal observer or direct canonical reader. Pure shared
option validation and trace admission precede path/output work; occupied DBs,
sidecars and missing parents refuse before native launch. History-only requests
actually invoke validation. Requested storage metadata survives failed runs;
default RAM history and absent-option behavior stay unchanged. The probe owns
no second parser/database service. Synthetic canonical-reader/SQLite controls
do not certify a native application or every captured owner.
