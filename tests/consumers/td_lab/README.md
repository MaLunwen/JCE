# Tower Defense reference recreation

SDK-only C99 consumer of JCE. Open this directory as a JCE project;
`resources/scenes/td_lab.scene.json` is the startup scene. The same compiled
`TdController` scene class owns rules and presentation in Editor Play and the
standalone application. The upstream JavaScript runtime is not shipped.

Reference: https://github.com/Casmo/tower-defense at
`784129a685627a18f3d3f8fa594208000d9313e8`. Source bytes remain immutable in
`_workspace/references/phase1_tower_defense`. The compatibility and evidence
sites are independent copies; their Object.observe fallback and capture probe
are disclosed in the evidence tools. They do not replace gameplay algorithms.

## Build and verify

From the repository root:

```powershell
python tests/consumers/td_lab/tools/acquire_reference.py --help
python tests/consumers/td_lab/tools/author_ground.py
python tests/consumers/td_lab/tools/author_ui.py
python tests/consumers/td_lab/tools/author_reference_scene.py
python scripts/jce.py cook tests/consumers/td_lab --rgba8 --mipmaps
python scripts/jce.py app tests/consumers/td_lab --variant dist --sdk dist/sdk/win32-x86_64-dist-single
python tests/consumers/td_lab/tools/verify_game.py --editor
python tests/consumers/td_lab/tools/verify_reference_inputs.py
python tests/consumers/td_lab/tools/verify_visual.py
python tests/consumers/td_lab/tools/verify_delivery.py
python tests/consumers/td_lab/tools/verify_interaction_render.py
python tests/consumers/td_lab/tools/verify_preview.py
python tests/consumers/td_lab/tools/verify_parameters.py
python tests/consumers/td_lab/tools/verify_performance.py
python tests/consumers/td_lab/tools/write_verification.py --help
```

Final delivery contains only `dist/apps/td_lab/windows-x86_64-dist/TowerDefense.exe` or
`dist/apps/td_lab/wasm32-dist/TowerDefense.html`. Build products, developer SDKs
and measurement files remain in their separate directories.
Current acceptance evidence belongs in the ignored `build/verification/perf27` tree;
raw logs retain the exact artifact identity and explicit measurement scope.
The report generator takes explicit named evidence and current artifact identities.
--final rejects missing, stale or contended performance records; it does not fall
back to an earlier report. Generated reports stay in the ignored evidence tree.
Earlier evidence remains at its original examples/td_lab/build paths and does
not certify a newly built executable.
The content Bundle requires the rebuilt native TdController authoring module
when opened in Editor Play. It does not embed portable native machine code.
Independent EXE/HTML delivery and relocation are separate acceptance checks.
Set TD_VERIFY_GAME, TD_VERIFY_EDITOR, TD_VERIFY_OUT and TD_VERIFY_SDK explicitly
when testing candidate artifacts before publication.
`build/verification/` stores actual D3D11 logs, input recordings and screenshots.
`build/reference/captures/` stores browser screenshots and state metadata.
The reusable comparator is `tools/compare_captures.py`; it requires equal
capture dimensions and explicit regions. It never aligns or recolors images.

## Original controls

Play opens the level menu; Play level 1 starts the board. Click an open tile,
then click a tower card or release 1/2/3. Release E or U to spawn Ghost/UFO,
W to start a wave, C/Escape to deselect. Left drag rotates, right drag pans,
middle drag and wheel dolly. Arrow keys pan. Touch counts 1/2/3 select
rotate/pinch/pan. Enter and R exist only in the explicitly enabled test
extension `TD_EXTENDED=1`; they are not original game controls.

Resources, score and lives persist when returning to menus. Remaining lives stop
at zero while all escaped actors are removed and counted, and numeric HUD sprites
clamp negative inputs to zero.
Tower balance,
A* tie-breaking, Catmull-Rom movement, bullet pursuit, death rewards, blood
and the two concurrent wave timers are in `src/rules.c`. Normal gameplay accumulates the runtime-supplied simulation dt on one fixed 60 Hz clock for motion, cooldowns and waves, independently of rendering. The runtime owns pause and time scale; the consumer never replaces its interval with a second wall clock. Each update accepts at most 0.25 seconds (15 steps) of supplied simulation time, records excess simulation time and never queues an unbounded backlog. Host suspension intervals already rejected by the shared host policy stay excluded. Reference frame captures use
explicit fixed 1/60 s fixtures and seed 7. Core rules have repeatable native checks.

