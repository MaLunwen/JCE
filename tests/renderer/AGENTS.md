# Renderer CPU fixtures

Mesh creation lifecycle coverage now expects one immutable static index handle
per indexed mesh. The triangle prefix and optional wire suffix have distinct
counts; optional wire staging refusal preserves solid geometry, while required
buffer failure retires the constructor's own handles. The existing model colour
submit fixture checks exact base/reduced-LOD ranges, including coarsest-band
reuse, so a whole-buffer bind cannot expose the packed wire suffix as triangles.
These controlled CPU assertions do not establish native GPU capacity or pixels.

The CPU skin fixture tests the production deformer without a graphics device.
It covers blending with the last supported joint, pre-deformed input,
normal/tangent transforms and invalid influences without losing the source
geometry. Vertex buffer submission and GPU pixels require separate evidence.

`test_jce_render_readback_contract` consumes the actual readback TU, central
submission body and backend destroy body. Real-ring controls distinguish one
ready observation from cancellation intent or post-backend CPU release, retain
wide identities, reject stale capture joins and exercise zero/MAX serial wrap.
Complete-owner missing-ready and false-stop-readiness mutants must fail ordinary
assertions. The optional fixture trace export is CPU evidence only.
The complete screenshot callback fixture checks that canonical format refusal
precedes file admission, staging allocation, blit and readback; its missing-gate
mutant must fail ordinary assertions. No all-zero-pixel heuristic is used.

The stock ownership fixture includes the complete actual jce_shaders.c stock
section (both PAK/FS entry bodies and their shared helper) plus complete private
headers. Typed API-info inputs and counted public overlay-load ports validate
exact full/compatibility requests and path forwarding without creating a device.
Empty/skipped slots must remain INVALID; genuine cached handle zero balances all
independent acquires and one borrowed set owner. Missing-profile-filter and
old-zero-initializer complete-header controls must compile and fail ordinary
assertions, in addition to CTest WILL_FAIL. These are CPU ownership/routing
controls, not shader compilation, native GPU execution or full GL2.1 Editor proof.

`test_jce_render_encoder_abort` consumes the complete actual private encoder
header with typed uniform/state/submit/discard ports and no backend device.
Failed deferred resolution must retire prepared uniforms, command range and
draw state with exactly one ALL discard on the captured implicit or worker
recording. Both implicit-only submit routes retain their implicit owner even
when another TLS encoder is installed. Separate pending recordings survive;
the next valid draw may bind the same uniform, and successful deferred programs,
depth, flags and indirect arguments remain unchanged. The complete-header
`missing_abort` control removes only the actual retirement wire and must compile
then fail these ordinary assertions. Direct execution is required in addition to
CTest WILL_FAIL. These are CPU routing/retirement controls, not GPU execution,
pixels or evidence for the cause of a particular application failure.

`test_jce_model_colour_submit` includes the complete model and private encoder
owners. Typed backend ports verify stock programs and the final callback sampler
state for solo, CPU instanced, fixed-buffer and indirect colour producers.
An unrelated TLS encoder must leave these implicit producers on their original
submit path. Cases retain 64-byte matrices, 80-byte matrix/tint data, fixed
slices, indirect arguments, tint folding and foliage material fallback.
The `old_raw_model` control restores raw submits and must fail the deferred
resolution assertions. These CPU controls do not create a graphics device.

`test_jce_shader_link_owned` retains the complete actual shader loader/link TU,
stock PAK and real bgfx Noop ownership. Its provenance cases forward actual
create_shader calls, inspect their borrowed Memory before consumption and
count the producer's SHA/clock calls. DIAG is configured through the actual
native environment and actual Noop renderer session; no test policy getter
substitutes for the renderer owner. On Native Windows the fixture uses its own
CRT _putenv_s and verifies the real getenv value; pinned SDL's Win32 setter
updates the OS table, which does not refresh the Engine CRT environment cache.
Other Native platforms retain the SDL setter. Default-off assertions preserve
existing Pak::Decompress/Core/link events and constrain only new provenance
scope/counter packets plus the producer's SHA/clock calls. This is Noop integration, not physical GPU
or another backend/device-policy certification.

