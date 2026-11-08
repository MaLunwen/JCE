# Architecture and ownership

Read AGENTS.md and the closest module charter before changing a module.
The maintained map is contracts/module-memory-index.md.

- engine/include/jce/ owns the public C99 API; engine/src/ owns implementation.
- editor/ and examples/ consume public SDK interfaces. Consumer code does not include engine internals or vendor headers.
- scripting/ implements language adapters over the same C ABI.
- tools/ owns reusable automation with explicit inputs; scripts/ owns manual and forwarding entrypoints.
- A game's content, balance, packaging defaults and dedicated tools belong under examples/.
- contracts/ contains stable authorities. Local plans and delivery records belong under ignored docs/.
- Optional unpublished AI workflows belong under private/; public core builds must not require them.

Follow lower-to-higher layering from OS primitives through renderer, middleware, runtime and application to consumers. Reuse each service's authoritative owner in contracts/dependency-ownership.yml. A new wrapper is not a reason to duplicate allocation, scheduling, physics or rendering implementations.

Static-instance velocity resources belong to each SceneRenderer, through the
private prepare/release functions in `engine/src/middleware/scene/jce_sr_cull.c`.
Initialize invalid handles before failure paths; queue admission failure retires
the loaded program. Scene teardown releases the program and queue through their
existing owners. Multiple view renderers keep distinct state. Pair actual source
controls with rebuilt runtime tracing; a passing CPU fixture alone does not prove
backend allocation closure or GPU completion.

Depth-prepass queues also belong to each SceneRenderer, using private
`sr_depth_prepass_queue_prepare/release` functions in the same cull owner.
Keep lazy allocation, clear before pass admission and preserve inline rendering
when allocation fails. Destroy releases the queue and allows a fresh lifecycle.
Constructor rollback releases every successful cull-cache allocation when the
next allocation fails. Verify real owner paths with failure injection and
compiled old-behavior controls; these CPU checks do not prove total process
heap closure or GPU completion.

`engine/src/renderer/jce_text.c` owns live immutable font-source sharing. Read
actual content before matching the public archive content hash, size and full
bytes; names and mount identities cannot certify equality. FT/HB faces, sizes,
SDF settings and atlases stay independent. Caller buffers remain caller-owned.
Close HB/FT before releasing the last source reference; refs zero immediately
unlinks and frees it. Stable blob IDs and existing trace scopes expose reference
and byte lifetimes, with explicit overflow refusal. Preserve the same-thread
font API contract. Source sharing does not establish peak, RSS or IO savings;
measure them on actual builds and keep other font engines separately owned.

`engine/src/renderer/jce_shaders.c` owns stage-to-program linking. Disable bgfx
automatic stage destruction and release both creator references explicitly on
success and failure; a successful program retains its own references. Native
PAK, filesystem, overlay and deferred realization use the same private owner.
Preserve asset/backend failure logs and common link outcome/release counters.
Counted runtime contrasts and actual Noop stock-byte lifetime tests establish
their respective ownership scopes, not physical shader execution or pixels.

`engine/src/os/core/jce_trace_capture.c` reuses the existing Chrome encoder's
bounded consumer buffer to merge host writes near 1 MiB. Keep the producer ring,
all lifecycle events and completeness gates unchanged; full drain budgets resume
without idle sleep, and encoder/short-write/CloseIO failures reject capture.
Join before freeing staging. Footer writer statistics describe this consumer's
CPU/IO attempts and pauses, not application or GPU performance certification.
Require a fresh caller-exclusive final destination: the private host-FS reserve
owner exclusively creates it with SDL's `wbx` mode and invalid-JSON PENDING bytes,
then exclusively creates a nonce sibling partial. Path-query failure never proves
absence. Verify footer write/CloseIO, complete history and byte-exact unchanged
reservation before public host rename replaces only that marker. Failed markers
and partials stay diagnostic data, even when a partial contains positive JSON;
readers must reject PENDING and never substitute a partial for the requested
final capture. Existing outputs are refused. The caller prevents concurrent or
malicious external replacement; read/rename does not claim that protection.

Public `jce_fs_host_copy_file_new` streams an explicit host source into a newly
exclusive host destination with at most64KiB scratch. The mandatory out_created
starts false and becomes true only when this operation creates the destination;
it remains true on later IO/close failure. Existing files/directories are never
overwritten. Success requires complete transfer, terminal EOF and both closes.
Only a created destination belongs to the caller for cleanup. Existing FS trace
scopes expose transfer/close outcomes; this is not fsync durability or a stable
snapshot of a concurrently changing source.

The private scene jce_scene_line_colors_internal.h shares the LineRenderer
colour-field schema between full JSON parsing and strictly admitted partial
colour updates. Preserve generic fallback, inactive-storage canonicalization
and the existing typed setter/OnSet. No live ECS pointer or serialized mirror
survives the call. CPU allocation and owner controls do not prove whole-frame
speed, pixels or GPU completion.