## Maintained file map

| Path | Responsibility |
| --- | --- |
| `src/main.c` | Stock JCE platform host registration |
| `src/controller.c` | Shared scene class and original input mapping |
| `src/rules.{c,h}` | Tower Defense rules and deterministic state |
| `src/view.{c,h}` | Authored camera, models, billboards, blood and UI |
| `src/ui_metrics.h` | Generated proportional text canvas sizes |
| `src/test_rules.c` | Balance, route blocking, waves and deterministic checks |
| `src/test_controller_clock.c`, `tools/verify_controller_clock.py` | Actual SDK runtime/native C controller pause, scaled time, host resume and frame-rate clock checks; generated old-wall negative control |
| `tools/acquire_reference.py` | Immutable pin, asset manifest, formal model conversion |
| `tools/author_ground.py`, `ground-reference.json` | Measured reference box normals and formal OBJ import |
| `tools/author_ui.py` | Default-font raster labels and original CSS artwork |
| `tools/author_reference_scene.py` | 104 static/UI entities and eight lazily spawned/reused actor prefabs |
| `tools/reference_fixture_server.py`, `reference_probe.js` | Isolated browser evidence fixtures |
| `tools/verify_game.py` | Real application input recording, capture and isolated Editor profiles |
| `tools/verify_reference_inputs.py`, `verify_visual.py`, `verify_delivery.py`, `write_verification.py` | Independent original input comparison, host captures, byte audits and Bundle reopen with native authoring context |
| `tools/verify_web_acceptance.js` | Explicit-config Playwright input/registry and lifecycle/actual WebGL ownership observations |
| `tools/verify_web_performance.js` | Saved six alternating source/JCE initial-board RAF and callback CPU observations |
| `tools/verify_web_hash.js` | Shared secure browser SHA-256 utility with no GL or server request |
| `tools/web_evidence.py` | Single strict render snapshot parser and quality CLI |
| `tools/pack_scene_bundle.c` | Thin SDK scene bundle producer; explicit dependency roots |
| `Settings/RenderPipeline.rp.json`, `.jce/project-settings.json` | Explicit original quality settings |
| `resources/licenses/Casmo-MIT.txt` | Upstream game asset notice |
| `ASSET_PROVENANCE.json`, `tools/write_asset_provenance.py` | Selected asset identities, transforms, typography and exclusions |

## Assets and limitations

The project explicitly requests MID material look semantics and the measured
original hemisphere ground color [247/255, 1, 243/255]. Renderer backend cost
clamps retain their independent bloom and screen-space budgets; a requested
look must not disappear when bloom is reduced. Actual native constant-buffer
and Web uniform observations are quality evidence; scene JSON alone is intent.

Original PNG/JPG art is retained and five original OBJ models go through
`jce_cook --geometry-only`. The ground is an authored model reconstructed from
the original BoxGeometry after its unweighted normal recomputation. All source
textures use RGBA8 with mipmaps for fidelity. Import sidecars explicitly use
linear numeric mip averaging to match the original gammaInput=false renderer. Blood and cursor images are
original assets. Dormant smoke/cannon art remains imported; disabled reference
behavior is not invented. Browser libraries are reference-only dependencies.

UI labels use Pillow's bundled default Aileron font (No Rights Reserved), or an
explicit authoring font supplied with --font. The original display font and
earlier local glyph captures remain reference-only. This is an intentional
typography difference, separately measured from scene/asset rendering parity.
[ASSET_PROVENANCE.json](ASSET_PROVENANCE.json) records selected file hashes,
conversion inputs, generated UI, font identity, notices and excluded reference
software. `python tools/write_asset_provenance.py --check` checks those records.
No font software or browser dependency tree is shipped by this consumer.

A successful build or rules test is not a visual-parity pass. The final
verification report records measured screenshot differences and unverified
input/host cases separately. No pixel-identity claim follows from file creation.