Before every actual Noop create or recreate, this synchronous shader-owner
fixture calls the pinned bgfx pre-init render_frame on the same API thread and
requires NO_CONTEXT. Noop advertises the multithreaded capability unconditionally,
so that caps bit is not an execution-mode oracle. With a known compiled profiler
and supported bridge, an isolated real counts/frame pump must yield paired
bgfx.cpu API-frame and renderFrame spans whose observed thread IDs equal the
public current-thread ID. A bounded cursor/stack checks matching IDs, no loss or
overflow and zero active work/current task after delivery. Profiling remains on;
unknown/off metadata reports an unavailable callback witness without certifying
mode. Core reset and trace toggles occur at synchronous API boundaries, with
zero active-work/current-task checks before creation, after initialization,
before the setup reset and after real teardown. This fixture does not cover a
concurrent render worker, GPU fences or performance.

The fixture constrains full payload words, same-invocation link receipts,
independent cached acquires, interface mismatch, real FS overlay and partial
creator release. A real stock carrier with rejected magic reaches the actual
Noop create_shader API, returns INVALID and retains full payload/failed-scope
observation without a fictitious link; the real surviving VS creator is released.
Starting capture after both real stage creations uses the transparent create port
only to enable Core trace after the second real return. Missing receipts retain
reason 1/zero IDs through the actual public loader/link owner. No API return or
DIAG policy is faked. Normal reason-3/4 scenarios remain separately unverified.
Actual latched-off, trace-off and thread suppression must
perform no producer SHA/clock observation. A quiescent capture change retains
real stage resources while refusing old receipt IDs. The complete-TU ordinary
missing_sha_word, wrong_extent, wrong_receipt, missing_partial_record,
missing_gate and old_capture_receipt controls must compile then fail assertions;
inspect their actual executable failures independently of CTest WILL_FAIL.
Registration is in shader_provenance_controls.cmake with isolated host paths.
These CPU API records do not establish final program/GPU resource destruction,
shader execution, presentation or a whole-frame improvement.

`test_jce_renderer_display_adapter` retains the exact policy, actual init prefix
and startup diagnostic owner. Explicit vendor-only, device-only and combined
requests on both D3D backends observe window topology once per create across
fallback, retain all requested keys and never apply the display key. Observation
failure retains its reason/source identity; Auto-only selection limits do not
rewrite explicit-request diagnostics. Non-D3D Auto and explicit requests never
probe. The old_explicit_observation complete-owner control restores only the
former explicit-request early return and must compile then fail ordinary Unity
assertions. These counted CPU observations establish neither actual GPU/display
LUID ownership, cross-adapter transfers nor a performance improvement.

The Web API admission fixture retains the actual complete post-init tail,
including its optional presentation-state and effective-reset publications.
Its admission-only caller passes NULL outputs with the real private policy
and pinned bgfx init types. Complete backend-attempt controls separately own
those output-publication assertions; the Web fixture retains its version,
context-epoch, refusal and once-cleanup assertions and ordinary old-wire
controls. Neither fixture certifies browser execution or presentation.

`test_jce_offscreen_resource_diag` consumes the complete actual offscreen owner
and exact renderer session-admission/accessor blocks. Typed counted texture/FBO,
view and log ports retain the pinned borrowed-attachment reference boundary; no
backend device, native window or logger worker starts. DIST plus trace-off checks
default/flag/session policy, actual clamped allocation vs forwarded view rect,
stable tuple silence, resize identity, destruction, rollback and retirement
saturation according to the real owner semantics. A failed allocation after a
successful retirement leaves no current RT; only retirement-capacity refusal
keeps the current one. Ordinary whole-owner requested_as_actual, missing_view,
missing_dedupe and missing_gate controls must compile then fail assertions.
This does not certify GPU allocation/execution, final pixels or speed. Register
through offscreen_resource_diag_controls.cmake; the existing real Noop fixture
continues to own integration of real renderer/device retirement and trace events.

