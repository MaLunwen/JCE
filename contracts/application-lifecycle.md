# Application lifecycle and observation contract

Engine and editor applications consume the same public runtime and service
interfaces. A host owns construction, event delivery, frame orchestration and
reverse teardown; a game owns authored behavior. A workflow or test fixture must
not duplicate the production loop to simulate successful consumption.

## Asynchronous readback ownership

Cancellation ends the caller's intent, not the backend's borrow of a CPU
destination. Pending destinations remain owned by the canonical readback service
until trusted same-backend frame progress establishes readiness or the backend
has stopped. UI cancellation and deferred destruction do not submit render frames.
Ambiguous serial/epoch observations retain the destination; resource admission
may refuse new work while retirement is backpressured.

Editor capture maintenance runs within the admitted host draw before layout,
independently of whether a Scene panel renders. Completion, failure or cancellation
during that UI frame retains its view-band reservation through the frame. Scene
replacement, history reconstruction and shutdown cancel capture before destroying
borrowed scene/render owners. These CPU ownership rules do not prove GPU pixels
or acknowledge hardware completion beyond the readback service's authority.

## Finite host terminal acknowledgement

The exported CPU trace ends before its own writer, final logger, SDL and Engine
allocation return path completes. That prefix and a process exit code alone do
not certify the tail. Optional host acknowledgement reuses the synchronous
owned-destroy result, one private session ticket and one final cleanup scope.
It emits once to an already redirected stderr after the Engine call returns and
the corresponding owner is empty; SDL waits for final AppQuit and frees its
callback context first, as JNI frees its bridge first. It never creates a
console, worker or file.
No-op destruction, rejected creation, retained ownership or an older ticket
cannot produce a successful current-session acknowledgement.

`JCE_TERMINAL_ACK_TOKEN` is an explicit diagnostic opt-in. The measurement tool
generates a fresh token and validates the original owned-child stderr. With
requested continuous capture, its final scoped `engine.terminal_session` counter
must match the acknowledgement and the complete, unchanged trace file. Missing,
duplicate, malformed or mismatched records remain failures. Default process
sampling stays engine-neutral. Logger flush/close fields describe actual API
results, not durable storage; acknowledgement does not certify process teardown,
unregistered user owners, GPU completion or presentation.

## Event lifetime

Application lifecycle events are main-thread broadcasts. Priority ties retain
registration order. Each emit snapshots listener identities at entry. Removed
listeners do not fire, newly registered listeners wait for a subsequent event,
and nested emits take independent snapshots. Shutdown cancels active emits and
invalidates old listener ownership. Old handles cannot remove new listeners.
Emission allocates no storage, including LOW_MEMORY delivery. Registration
allocation failure preserves the existing listener set and permits retry.
Focus/pause getters reflect the latest emitted event, including nested events.

## Application callbacks

Update and draw are independent optional callbacks. If both are present, update
runs first. The host retains the historical combined app_update CPU metric and
publishes app_simulation and app_draw independently. Timing scopes measure CPU
work on the host thread; submission is not proof of GPU execution or presentation.

Engine event delivery, iteration and destruction run on the host thread.
Callbacks borrow the engine and its services through the enclosing dispatch.
Destroy during an active frame or event callback requests quit and retains
ownership; recursive iteration during those callbacks returns CONTINUE.
Nested event delivery retains the outer dispatch lifetime. The host consumes
that quit request through the normal application result and calls destroy again
after the outer event/iterate call returns. The engine never frees itself before
the host receives that result. Terminal finalization and teardown reject recursive
event/frame/teardown entry.

The optional private instruction-pointer sampler must stop before the frame
capture writer, async executor or other borrowed application services are joined
or destroyed. An incomplete native collector join or unresolved owned suspend
requests quit and retains the engine for a later host retry. Successful stop
releases sampler ownership before the exit callback. Native suspend/context
calls, symbol lookup and report IO have no portable deadline; a CPU fake-native
control does not certify native host-thread recovery or bounded OS shutdown.

The shared async executor must stop before event-watch, modal-tick, application
or resource teardown. If shutdown is rejected from an active owner-thread
completion pump, destroy requests quit and retains the remaining owners for
retry after the pump returns. Frame capture may already have finished; this
deferral does not promise that capture remains writable. A terminal result
alone does not release the host's engine pointer or run the exit callback.