Run formal `python scripts/jce.py cook tests/consumers/td_lab --rgba8 --mipmaps` after authoring resources and before `build-project`; the build consumes the cooked tree. Dynamic prefab dependency descriptors are cooked and packed with the scripts. `tools/verify_interaction_render.py` checks actual camera images and the Editor event path; `verify_parameters.py` audits the pinned source; `verify_performance.py --protocol standard` (the default) retains three capture-free runs per workload and host: 2 hosts x 3 modes x 3 repeats = 18 runs, with 120 warmup and 300 measured frames (450 maximum). `--protocol publication` uses `td-native-publication-v2`: 7 repeats = 42 runs, discards the first 120 startup frames, then accumulates at least 3000 ms of actual continuous frame intervals before the next 600 measured frames. Warmup has a 3000-frame budget; the bounded capture is at most 3750 frames and times out after 300 seconds. Missing warmup time or insufficient subsequent frames is rejected. The `[1280,720]` native protocol label does not apply or observe window/FBO dimensions: `actual_window_size` and `actual_game_fbo_size` remain unavailable, and Editor uses its live Game panel resolution policy. Native host measurements are separate workloads, without a same-render-surface comparison. Both preserve complete raw CSV and spikes, use realtime simulation, and request hidden D3D11 with synchronous asset streaming. This is a frame-pacing observation with requested VSync enabled; it does not measure uncapped throughput or prove all shader compilation completed. The standalone actual linked-shader and submitted-uniform witnesses are separate from timing.

Set `TD_VERIFY_GAME`, `TD_VERIFY_EDITOR`, and a fresh absolute `TD_VERIFY_OUT` before invoking the native performance tool. Completed historical aggregates are never overwritten. `--resume <partial.json>` requires the same explicit protocol, current executable hashes and every original raw file identity; it creates a fresh run directory. Reports record the stable protocol id, warmup/measured frames and repeats. Publication reports also bind the earliest valid warmup boundary and actual elapsed warmup milliseconds; startup delays cannot supply that time. Each repetition binds original CSV, execution logs, OS samples, and before/after UTC process inventories. Final admission recognizes only the two complete protocols and independently reparses frame continuity, the exact measurement window, summaries and FPS. Reusing one capture as multiple repetitions, overlapping runs, stale raw files or missing GPU timings presented as numbers is rejected. The named guard also blocks RenderDoc and known native/Web QA producers; ordinary and inaccessible background process details remain retained, and the executor separately owns browser/GPU scheduling. This guard does not claim absolute full-machine GPU idleness. The auxiliary live-clock control remains separately identifiable. Previous method strings and observations remain historical evidence.

Normal game speed consumes host simulation time, independently of presentation rate, calibrated to the original at 60 Hz. Legacy frame-dependent movement survives only behind explicit `TD_LEGACY_FRAME_MODE=1` / Web `?legacy-frame` diagnostics; those source-frame diagnostics deliberately retain historical movement policy. `TD_FIXTURE` ghost/ufo/combat advances at most 120 fixed capture steps and holds while runtime simulation dt is zero; other fixture layouts stay frozen. `JCE_FRAME_DT_FIXED` belongs to the host QA producer and does not bypass runtime pause or time scale in normal gameplay. `TD_BOOT` reports the effective consumer clock, fixture/legacy policy and whether fixed host dt was requested. `TD_TIMING=1` enables wall/simulation observations without starting gameplay; `TD_TIMING_START=1` explicitly starts the single-enemy timing scenario. The real runtime/native C clock target additionally checks pause, zero/half/double time scale, actual shared host-policy resume, 30/60/120/144 Hz complete rule-state equivalence and bounded hitch recovery. After building `td_controller_clock_test` and `td_controller_clock_old_wall_test` against the installed SDK, run `tools/verify_controller_clock.py --normal <normal-executable> --old-wall <control-executable> --out <fresh-directory>`. It requires exit 0 from the normal binary and exit 1 with named assertion failures from the generated old wall-resampling control; a crash, timeout or setup failure cannot count as a passing control. Headless presentation ports make this clock evidence separate from visual or physical editor-input acceptance. Historical clock evidence is retained separately from the current design. `verify_preview.py` retains negative and repaired consecutive first-placement frames.