The same complete-owner fixture now requires each successful main/overlay
renderer.view_configure service to own exactly 22 ordered resource-identity,
actual forwarded-rect and CPU-ordinal counters. Independent counted allocation
formats/flags and returned view bindings qualify the tuple; failed entries own no
successful tuple. Real Core ring checks retain per-call parent/thread/closure,
capture-origin gaps, zero/wrapped ordinals, disabled/suppressed tracing, full
resident-drain accounting and zero active work/waits. The Noop fixture separately
retains strict original allocation-scope counter and lifecycle-event counts
across unchanged prepares while requiring each configure's new rectangle packet.
These are CPU configuration observations, not backend acceptance, GPU completion
or evidence of final displayed texture. The missing_rect_height whole-owner
negative belongs in the existing registration once that production wire exists;
its ordinary Unity failure must be inspected independently of WILL_FAIL.

The diagnostic log port formats each message into the actual private
JceLogMessage.message[512] before any pair reassembly; a larger assembled record
is only a test assertion view, never the logger's formatting capacity. Schema2
parts must match all four identity/ordinal keys and remain complete, including
the final CPU/GPU-known fields. The formatter-only maximum-width group safely
passes UINT64_MAX/UINT32_MAX/UINT16_MAX values without allocating or accessing
backend handles, checks the longest 455-character part and fail-closed ordinal
exhaustion. The old_single_line control restores the actual former single-call
layout and must compile then fail the same ordinary no-truncation assertion.

`test_jce_render_pipeline_persistence` compiles the complete actual RP owner
and includes the unchanged complete host-FS owner with real SDL/files. Only
native write/close/rename/remove boundaries are counted or failed; the shared
atomic implementation is not copied or mocked. Existing valid asset bytes and
public reload survive partial write, failed close and refused rename; retry
publishes the replacement once. Invalid requests do not touch prior bytes.
The old_direct complete-RP variant changes only the publication call and must
fail ordinary assertions. Private inclusion is confined to this fixture and its
independent CMake registration. Per-target work directories and owned child
paths isolate sequential native controls; this is not concurrent writer or
fsync/power-loss durability qualification. Workspace INI remains separate.

`test_jce_renderer_perf_snapshot` consumes the complete actual end_frame wrapper
and private snapshot entry with the real Core ring. Typed stats/caps/environment/
log ports cover trace/PERF_LOG 00/01/10/11, first/existing cadence before environment
reads, rejected submission, fallback/headless, existing flag semantics and missing
provider data. Both enabled share one getter and one global/view counter set;
PERF_LOG-only logs never enter the ring, and both off never query stats/caps.
The old_scope_gate complete-wrapper control must compile and fail ordinary
assertions. This fixture may include pinned bgfx C99 declarations solely to type
borrowed stats/caps; it never initializes a device or queries a GPU. Register via
renderer_perf_snapshot_controls.cmake. Existing lifecycle-adapter controls count
snapshot dispatch independently of the actual owner's observer admission.

`test_jce_postfx_taa_ownership` reuses the existing complete PostFX TU fixture's
pinned bgfx reference-count provider (no parallel ownership model), with Noop
only for ordinary constructor resources. Eight groups call the actual target
allocation owners and public feature/resize/destroy APIs: motion-only lifetime,
validation/handle/texture failures and retry, history-plus-motion reuse/toggles,
independent history failure, and idempotent retirement. Existing TSR/AE groups
remain in their original target. Whole-owner old_history_gate, old_transfer and
missing_rollback controls must compile then fail ordinary assertions; their
controlled handles never reach a GPU. Register via postfx_taa_ownership_controls.cmake.
The typed provider follows pinned bgfx_p.h:5549-5602: validation rejects before
ownership mutation, but handle exhaustion still applies destroyTextures.
These controls prove CPU release requests, not driver OOM, GPU release/fence,
actual motion-blur pixels or application performance.

