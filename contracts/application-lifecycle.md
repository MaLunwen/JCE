# Application lifecycle and observation contract

Engine and editor applications consume the same public runtime and service
interfaces. A host owns construction, event delivery, frame orchestration and
reverse teardown; a game owns authored behavior. A workflow or test fixture must
not duplicate the production loop to simulate successful consumption.

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

## Editor simulation transaction

The authoring snapshot must succeed before creating a mutable Play runtime.
On construction failure, owned services are released before restoring authoring
state. Clear the restore point only after a successful restore. A failed restore
remains available for an explicit Stop retry and prevents another destructive
Play. Scene roundtrips preserve authored rotation without repeated quaternion/
Euler conversion; legacy Euler input remains readable. Stop detaches borrowed render callbacks and input ownership before freeing
runtime services, then restores authoring state. Tests must cover failure and
retry, not only the successful Play/Stop path.

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

Portable application acceptance still requires editor simulation, a corresponding
platform executable and reopening a single-file Bundle. Relocation must work
without a source checkout, writable current directory, loose asset sidecars or
an external language VM. Deterministic stock renderer tables may be precomputed
using the same kernel and precision, with parameters and immutable content hashes;
never improve a benchmark by silently reducing quality or disabling a feature.