Web build and single-artifact QA: see `web/README.md`. The native single EXE and Web single HTML are independently verified; neither the former DLL bundle nor an untested delivery profile is evidence of single-file execution.

The source and dedicated acceptance tools are SDK consumers. examples/td_lab
contains the standalone Bundle catalog and upstream license/provenance. Earlier
raw evidence remains under its original ignored examples/td_lab/build location;
its identity is never rewritten as a new migrated-project run.


## Replaying Web acceptance

`tools/verify_web_acceptance.js` is a saved `async (page, config, hashValue)`
function for Playwright's code tool. Read that source and the saved
`tools/verify_web_hash.js` function source, then invoke the tool with a wrapper:

```javascript
const code = 'async (page) => await (' + sourceText + ')(page, ' +
    JSON.stringify(config) + ', (' + hashSourceText + '))';
// Pass code to the Playwright browser_run_code_unsafe tool.
```

Supply `mode` (`input` or `lifecycle`), `artifact_url` (the HTML HTTP URL without
query parameters), current `artifact_sha256`, exact `request_json` text from
`build/native-registry-proof/web-request.json`, its `request_sha256`, and the
same request `nonce`. Also supply an absolute existing `output_dir` and a unique
ASCII `run_id`. There are no baked-in artifact hashes or nonce values. The
script verifies the request text and served HTML bytes before gameplay, then
validates the actual MEMFS receipt against that request. Each call owns one
Chrome headless application page, requests ANGLE D3D11, and closes its browser
in `finally`. Actual adapter identity is recorded independently of those flags.
The hash function uses an owned non-rendering context of the default MCP
browser and a dedicated loopback `/__jce_qa_hash_*` URL fulfilled with empty
QA HTML. It makes no test-server request or WebGL context; it transfers bytes
as base64 to secure WebCrypto and always closes its context. No `require`,
dynamic import, Node filesystem or process API is assumed in the MCP runtime.
The executor must schedule exclusive GPU access and archive the downloaded
results; do not use these probes as formal performance measurements.

Input mode uses a 1280x720 DPR=1 viewport, actual zero-delay and 100 ms clicks,
cell 60, a basic tower card hit costing one resource, camera drag, E hold/release,
and resize to 896x504. It saves registry/input JSON and actual before/after/resize
screenshots. A picture hash difference is an observation, not a pixel-parity
certificate; inspect and compare those pictures independently.

Lifecycle mode runs realtime autodefense for `frames` (default 1000, minimum
1000) with cooperative tracing. The optional `viewport` defaults to 640x360;
`timeout_ms` defaults to 60000, and `browser_channel` defaults to `chrome`.
It saves the producer's unchanged raw trace and actual WebGL create/delete
counts plus remaining object validity before browser close. `HEAPU8.byteLength`
is reported only as linear-memory capacity. Apply the existing
`tools/lifecycle_trace_report.py` with explicit required CPU phases afterward.
CPU scopes, GL deletion and this finite run do not prove GPU fence completion,
driver memory reclamation or long-term leak freedom. Failed observations are
saved as `FAILED`; a compiled script or missing browser run remains unmeasured.

Final Web admission requires current schema/status, actual served-byte identity,
exact registry request/receipt, trusted captured input events and unchanged
screenshot files. GL admission recomputes retirement from every observed
resource kind and validity inspection, then checks the unchanged raw trace,
1000+ consecutive game frame IDs and all 19 required CPU service phases using
the existing independent parser. When archiving that parser's Web summary,
add `cleanup_evidence_path` (absolute actual cleanup JSON path) and
`cleanup_evidence_sha256`; the summary must bind the same raw trace and current
HTML through that cleanup observation. Missing old fields remain diagnostic.
This does not alter Native/Editor evidence admission.


Set `restrict_network: true` for a relocated sole HTML in an otherwise empty
Unicode directory. Retain that filesystem inventory separately; the browser
cannot establish directory contents. The opt-in route permits only the selected
HTML origin/path, including its QA query, and records actual HTTP request methods,
URLs and response statuses. Every other HTTP(S) request is recorded and aborted;
an unexpected favicon is a diagnostic failure, not an implicit exception.
Inline data/blob mechanisms retain normal browser handling. This run verifies
HTTP transport and dependency isolation. Tool security makes `file://` testing
unavailable, so it never certifies offline local-file execution.