The same PostFX ownership fixture now calls the actual ping-pong allocator,
public versioned output query, empty/failed apply, resize and retirement. It
covers complete-pair admission, exact narrowed allocation extent, independent
slots/reuse, unavailable/borrowed outputs, valid zero handles, size/version
refusal and validation/handle/texture rollback. New exact-source controls omit
metadata/retirement/pair guards, substitute requested extent or mislabel
borrowed output, and reject zero. Qualified initial RED stages the new query
and fixture against the unchanged old allocator (the original had no query),
so it is not represented as a run of the untouched original API. Every mutant
must run and fail ordinary assertions; no GPU work is submitted for controlled
targets and no performance improvement is certified.

`test_jce_render_pipeline_publish` consumes the complete actual RP owner with
controlled backend/tier providers and the real Core trace ring. It asserts
all ten effective counters, exact scoped identity/parenting and success closure,
tier re-resolution, publication before observer reentry, newer policy last,
immediate observer-registration synchronization before explicitly armed reentry,
disabled tracing and no publication from ordinary getters/unchanged end_frame.
The whole-owner missing_publish control must compile and fail unchanged
ordinary assertions. Register via render_pipeline_publish_controls.cmake.
These records prove effective CPU policy publication, not GPU pass execution,
presentation, final pixels or a performance improvement.

`test_jce_volfog_shadow_fade` and its GLSL variant consume the complete actual
volfog shadow-depth/clip/split/tap/sunlight helpers. The generated C++ fixture
changes only two swizzle spellings and decimal-literal float suffixes; typed
vector/matrix, sampler and scalar-math ports supply CPU execution and operation
counts. Seven groups compare the actual helper with its same-source old path,
covering padded cascade counts, blend/fallthrough, all fade boundaries, disabled
sun, invalid/degenerate reach and NaN view depth. Fully faded valid samples must
retain visibility while making no matrix/texture calls. The old-no-early-out
variant removes only the actual shortcut and must compile then fail unchanged
ordinary assertions. This is Engine-domain CPU control, not shader compiler,
undefined-input, physical texture-fetch, GPU quality or timing certification.
Register through volfog_shadow_fade_controls.cmake; preserve shared generated
fixture write-if-changed behavior. Float-to-int conversion of invalid counts is
outside the valid shader contract and must not be exercised as a CPU UB test.
Distinct non-identity matrices additionally check known clip coordinates and
non-unit homogeneous w for all four selectors and the helper's final fallback.
Optional HLSL branch hints are outside CPU execution; these controls never claim
shader branch selection or GPU work reduction from C++ operation counts. The
matrix-hint experiment was removed after whole-frame timings showed no benefit.

`test_jce_ktx_container` exercises real first-party bridge/bimg writes and
independently described KTX1 fields, mip/face bytes and legacy headers. Malformed
shape/type/length/mip and narrow-integer inputs must refuse without publishing
outputs. Single/full chains match the backend's hasMips contract; partial chains
are unsupported. Each target has its own working directory. The complete-owner
old-untyped, missing-header and missing-chain controls compile and fail ordinary
unchanged assertions; none initializes a GPU or runs unsafe historical shifts.

`test_jce_probe_convolution` additionally checks the 256/512 complete 9/10-level
cube layouts, integer/address refusal, real RGBA8/RGBA16F high-resolution KTX
write/read with every face/mip marker and actual 256 HDR convolution of the 1x1
tail. The independent scene probe bake resolution fixture checks the real
default/authored/clamped file dimensions and mip headers. These are CPU file
and convolution controls; they do not certify GPU sampling or bake performance.

`test_jce_screenshot_completion` includes the complete actual callback/PNG
owner with real SDL encoding, filesystem results, trace ring and writer threads;
typed GPU ports and deliberate allocation/thread failure ports remain the only
substitutions. Its identity-keyed RENDER COUNTER assertions require one admit
and the unique actual SUCCEEDED/FAILED/CANCELLED terminal for real PNG writes,
invalid pixels, early readback allocation failure, shutdown cancellation and
the existing active-writer drain/reopen/borrowed-buffer cases. Repeated/late
identities cannot append terminals; uint64 identities and caller task TLS are
preserved, rejected/off-trace admissions cannot create orphan ledger entries,
and no active task is invented. The previous complete callback must fail these
ordinary ledger assertions with a normal Unity terminal; no WILL_FAIL wrapper
substitutes for that negative evidence. These CPU controls prove neither GPU
completion nor file durability.