Default-executor lazy creation has a separate private Engine-session admission
latch under Core's existing default-owner lock. It starts open for standalone
consumers; their public default shutdown still permits recreation. An Engine
opens admission only after validating creation and publishing its allocated
owner, and closes it before entering DRAINING, including borrowed-constructor
or frame/event deferrals. Refusals, exit/subsystem callbacks, final release and
the inter-session gap cannot reopen it. Only the next admitted Engine create
does so; early creation rejection and legacy drain-only retry do not.
This closes lazy creation, not existing borrows: a retained default executor
remains queryable until its original owner-thread shutdown, and independently
owned executors retain their own contracts. Do not treat the latch as reference
counting, pointer revocation or a replacement for orderly producer shutdown.
The actual callback controls passed in the 2026-10-18 retirement-r3 Native run
(65/65); the close-wire negative separately failed three ordinary assertions.
See that date's verification record for artifact identities and scope.

Native hosts use `jce_engine_create_owned` and `jce_engine_try_destroy` to keep
the cleanup owner observable even when initialization fails. Creation reports
CONTINUE only for a ready engine. FAILURE may retain a nonnull owner; the host
must retry cleanup on that same slot until completion clears it. An occupied
output slot is rejected without replacing it. Construction callbacks borrow
the owner just as frame/event callbacks do. A deferred close enters draining:
no subsequent business event, update, resize or draw is admitted, and no second
engine session may replace the process-global services or application descriptor.

Legacy create/destroy symbols remain available. A legacy failed creation whose
cleanup is deferred retains one process-owned pending engine; `destroy(NULL)`
or a later legacy create retries it. This compatibility path is not a background
collector. A legacy create that finds pending cleanup only drains that owner and
returns NULL, even after successful cleanup; configure and create the next
session afterward. New hosts use explicit owner slots. Never report complete teardown
or export a successful final trace while producers still own borrowed resources.
Cleanup attempts retain their actual refusal reasons and final release outcome.

Terminal diagnostics have a two-stage barrier. After application producers stop,
join the continuous capture worker without finalizing its capture. Then drain and
join the dedicated logger under `engine.log_quiesce`, preserving its synchronous
file/sink path for final export diagnostics. Only then close the cleanup scope and
main trace registration, finalize the capture, and release logger storage before
SDL shutdown. Capture worker availability records historical creation, not whether
its pointer remains nonnull after this join. The logger's raw SDL worker is not
counted by the trace thread registry; its completed quiescence scope records the
owner's native join. Neither observation is a physical GPU completion fence.

The analyzer's opt-in `--cleanup-ownership` verifies each captured attempt's
submitted service, scope, thread, ordered reason/completed fields and matching
terminal state. Earlier refused attempts are allowed; the final captured attempt
must succeed, within a complete continuous stream. Generic `--require` semantics
are unchanged: do not require `engine.cleanup_attempt` when failures are expected.
Use an actually successful phase such as `engine.async_shutdown`. No stable Engine
session identity is present, so this proves captured attempt outcomes, not a
process-wide resource ledger or the post-export allocation/SDL exit tail.

SDL drains through spaced host iterations before returning its original terminal
result. Its per-application callback context owns that session, without global
host storage. SDL calls AppQuit once, including failed Init, and never supplies
appstate afterward. Final teardown makes one attempt: successful release frees
the context before its only ACK, while refusal logs unresolved process-lifetime
retention and emits no ACK. There is no promised post-AppQuit retry. A foreign
event rejects before context access; reentrant final release cannot free twice.
JNI keeps the bridge handle until native cleanup
completes; Java close must not clear that handle when native cleanup throws a
retryable refusal. Typed host controls establish callback/handle ownership, not
physical window delivery, native sampler recovery or a universal exit deadline.

## Completed render capture

Raw frame capture records completed rendered iterations, with contiguous ordinals
independent of the application update counter. Initialization, headless ticks and
incomplete iterations do not become rendered rows. A normal next-iteration
boundary rotates and exports the preceding render; terminal full completion
rotates and exports its current render before writer finalization. Quit between
iterations and explicit host destruction preserve a pending completed render.
The existing owner gate prevents duplicate terminal capture and callback teardown.