## Replaying Web formal performance

`tools/verify_web_performance.js` uses the same saved-function wrapper with
`async (page, config, hashValue)` and the shared hash function above. Supply
unqueried HTTP `source_url` and `jce_url`, current
`jce_artifact_sha256`, exact `source_html_sha256`, `source_application_sha256`,
`source_three_sha256`, `source_probe_sha256`, and `reference_commit` equal to
`784129a685627a18f3d3f8fa594208000d9313e8`. The source URL points to the separate
reference evidence server; hash its actual compatibility/evidence HTML and
first-party probe, and hash the pinned original application/Three.js bytes.
Also supply `run_id`, absolute existing `output_dir`, and external scheduling
`conditions` (including actual `contended: false` evidence before a final claim).
Optional `browser_channel` and `timeout_ms` have the acceptance tool's meanings.

Set `protocol: "publication"` (the default) for `td-web-initial-publication-v2`. One Chrome browser runs exactly fourteen alternating source/JCE windows, seven per host, at 1280x720
DPR=1. Each preceding page/context closes before the next renderer opens.
The source fixture is setup only: a prefix on the owned probe preserves normal
render/Date.now/Math.random, READY restores those originals, and the real game
counter must advance once per observed callback. Original game bytes are not
patched. Actual context version, adapter, attributes, capabilities and final
canvas/drawing-buffer size are read before warmup. Setup draw observers restore
before at least 3 seconds of actual warmup. A sample needs both at least 5 seconds and 600 complete consecutive RAF intervals (at least 601 callbacks), and is bounded to 30 seconds. Missing, late or short samples retain partial raw observations and are unavailable. Explicit tagged `standard` uses the historical six-window / 2.5-second diagnostic policy and cannot enter final admission. Timing records only the real
game callback's RAF timestamps and callback CPU wall duration; preserve every
interval and spike. It never substitutes an observer loop for product rendering.

Pass `jce_quality_evidence_json` (exact prior successful input JSON), its
`jce_quality_evidence_sha256`, and absolute `jce_quality_evidence_path`. Before
the browser call, run `tools/web_evidence.py --stdin-quality` with the exact
input bytes on stdin and save stdout bytes unchanged. Pass that saved JSON
as `jce_quality_snapshot_json`, its `jce_quality_snapshot_sha256`, and absolute
`jce_quality_snapshot_path`. The CLI output contains `available`,
`artifact_sha256`, `evidence_sha256`, `effective`, and `protocol`; chunked output
also records `parts` and `bytes`. The single parser accepts a complete legacy
`TD_RENDER` JSON object or the exact ordered 384-byte `TD_RENDER_CHUNK` set,
including each `TD_RENDER_END`. Truncation, missing/repeated/reordered chunks,
wrong byte lengths, nonstandard JSON and ambiguous repeated snapshots fail.
The browser only hash-binds the supplied snapshot, outside timing; the final
validator rereads both files and invokes the same parser to verify the result.
This retains actual same-HTML render settings without enabling trace during
formal timing. Missing, stale or unavailable quality stays unavailable; raw
windows can be retained but `final_claims_allowed` remains false. Other failures
remain `UNAVAILABLE` with their original errors, without invented FPS. The
saved JSON keeps all fourteen host rows plus raw intervals/callback durations and
has the existing report generator's `jce_artifact_sha256`, `runs`, `fps`,
`interval_ms` and `cpu_ms` fields for successful windows. A final report requires
`complete` and `final_claims_allowed`, independent scheduling evidence and
separate visual/lifecycle acceptance. Scheduling `conditions.evidence_path`
and `evidence_sha256` must bind an actual JSON with `contended: false` and
`before`/`after` observations. Each records a UTC ISO `observed_utc`, nonempty
original `processes` inventory, `competing_build_test_processes`, and
`competing_gpu_processes`. Both competing lists must be empty, and observation
times must bracket `generated_utc` through `completed_utc`. Busy or missing
observations cannot be promoted by a caller flag. The admission validator
recalculates every raw interval, summary and FPS, checks real game updates,
GPU/canvas attributes, source byte identity, trace-off environment and all
fourteen alternating windows.