Follow the repository and tests/ charters. GPU devices, native windows and pixel
acceptance are separate from these counted CPU owner controls.

The actual central-submit extraction in readback controls includes the canonical
private readback declaration before consuming the body. Context-admission
controls use a typed counted collector dependency and require exactly one
collection per accepted backend frame, with the actual backend serial and no
collection on refused calls. They do not substitute for the complete queue/
capture lifetime fixture under middleware/scene.

The existing readback fixture also consumes the complete actual
jce_renderer_destroy_impl body, including its NULL/fallback branches and all
unrelated teardown calls through typed inert CPU ports. Pending and cancelled
real queue destinations must remain borrowed until the controlled bgfx_shutdown
return; actual after-stop cleanup releases them once and a detached wrapper may
then be destroyed without another backend call. No cleanup order is copied.
The configure-time real-source order guard remains. The ordinary
`test_jce_render_readback_early_backend_stop` control swaps only the backend
shutdown and after-stop calls in that extracted fixture body; it must compile
then fail the same guarded-free assertions through normal Unity termination.
Negative cleanup disposes wrappers before assertions and never writes through a
freed submitted pointer. This certifies CPU borrow ownership through the actual
production call sequence, not a GPU fence, device-idle, transfer or presentation.

`test_jce_renderer_trace` retains the original complete lifecycle-adapter
assertions and also consumes that same strict complete central-submit extract.
Typed CPU ports count the actual frame call, readback collection and existing
retirement/upload fan-out; no backend is initialized. The real Core ring checks
the four submission identity counters inside their actual B/E scope, restored
parent, generation above double precision, retained/unavailable generation,
zero/wrapped serials, unchanged serial return, refusal and disabled tracing.
`test_jce_renderer_trace_missing_submission_packet` removes only the actual
packet call from this extracted owner, retains its frame/scope/fan-out behavior,
and must compile then fail ordinary unchanged assertions. The existing context
and readback fixture renderer shapes carry the real public payload-stats type
solely for this private identity read. These controls certify CPU identity and
dispatch, not a physical GPU interval, readback transfer or presentation.

The single internal fixture `test_jce_renderer_reset_trace.c` may include the
pinned `<bgfx/c99/bgfx.h>` and link bgfx::bgfx solely to type actual private
reset/stats/platform declarations and constants before counted ports replace
backend calls. This is a narrow exception to the parent public-API-only default,
not permission for other tests, SDK consumers or upstream changes. No backend
initialization is allowed in this fixture.

`test_jce_renderer_reset_trace` includes strict extracts of complete actual
resize/rebind/vsync/MSAA entries and the shipping reset trace helper. Actual
bgfx declarations type the counted backend ports; real Core ring events verify
parenting, closure and ten fixed reason/request/call/outcome details. Eight
groups retain odd/zero sanitization, partial-size stats fallback, native rebind,
unchanged/refused entries and disabled/suppressed behavior.
Known raw zero resize requests and unavailable MSAA request placeholders have
independent literal true/false assertions, never a nonzero-derived oracle.
The complete-helper old-missing-details control must compile and fail ordinary unchanged assertions;
it keeps the real reset call and balanced scope. CPU call-returned does not prove
backend-applied extent, swapchain recreation, presentation or GPU completion.

The same reset fixture also compiles the complete renderer launch-policy owner
and actual Vsync getter. Added groups retain forced on/off through later
requests and MSAA/resize, ordinary absent-override behavior, unavailable
headless/fallback classification, new object reset and real getter observations
even for an inconsistent initial flag. Bind/unchanged requests do not reset or
query backend dimensions. The old-launch-override variant removes only sticky
Vsync resolution and must compile then fail the same ordinary assertions.
These tests do not establish native Vsync, final presentation or a measured gain.