Wall intervals retain monotonic counter precision; simulation dt is separate.
CPU phases require the matching uninterrupted enabled boundary. Missing phase
attribution remains unavailable while the actual wall interval remains present.
A clean writer footer proves record and byte completion for its declared window.
Timed acceptance separately joins that window to native clock observations and
its actual final rendered count; a configured limit or log marker is insufficient.
Explicit capture limits may declare a prefix. Preserve failed and partial windows
with their measured coverage rather than promoting them to whole-run results.

## Editor simulation transaction

The authoring snapshot must succeed before creating a mutable Play runtime.
On construction failure, owned services are released before restoring authoring
state. Clear the restore point only after a successful restore. A failed restore
remains available for an explicit Stop retry and prevents another destructive
Play. Scene roundtrips preserve authored rotation without repeated quaternion/
Euler conversion; legacy Euler input remains readable. Stop detaches borrowed render callbacks and input ownership before freeing
runtime services, then restores authoring state. Tests must cover failure and
retry, not only the successful Play/Stop path.

Game's stopped preview camera is an independent host state, not part of the
authored scene JSON restore point. Play uses an owned complete camera clone;
camera-less scenes inherit the preview pose and projection. Allocation failure
participates in admission rollback. Stop, stopped-state retry and host shutdown
retire this camera even when Game is hidden. Borrowed getters select the current
owner and cannot be retained across its retirement. Camera changes invalidate
the viewport's temporal continuity until an actual apply consumes the cut.

Normal editor host exit closes an active Play or paused runtime through this
same Stop transaction before viewport renderers, panels or scene owners are
destroyed. Edit-mode exit without a runtime remains a no-op for simulation.
Script destruction must actually occur while its host services are alive;
a successful application-exit span or complete trace footer cannot substitute
for that callback and teardown evidence.

## Observation and acceptance

Use the existing bounded jce_trace producer and Chrome/Perfetto export. Nested
synchronous scopes preserve their parent task. Export after producers join for
stable process-lifetime observations. The exported metadata distinguishes the
prefix overwritten before reading from records lost during reading; a reset
invalidates completeness. Task completion retains success, failure or cancellation.
A requested export enables capture, including builds without an external profiler.

A report names its required phases explicitly, preserves the raw input identity
and rejects empty, unmatched, failed, cancelled or incomplete observations. The
resident CPU timeline cannot certify every engine service, GPU completion,
acoustic latency, old-device performance or absence of resource leaks. Validate
those separately with appropriate clocks, captures and ownership observations.

Long-running coverage uses an optional cursor consumer of the same bounded ring,
not a second producer registry. JCE_TRACE_STREAM names its output; native hosts
use a consumer worker and Web hosts default to cooperative pumping. Explicit
JCE_TRACE_STREAM_MODE selects threaded or cooperative controls. Capture IO
suppresses itself per thread; other application producers remain observable.
The engine starts capture after host initialization and stops it only after
producer shutdown, including sampler and application worker joins. The final
drain retains thread exits and the consumer join. Default-off behavior must
remain free of a capture worker or capture-file writes.

A continuous footer reports consumer identity, write status, generation stability,
recorded/captured/lost/unexported counts and remaining active threads/work/waits.
Successful capture requires exact count accounting, zero loss and unexported
records, successful writes and no active producers. A lagging consumer, failed
write, reset or unfinished producer is incomplete and must never be accepted
by dropping events or relaxing required phases. Performance controls disable
capture; instrumented coverage is separate evidence.

Portable application acceptance still requires editor simulation, a corresponding
platform executable and reopening a single-file Bundle. Relocation must work
without a source checkout, writable current directory, loose asset sidecars or
an external language VM. Deterministic stock renderer tables may be precomputed
using the same kernel and precision, with parameters and immutable content hashes;
never improve a benchmark by silently reducing quality or disabling a feature.

Offscreen targets retire replaced GPU handles against the renderer's actual
submitted-frame clock, not repeated prepare calls or a GPU-completion fence.
Collection continues when the target is hidden. If the bounded eight-slot
retirement queue is full, a new resize request returns false and preserves the
current target; preparation at its unchanged extent can still succeed. Renderer destruction detaches all associated GPU handles before backend
shutdown; the caller can destroy the detached wrapper later without issuing GPU
calls. Consumers use public offscreen APIs and ordinary renderer/application
lifecycle, never the private submission seam. Real Noop resource-count controls
prove these ownership transitions; GPU quality and Bundle reopen observations
remain separate evidence, not universal backend/device safety certification.