Formal admission also requires `jce_render_quality_evidence_json` (the exact
raw successful `verify_web_render_quality.js` report), its
`jce_render_quality_evidence_sha256`, and absolute
`jce_render_quality_evidence_path`. This is a separate instrumented observation
before timing: it binds the served and executed HTML hashes, actual linked
fragment source files and submitted ground uniforms. The performance report
stores these as `jce_render_quality`; final validation rereads the original
JSON and fragment bytes through the single `web_render_quality_evidence.py`
authority, requires exact reported JSON equality, and matches its actual GPU
vendor/renderer/context to every measured JCE run. Desired scene values or a
successful bootstrap JSON snapshot cannot replace this witness. Missing,
failed, stale or altered GPU quality keeps `final_claims_allowed` false; a
faster run with a disabled requested feature is rejected. These selected draws
and linked-source finite-power guards do not certify every fragment or the
whole render pipeline, and never serve as GPU timing samples.

Requested-authoring and immutable-source controls belong to this consumer: run
`python -B tests/consumers/td_lab/tools/test_requested_render_parameters.py`.
A clean clone without the ignored pinned reference records an explicit skip;
it does not claim source comparison success.

Run CPU-only contract controls with `python -B -m unittest discover -s
tests/consumers/td_lab/tools -p test_web*.py -v`. The fixtures are
explicitly synthetic admission controls, not browser observations or measured
product performance. Node syntax/mock checks are also not MCP execution:
the first actual invocation exposed unavailable Node module APIs, and the
executor separately preflighted the supported secure WebCrypto mechanism.
Browser RAF/CPU observations do not prove
GPU execution, uncapped throughput or universal engine parity.


## Diagnosing the zero-shininess shader boundary

`tools/verify_reference_shader_boundary.js` is a separate explicit-config
Playwright function, `async (page, config)`. It creates a Chrome browser at
1280x720 DPR=1 and closes its owned pages/browser in `finally`. Supply
unqueried `source_url`, `reference_commit` equal to
`784129a685627a18f3d3f8fa594208000d9313e8`, exact
`source_html_sha256`, `source_application_sha256`, `source_three_sha256`,
`source_probe_sha256`, `run_id`, and an absolute existing `output_dir`.
Use the actual compatibility/evidence HTML and owned probe hashes; application
and Three.js hashes identify unchanged pinned original bytes. Optional
`browser_channel` defaults to `chrome`; `timeout_ms` defaults to 60000.

Default `controls` is `["initial", "touch-rotate"]`; additional independent
controls are `wheel`, `right-pan`, `middle-dolly`, `touch-pinch`, and `touch-pan`.
Each control opens a fresh initial fixture, uses the existing original
OrbitControls input producer, and intercepts `/evidence` POSTs without writing
the existing raw-reference goldens. Default `capture_repeats` is 4 (range 1..8):
four untouched browser PNGs precede and four follow the shader policy for each
control. Probe panels are hidden in both sets. PNGs are never recolored, cropped
or replaced by a generated expected image.

The policy is explicitly `DIAGNOSTIC_DEFINED_POWER_REFERENCE`, with
`raw_source_parity_passed: false`. It changes exactly one directional and two
hemisphere lobe powers in memory: exponent zero returns one, a nonpositive base
with positive exponent returns zero, and the remaining positive-base power is
unchanged. The defined input domain is finite nonnegative shininess. Fixed-5
Fresnel and all other shader bytes remain unchanged. The tool records both
ShaderChunk strings and their preassembled ShaderLib fragments plus hashes.
The helper is declared globally, outside the chunk's `main` body. No original
file, network byte, geometry, camera or material value is rewritten.

Because r68 caches builtin shader programs, unique scene Phong materials are
disposed and marked `needsUpdate` solely to rebuild diagnostic GPU programs.
Success requires a newly linked and actually drawn ground program whose
recorded real GL fragment contains the helper and all three replacements, with
the actual ground shininess uniform still zero and no GL/browser errors.
Exact scene, geometry, camera and material-value snapshots must remain equal.
Missing replacements, stale GPU programs, unavailable uniforms and compile
errors remain `FAILED`. SHA-256 uses platform WebCrypto on an owned secure
utility page fulfilled from the selected source origin; that page creates no
GL context and makes no application/server request.