The same complete-owner fixtures exercise observed hidden-window D3D12 Vsync
constraints on Windows: known HIDDEN masks only the effective Vsync bit while
visible, unknown flags, MINIMIZED-only and hidden D3D11 retain normal policy.
Initial bgfx attempt flags and the actual adjacent Renderer flags/policy
publication are consumed independently; refused attempts cannot overwrite either
out parameter. The existing display-adapter init-prefix controls retain their
unchanged selection assertions. Later setter, launch override 0/1/absent, latest
unbound/bound request and explicit rebind checks preserve MSAA bits and the one
original rebind reset, with no repeated native flags reads or unchanged-setter
reset/stats query. Initial-mask and later-mask whole-owner mutants compile and
must fail ordinary unchanged assertions; existing terminal, missing-details and
sticky-override mutants remain. Windows-only missing-mask controls are registered
only on that platform, since other builds do not implement this constraint.
Counted window/backend ports supply observations rather than eligibility logic;
these CPU controls do not prove a native fence fix, visibility notifications,
swapchain application, Present completion or GPU performance.

`test_jce_volfog_density_march` extracts the complete actual march from ray
length/step sizing through gl_FragColor, including IGN and the original loop,
sunlight calls, scatter/transmittance recurrence and early exit. Its reference
removes only the uniform-zero-height optimization and derives the old work from
the actual per-sample fallback. Typed float/vector, exp and varying-sunlight
ports count operations and retain every sample position; no integration formula
is copied into the fixture. Finite zero/positive falloff, disabled sun, changing
visibility, zero extinction, fractional/integer steps through64 and early exit
compare exact final CPU float RGB/transmittance. Syntax adapters change only
the loop hint, swizzles and float literals. After stripping the hint, this march
slice has no remaining language-dependent semantics; one positive CPU target
is retained, with actual shader compilation qualified separately.
The old-repeated-exp control must compile and fail unchanged ordinary work-count
assertions; CTest WILL_FAIL is not its independent execution receipt. Register
through volfog_density_march_controls.cmake. These Engine-domain CPU controls
do not certify GPU fetches, framebuffer parity or timing, and do not repair the
existing public CPU256-versus-shader64 step-limit gap.

GPU annotation metadata controls reuse the complete actual bgfx callback owner
and real trace ring in test_jce_bgfx_profiler_trace.c. Missing, off, on and
Debug-with-option-off metadata ports retain request/option/compiled separately.
Whole-owner old controls remove only publication, mislabel unknown as off, or
substitute option for compiled state; they must compile and fail ordinary Unity
assertions. The failed-attempt fixture checks the same bounded publication.
These CPU controls do not certify dependency compilation or native GPU markers.

## PBR alpha admission order

`pbr_alpha_order_controls.cmake` registers `test_jce_pbr_alpha_order` and ordinary
`test_jce_pbr_alpha_order_old_order`. Strict unique boundaries consume the actual
shared PBR main through Normal, including UV transform, graph invocation, complete
fade-dither and complete material/view/mask policy. Only the complete shadow
calculation/contact/cloud block becomes a typed counted dependency. The old-order
reference/control moves the same actual material block after that dependency;
no alpha or discard predicate is copied as an implementation. Syntax adapters
change only the function name, swizzles and float literals, with an observation
at the Normal boundary. A discard exception terminates the CPU lane.

Seven compile-time branch combinations cover regular/tint/array/graph and fade.
Typed texture, graph, checker and existing LOD-helper ports provide inputs and
record consumption; they do not implement GPU derivatives or those helpers'
algorithms. Unchanged assertions compare accepted material RGBA and unlit output,
strict cutoff below/equal/above, opaque/mask/blend, factor/tint/graph rules,
array layer/transformed UV, raw checker coordinates, view2/3 return-before-mask
and fade complement with graph evaluation before fade. Mask rejects and unlit
returns require zero shadow-dependency calls; the old-order target must compile
then fail those ordinary assertions. CTest WILL_FAIL alone is not an execution
receipt. These CPU controls do not prove full lighting pixels, GPU work elision,
helper-lane/derivative/texture-LOD parity or a frame-time gain.

## PBR point attenuation dependencies