Resource-specific acceptance must retain actual allocation, token, context and
submission identity. The shared instance owner emits scoped observations for
upload acceptance, staging transfer/release or cancellation, and handle retirement
and destruction. Its opt-in metadata budget is bounded; missing metadata or an
origin outside the current capture explicitly invalidates complete resource
observations while normal rendering remains available. Match each resource's
lifetime, not just aggregate end counters. Backend release callbacks may outlive
the renderer owner; the independent observation state retains its own references.
A balanced CPU resource ledger is not a GPU-completion fence or physical-residency
certificate, and the generic capture footer does not imply that ledger closure.

Editor cold restore binds a valid declared project through the same Open
Project transaction before deserializing its scene or Bundle, after editor
state and VM owners exist. Missing or malformed declarations do not mutate
project ownership. Ordinary source scenes follow their nearest project through
the public resolver; a relocated Bundle preserves valid explicit authoring
context and uses its physical ancestor only as fallback. A fixed directory-depth
limit or accidental source-tree ancestor cannot certify relocation. Required
native classes must actually load and run in deep isolated-context Play/Stop
controls; scene restoration alone does not prove gameplay ran.

## Optional same-interval viewport observations

Default FrameLog bytes and explicit window-v1 context remain compatible. Only
explicit frame-context mode2 admits the public sized host observation seam.
The current ready Engine's main-thread app.draw callback may report at most
four unique nonzero source IDs once per interval. Copy the known uint32-only
prefix immediately and retain no caller pointers. Invalid size/stride/count,
unknown typed values, wrong-thread/outside-draw calls and duplicate reports
refuse without changing a previously accepted observation. No report and an
explicit empty report remain distinct. Begin clears the report; completed
render seal, delayed export and terminal pending export retain that same packet.

Availability flags qualify requested, allocated and display extents, texture,
Image invocation counts, queued-command counts and CPU submit counts. Prepare
success and CPU outcome are separate facts; retained resources are not proof of
fresh rendering. Main and recording submits are separate. Unknown stays null,
measured zero stays zero, and consumer-defined refusal reasons retain uint32
values. These observations do not acknowledge GPU execution, presentation,
resource fences, displayed pixels or complete application feature acceptance.
The existing bounded writer and independent strict schema readers remain the
only record/export owners; mixed context versions cannot share a capture.

The shared SDK default entry assigns source1 to its current standalone scene
attempt. Its local static helper publishes both existing draw-admission returns
as CPU_SKIPPED (host-policy reason1 or missing-owner reason2). A new invocation
starts without prior state; a direct fallback replaces bridge request/allocation
and prepare fields. Requested scene-config dimensions and get_extent's actual
admitted offscreen allocation are independent; retained targets do not imply a
successful prepare. CPU_COMPLETED requires the scene-render call to return and
the callback tail to be reached, including an internal renderer refusal that
returns the same view ID. An incomplete entered CPU path remains reason3.
Display/texture and all ImGui counts stay unavailable: a requested destination
rectangle, void frame setup or void PostFX present is no observed display or
submit success. Screenshot scheduling and normal admission remain unchanged.
Default/v1 incurs only the enabled query, with no observation clear/extra query.


## Shared audio master final barrier

The process audio master belongs to the Engine session rather than an individual
Play graph. DRAINING closes new device/graph/tap admission; only an admitted next
Engine opens it. Callback-context Engine creation/destruction refuses before
service waits, including same-main-thread Web callbacks. After background async
producers stop and before app.exit begins, the private master gate closes and
waits for actual user PCM/tap borrowers. Timeout retains all targets, the device
and the same Engine owner for retry; it is never permission to free a target.
After quiescence, native device uninit joins before app-owned graphs/tap contexts
can disappear. Native join latency has no portable upper bound. The successful
`engine.audio_master_retire` CPU scope certifies this owner return, not physical
speaker drain or GPU/presentation completion. Last-graph/Play Stop preserves the
resident device and gapless silence; final session teardown alone retires it.
Legacy graph/tap release remains synchronous and may block behind user callback
code. Callback-context release refuses before mutation, so it is not a completed
destruction; callers must preserve their owners until valid serialized owner release.
Foreign caller graphs are disarmed but never implicitly freed by Engine cleanup.