Per-control JSON and an aggregate `reference-boundary-<run_id>.json` retain
raw/defined-policy picture paths, hashes, source identities, shader compilation,
ground uniforms, actual GL context/adapter, camera and failure observations.
`DEFINED_POWER_REFERENCE_CAPTURED` means this diagnostic reference was captured;
it does not turn a failing raw-source visual comparison into a pass. Compare
the separate finite-policy pictures independently with existing unchanged
thresholds and preserve the raw-source differences alongside them. Actual PNGs
and shader-source checks do not reveal fragment floating-point NaNs, establish
GPU completion, measure performance or certify all platforms.


## Same-window update and draw performance evidence

The default `publication` protocol remains v2 and reports callback clocks.
Explicit `protocol: "v3-work-consumed"` uses the same quality, artifact and
scheduling inputs above, with seven source/JCE off/off/on/on repeats (28 windows).
Each retains at least 3 seconds of warmup, 5 seconds and 600 consecutive sampled
RAF intervals, with the existing 30-second bound. Rebuild the consumer first:
the QA export belongs to its compiled controller, not an Engine SDK service.

Only on rows enable `TD_UPDATE_CENSUS=1`. Each sampled callback must advance
exactly one actual TD `on_update` entry and final normal `JCE_C_OK` return,
with the same live instance generation and no overflow/invalidation. The frozen
initial game rules are not an update counter. Source rows instead require the
real `TowerDefense.counter` delta and do not fabricate TD instance fields.
`src/qa_update_census.inc.h` publishes the consumer-owned fixed eight-word
record through public Emscripten `EM_JS`/`EMSCRIPTEN_KEEPALIVE`:
`Module._td_qa_update_census_snapshot` and
`Module.jceQaUpdateCensusRead(new Uint32Array(8))`. The reader refuses foreign
Module ownership, wrong output types/length, writes into the wasm heap, dead or
changed instances, saturation and invalidation, and refreshes `HEAPU32` after
memory growth. It does not inspect private Engine addresses.

Each on-row callback also needs an actual selected-program GL world draw.
Setup records shader source at real program linking before bgfx detaches it;
JCE program hashes must match the separate same-artifact ground-quality witness.
The sampling window counts real program-object identities without GL queries or
readbacks. Missing draws, unknown programs or relinking/deletion refuse evidence.
The corresponding off row restores all GL wrappers and disables census counters.
Disabled probing still adds conditional overhead. Census reads and witness
construction are outside callback CPU spans; GL counters are inside them, and
RAF intervals include bookkeeping. Keep signed raw off/on differences without
subtracting an estimated cost. These clocks and GL calls do not certify bgfx
submission, completed GPU work, pixels or uncapped throughput.

The native-only consumer targets `td_controller_census_normal_test`,
`td_controller_census_missing_update_test`, `td_controller_census_missing_entry_test`,
`td_controller_census_missing_return_test`, `td_controller_census_reset_test` and
`td_controller_census_overflow_test` run eight cases through the whole actual
controller, public runtime and native C VM. Their ordinary assertion controls
remain separate from the original thirteen controller clock cases. After building
these targets against the installed SDK, `tools/verify_update_census.py` accepts
`--normal`, `--missing-update`, `--missing-entry`, `--missing-return`, `--reset`,
`--overflow` executable paths and a fresh `--out` directory; it records actual
exit outcomes, case/failure counts and artifact/log hashes.

`tools/test_web_update_census.py` exercises the saved EM_JS reader and whole
saved browser observer through CPU ports, including memory growth, missing
update/return/draw, generation changes and ordinary old-reader mutations.
`tools/test_web_evidence.py` checks v2/v3 admission, raw signed control deltas
and missing/reset/foreign-program counterexamples. These CPU checks do not prove
Web compilation, exported-symbol reachability or real browser work: those need
the rebuilt HTML and independent browser/quality observations before admission.