`pbr_point_attenuation_controls.cmake` registers `test_jce_pbr_point_attenuation`
and ordinary `test_jce_pbr_point_attenuation_old_fullwork` and
`test_jce_pbr_point_attenuation_epsilon_cutoff` controls. Unique actual boundaries
consume the complete stock brute-force point loop through the Spot boundary,
including the original count/pack iteration, squared falloff, shadow-slot and
cube/legacy selection, radiance expression and BRDF accumulation. The complete
actual direct-BRDF helper chain comes from `pbr_common.sh`. Syntax adapters only
change swizzles, float literals and the CPU entry name; a transparent BRDF wrapper
counts calls, and an observation before accumulation records computed radiance.
Only final shadow-sampling dependencies have typed counted ports. No attenuation,
guard, slot-selection or BRDF formula is copied into the C++ fixture.

Finite, well-defined inputs cover empty/inside/exact-radius/outside, every mixed
count through sixteen, all packed shadow groups, cube/legacy/missing slots,
Receive Shadows, a nextafter just-inside radius with positive tiny attenuation,
finite nonzero and negative colors and signed-zero color/intensity. Retained
indices and per-light radiance must match the old full-work reference; skipped
reference radiance must be zero and accumulated direct light must remain equal.
The old control removes only the exact-zero guard, and the epsilon control only
widens that guard. Both must compile then fail these ordinary unchanged work or
contribution assertions. CTest WILL_FAIL alone is not their execution receipt.
These are CPU dependency controls, not shader/GPU/pixel/performance certification;
NaN, infinity, invalid radius/material and undefined normalization are outside
the equivalence claim.


## Legacy GPU statistics context admission

`test_jce_gpu_timing_observation` consumes the complete caps owner and actual
renderer-context getter with typed backend ports. Legacy stats cases cover
before-bind, invalid payload/generation, after-unbind and Web loss: output is
zeroed unavailable and backend-query count stays zero. Bound Noop-like zero
timers and ordinary active timing/resource fields retain their prior values;
a missing backend still queries once and returns cleared unavailable output.
The ordinary `old_stats_context` variant restores only the legacy unguarded
lookup and must compile then fail the same counted-query assertion. These are
CPU API-owner admission controls, not a real device/context loss, GPU fence,
physical resource release or performance result.

The startup shader-root controls include the actual renderer-owned
jce_renderer_shader_dev.inc.h body with typed instance/device, allocator and
loader ports. They cover identity and API-thread/Web admission, copied root
lifetime, empty/unset zero allocation, repeated/foreign refusal, copy failure,
matching detach, named aliases and old PAK delegation. The ordinary old-PAK
routing variant changes only the actual overlay call and must fail unchanged
assertions, not pointer access or setup. These controls do not create a device.
The existing complete shader loader/Noop fixture additionally calls the compiled
private bind/load wire with actual host overlays and verifies partial-stage
fallback, invalid-stage refusal, caller-buffer independence and real resource
reference baselines. Noop cannot certify hardware compilation, pixels, shader
reload broadcasts or performance. Root mutations never erase old failed runs.

The complete foliage model-LOD owner fixture also covers a missing material
table and an out-of-range material index. Each emitted band must bind one
effective ordinary default material before the existing pre-submit callback.
These typed material/backend controls do not load a real glTF or render pixels.

## Graphics API policy admission

`test_jce_graphics_api_admission.c` includes the complete shipping caps owner
with typed backend/SDL/log ports, compiled separately for all four API policies.
The same real parser and pre/post-init comparators reject below-floor readings,
admit exact floors, preserve observed API/GLSL identity, clear cross-backend
carry and keep unreadable readings explicitly unverified. Querying the new lower
floor cannot mutate a stable build into compatibility. Vertex-fetch controls
retain explicit YES/NO and reject native unanswered support only in compatibility.
The ordinary old-range, missing-floor and old-vfetch variants change exact current
owner sites and must fail assertions. The fixture creates no graphics context;
its selected policy definitions do not certify a real Conan package, GPU shader,
XP Editor or public SDK consumer. SDK/binding and device integration are separate.

`test_jce_light_cookie_texture` includes the complete shipping private cookie
owner with typed bgfx memory/create/destroy and encoder-bind ports. It checks
no-array and compatibility refusal, modern atlas dimensions/white data, real
two-layer fallback, allocation/creator failures, legal handle zero and
idempotent cleanup. Complete-owner wrong-placeholder and missing-compatibility
mutations must compile then fail ordinary assertions in direct execution;
CTest WILL_FAIL registration alone is not their execution receipt. These CPU
ports create no device and establish neither image parity nor a speed gain.

`test_jce_postfx_grade_owner` consumes the complete shipping private LUT owner
with typed GPU-API/log ports and no device. It checks compatibility/no-3D
refusal, quiet neutral default, preserved borrowed settings, once-per-pipeline
unsupported diagnostics, modern params/tonemap gate, handle zero, allocation
and partial-creation failures, and idempotent cleanup. Whole-owner missing-
compatibility and borrowed-destroy mutations must compile and produce ordinary
assertion failures under direct execution, independently of CTest WILL_FAIL.
These CPU controls establish calls and ownership, not pixels or performance.

`test_jce_decal_admission` compiles complete actual decal create/destroy,
`sr_drive_decals` and explicit entity-cache reset bodies. Typed heap, shader,
profile, scene-iteration and resource ports check empty-demand zero acquisition,
one attempted failure, runtime-stamped updates despite authored failure,
explicit-reset retry, retained successful pools and partial creation cleanup.
Unrelated cache dependencies are inert typed ports; no culling/render algorithm
is copied into this admission test. Empty-demand, missing-retry and missing-
profile whole-owner controls must compile and fail ordinary direct assertions,
independently of CTest WILL_FAIL. These CPU checks are not graphical acceptance.

`test_jce_optional_stock_admission` includes complete shipping pick/normal
acquisition owners plus the actual skinned pick submit body. It consumes public
policy and the private actual-backend adapter, with typed shader/API-info/draw
ports. It checks compatibility refusal, unchanged modern/non-GL requests,
handle-zero static aliases, no depth-only normal acquire, failure latching and
actual INVALID-skinned early return. Missing-pick and missing-G-buffer profile
whole-owner mutations must fail ordinary direct assertions, independently of
CTest WILL_FAIL. No device, driver output, Editor behavior or timing is inferred.

`test_jce_bgfx_trace_sink` includes the complete actual callback TU and invokes
its real vtbl with typed environment/log/metadata ports under JCE_DIST=1.
Default-off, opt-in INFO, silent API capture, selector latching, basename paths,
format safety, newline stripping and explicit truncation are checked without
a device. The complete-owner old-LOG_INFO mutation must compile and produce
ordinary direct assertion failures, beyond CTest WILL_FAIL. These callback
consumption controls do not prove an upstream trace emitter or driver result.


`postfx_agx_controls.cmake` extracts the complete actual composite tonemap
operators, dispatch and combine functions, with unique boundary checks and
syntax-only typed C++ ports. The shared ordinary fixture verifies grayscale,
color/HDR, finite/range and non-AgX/output-gamma behavior. Its missing-EOTF
variant removes only the real transfer and must compile then fail normal
assertions, separately from CTest WILL_FAIL. Keep the shader as the algorithm
authority; no copied curve or GPU/pixel claim belongs in these CPU controls.

`csm_dynamic_filter_controls.cmake` extracts the complete actual dynamic PCF
helper, complete common-surface consumer and complete PBR dynamic block. Only
the PBR entry signature/preceding static result and typed vector/point-sampler
ports are supplied; float-literal suffixes are syntax adapters. Both GL row
conventions execute finite hard/filtered/disabled, depth bias, four-cascade
tile isolation, sharp-edge continuity, comparison coverage moments and static
min-combine cases at actual supported atlas sizes. The whole-owner equal-box
weight negative preserves all nine reads and addressing, and must compile then
fail ordinary continuity/moment assertions independently of CTest WILL_FAIL.
These are CPU arithmetic/address controls, not compiled GPU shaders, actual
atlas bindings, native motion/pixel acceptance or performance measurements.
