# engine/src/renderer — Renderer Layer (L3)

Debug line flush uses LEQUAL without depth writes so coplanar floor/collider
edges survive equality and the overlay does not poison later scene depth.
The private `jce_debug_line_sample.h` copies complete vertex pairs across the
whole queued range when remaining transient capacity is insufficient. Preserve
the exact-copy path when all lines fit, endpoint/attribute pairing, even vertex
counts and the once-only capacity warning; a queue prefix is not a faithful
substitute for distributed collision detail. The fixed CPU queue still reports
overflow; the Play overlay applies its smaller per-view budget before admission.

Accepted canonical readbacks emit transition-only `readback.operation` packets
from their existing slot owner. Capture-local operation/context/ticket identity,
provider promise, observed serial, readiness and actual CPU release remain
separate fields. Repeated pending polls add no packets; capture-generation
changes leave old work unlinked. Ready observations use the existing wrap-safe
provider contract, including valid zero/MAX serials, not a whole-device fence.
All native/headless/rollback/destroy `bgfx_shutdown` calls have a nested
`renderer.backend_shutdown_cpu` interval; its return never asserts Present.
The legacy PNG/video staging path reuses the canonical RGBA16F blit/read-back/
2D-format admission before allocation or file admission. Unsupported requests
refuse and warn once per backend lifetime, reset after backend shutdown. A
black/transparent image is not itself a failure heuristic or proof of rendering.

Stock acquisition/cook profiles are declared once in contracts/shader-keywords.json.
Its existing generator emits the private profile table and machine catalog;
normal shader entries/public variant layout remain unchanged. Both PAK and FS
stock routes initialize every owned/variant handle to INVALID, then acquire only
the selected profile's programs. Compatibility applies only to a compiled
compatibility tier on actual GL/GLES. Its static-only table excludes unsupported
families rather than treating zero as missing; acquired handle zero stays valid.
Cached successful acquires still normalize before named aliases borrow them.
Declared catalog membership is not shader/link/device/Editor acceptance.

When object-ID readback is unavailable, model meshes own retained triangle
data. Rigid meshes retain positions and indices, while skinned and morph
meshes reuse their immutable source vertices. The CPU ray query is bounded
and uses one temporary buffer to apply morph and skin data on demand.
A failed or over-budget source reports unavailable rather than a partial hit.
`JCE_DBG_PICK_CPU=1` forces this route from launch for QA.

Compatibility GL/ES keeps the GPU skinning family excluded. Its skinned meshes
retain immutable base vertices for the private CPU deformation path.
The CPU deformer consumes the existing model-space joint palette after any
morphing and leaves material attributes intact. Posed geometry binds through
the existing static programs, using a per-instance dynamic buffer or a frame
transient source for direct model draws. The renderer does not own animation
sampling or editor pick state. Modern non-morph meshes retain no CPU copy.

The public compatibility API tier appends value 4 after the legacy COUNT
sentinel 3. The renderer maps cached bgfx package ordinal 3 to public policy 4.
Public COUNT stays invalid for name and floor lookup. The compatibility package
uses GL 2.1 / GLES 2.0 and checks observed versions before and after initialization.
Its Vulkan 1.0 policy row is advisory, not new dependency compile metadata.
Version admission does not certify required extensions, shader families,
offscreen formats or a usable Editor. A native UNANSWERED vertex-fetch field
is conservative for compatibility; explicit backend YES/NO remain authoritative
and the original native policies retain their existing guarantee fallback.

The shader loader owns the public blob-program create/destroy bodies in
`jce_shaders.c`. The blob path checks the uint32 backend size limit, submits
one owned copy at a time, and uses `link_program_owned` for creator release
on success or partial/link failure. Caller buffers remain borrowed.
The renderer submission path owns the actual backend frame call and raw
counter edges. Reuse its existing service and packet identity.

Native synchronous `jce_shaders.c` PAK/FS/overlay loading has optional CPU provenance
under the existing device session's JCE_RENDER_RESOURCE_DIAG latch and Core
trace admission. It fingerprints the complete actual bgfx Memory before
create_shader consumes it: eight digest-order big-endian uint32 SHA-256 words,
byte count, capture generation, known/unknown device generation and returned
handle. Stack receipts directly qualify that invocation's link and partial
creator release; no global handle registry, copied SHA or submit hook exists.
The native evidence wrapper keeps the existing shared link_program_owned body
and its destroyShaders=false plus explicit creator-release policy unchanged.
Its existing six child counters report actual link return and creator-release
calls. Void destroy return is not final resource destruction.

Missing, capture-changed, wrong-handle and unavailable/changed-context receipts
have explicit reasons and zero joined create IDs. Duplicate cached handles
remain independent acquisitions; these facts do not certify bgfx's internal
cache payload, program final references, backend execution or presentation.
The Web deferred resolver, other shader loaders, final program destruction and
all submit/dispatch paths remain outside this synchronous owner scope. Default
off neither hashes nor allocates nor reads a new clock; this is not a measured
zero-cost or frame-performance claim. Capture reset still requires producer
quiescence. Core owns SHA once; private policy reads reuse the device latch.

Static CSM PCF selects its cascade sampler before the shared csm_filter.sh
kernel. Tiers retain 1/9/25 depth-fetch budgets; filtered tiers use balanced,
staggered fixed rings rather than a per-fragment hashed rotation. Opposite taps,
zero centroid and equal X/Y second moments guard directional bias. Reciprocal
map size honors all positive resolutions. PCSS, texel bias, split blending and
dynamic-atlas clamping retain their separate policies. CPU shader-port controls
do not certify native compilation, visual acceptance or GPU performance.
PBR variants, graph/classic materials and unlit shader parsing also consume the
declaration; compiling only the default entry does not qualify them all. Static
shadow targets retain one mip, point filtering and clamp. Revisit derivatives
and implicit LOD before introducing a filtered mip chain.

Grass CSM selects its cascade sampler once outside the unchanged four-tap PCF.
Coverage, bias, tap order, cascade fallthrough and edge blending remain owned
by the existing grass shader. Shadow targets currently have one mip and point
filtering; changing either requires revisiting implicit-LOD sampling within the
per-fragment cascade branches. D3D11 replay pixel parity or local event timing
does not certify dynamic boundaries, another backend or native whole-frame gain.

Mounted RenderPipeline boot resolution seeds only absent MSAA from the current
requested descriptor, so the stock launcher's legacy quality fallback precedes
an explicit canonical field. Private seeded host/PAK readers share the existing
strict JSON parser; public load/load_pak/load_fs retain their LOW defaults and
do not inherit active state. JSON key presence distinguishes explicit 1/off
from an absent field and preserves the existing legacy 16-sample fallback.
The final effective RP remains the only configuration policy consumed by the
stock launcher's existing renderer setter. This concerns swapchain requests,
not scene-geometry multisampling or backend/GPU acknowledgement.

The private renderer session reads JCE_RENDER_RESOURCE_DIAG once after device
admission, using the shared environment flag convention. Offscreen wrappers copy
that default-off policy. Explicit DIST-capable render-resource records describe
CPU create/resize publication, rollback/destroy-call return and the main view's
actual forwarded rectangle/FBO tuple after both void calls return. Allocation
extent and requested extent remain distinct; rollback extents are unknown, even
when staged dimensions exist. Existing creation serial/capture generation and
handles qualify identity. Only first/changed allocation/view tuples log; repeated
prepares neither re-read environment nor query the backend. Overlay/composite
view changes, Fog/other RT owners and subsequent overwrite by other view users
are outside this first-stage diagnostic. No record proves GPU execution, backend
allocation acknowledgement, residency release or presentation. Pure CPU controls
consume complete offscreen ownership plus actual session admission/accessors.

Separately, enabled Core tracing publishes the existing offscreen identity plus
forwarded rectangle width/height and CPU submission ordinal inside every
successful main/overlay renderer.view_configure service. Its 22 ordered counters
reuse creation serial, origin/current capture generations, actual view,
allocation extent, formats, single-sample policy and handles; overlay uses its
actual forwarded view id. Only successful tails publish this tuple, after the
existing view calls return. Allocation lifecycle consumers must qualify the
parent service and create_committed, not count repeated identity/logical-byte
appearances as new resources. First/change INFO diagnostics remain unchanged.
Complete-owner real-ring controls verify the tuple against counted allocation
and view ports; the missing_rect_height whole-owner control must compile then
fail unchanged ordinary assertions. These records describe CPU configuration,
not backend acceptance, GPU completion or the final displayed texture.

These diagnostics use schema=2 with exactly two bounded messages per observation,
part=1 then part=2. Each repeats event_ordinal, creation_serial,
origin_capture_generation and view; consumers require both parts with identical
keys before accepting a resource witness. The wrapper-owned ordinal advances
only for a diagnostic observation, not each frame. Its last UINT64_MAX event is
valid; a following attempt emits part=0 diagnostic_exhausted once and disables
that wrapper's diagnostics. Missing, malformed, duplicate or exhausted records
cannot prove the resource observation. Part1 owns allocation/phase/submission;
part2 owns request, forwarded rect and destroy/CPU/GPU-known fields. Both fit the
actual logger's 512-byte message including NUL even at maximum integer widths
(402/455 payload characters); no logger buffer or rendering policy is expanded.
Steady unchanged prepares still emit neither part. Logger loss is unavailable
evidence, never permission to reconstruct a missing part from requested state.

PostFX TAA history and camera-motion targets keep explicit caller texture
references; their FBOs use destroyTextures=false. Pinned bgfx's validation
failure retains caller ownership, while post-validation handle exhaustion
still honors destroyTextures=true, so failure cleanup must never guess which
branch consumed a texture. create_taa_target publishes only a complete pair
and rolls back an unadmitted texture; history-valid publishes only on success.
Motion can exist without history when only motion blur is enabled. Resize and
destroy therefore retire each target's handles independently, clear them before
release and reset history validity. Disabling a feature retains reusable targets
until resize/destruction; no GPU fence or immediate residency claim is made.

RenderPipeline load_fs uses the public explicit archive-only bounded read with
a 1 MiB limit before decode. All loaders share the strict JSON-object parser;
the explicit-FS loader clears output on failure and does not apply. The public
get_requested snapshot preserves the last applied unclamped intent; deferred
edits are excluded until end_frame commits them. Temporary hosts must snapshot
that intent rather than the cost-clamped active descriptor.

Tracing-enabled actual RP application publishes one render_pipeline.publish
service before observer callbacks, with same-scope core counters rp.volfog,
rp.csm, rp.ssao, rp.ssr, rp.taa, rp.bloom, rp.mblur, rp.scale_milli, rp.shadow
and rp.cascades. The same publication includes rp.shadow_filter,
rp.backbuffer_msaa, rp.post_quality, rp.hdr, rp.depth_prepass and rp.ssgi.
Boolean queries include existing QA forces; numeric values follow resolved
policy. Backbuffer MSAA policy does not report offscreen target samples.
Queries and unchanged end_frame do not emit. This is effective CPU policy
publication, not pass execution, GPU completion or final framebuffer appearance.

RenderPipeline asset save publishes its existing serialized JSON through the
shared public atomic host writer. Failed write, close or rename retains the
previous authored file; a successful save does not itself apply the descriptor.
The shared writer's same-destination serialization requirement and absence of
fsync/power-loss durability remain authoritative. No second file writer or new
asset schema is introduced. Real RP/host-owner failure and old-direct controls
live in tests/renderer/render_pipeline_persistence_controls.cmake.

The private KTX bridge validates complete RGBA8/RGBA16F cube wire layout before
allocating decoded storage. Reads accept typed KTX1 or the exact legacy pinned
bimg zero-type triple; writes normalize checked, owned blobs to typed headers.
Single/full chains, uint16 edge, uint32 upload and encoder int32 wire limits are
explicit; partial chains cannot be passed to the backend's boolean hasMips API.
Upstream bimg remains unchanged. CPU roundtrip/malformed/ordinary controls live
in tests/renderer/ktx_container_controls.cmake; external-tool and GPU liveness
require their own actual evidence.

Cube-source probe convolution uses a checked complete chain through 1x1:
256/512 edges require 9/10 levels. Count, RGBA8 byte sizing and both RGBA8/HDR
convolution consumers share this layout; invalid backend edges and count/float
allocation overflow return zero before narrowing or copying. The independent
equirectangular sky prefilter retains its existing eight-level budget. Do not
weaken the KTX single/full-chain guard to admit a truncated probe chain.

> Sub-memory for the renderer. See repo-root `AGENTS.md` for global rules.

Launch Vsync policy is object-local and fed by the existing lifecycle bridge;
Renderer never reads Engine state; the lifecycle bridge owns the Vsync launch override. Initial
config resolution precedes backend creation, and the same public Vsync setter
resolves later requests without extra resets when its effective flag is stable.
Resize/MSAA/rebind preserve unrelated reset bits. Renderer-only
jce_renderer_launch_policy_internal.h/.inc.h keep this below the main TU cap.
The bridge's explicit observations read the actual flag getter and distinguish
headless/Noop/fallback unavailability. Repeated identical requests do not publish;
scope success means CPU observation, never backend acknowledgement or GPU idle.

Windows D3D12 uses an observed unmapped-window constraint on effective Vsync.
Each backend attempt queries the public window flags before bgfx initialization;
only a successful flags query containing HIDDEN masks BGFX_RESET_VSYNC. Visible,
MINIMIZED-only, unknown flags, other backends and other platforms keep their
requested policy. The successful attempt publishes its actual reset flags and
private presentation snapshot together. Neither config values nor explicit launch
override presence/value change. Later setters retain the latest request even
without a launch override, then apply the existing launch pin and the constraint.
Explicit platform rebind refreshes that snapshot and restores/clears the Vsync bit
inside its existing single reset, preserving MSAA and unrelated flags. There is
no per-frame query, hidden-to-headless conversion, new visibility API or dependency
patch. Constraint transitions and opt-in launch observations describe CPU flags,
not display synchronization acknowledgement or GPU completion.

Private device launch diagnostics reuse `jce_renderer_launch_policy_internal.h` /
`.inc.h`. `JCE_GFX_FLIP_AFTER_RENDER=0|1` and
`JCE_GFX_MAX_FRAME_LATENCY=0..3` are independent, default-absent experiments.
Only one ASCII digit is valid; missing/empty retains exact bgfx ctor/reset values.
Windowed creation snapshots once before native/allocator/device acquisition and
refuses malformed requests explicitly. Every fallback attempt borrows that same
immutable value, never process environment pointers. Headless and software entry
points report inapplicability without changing their existing device policy.
The flip control selects bgfx CPU flip scheduling, not DXGI flip-model; the bit
persists through Vsync/MSAA/resize/rebind and hidden D3D12 clears only VSYNC.
Latency forwards at init only for pinned bgfx 1.146.9306-550 D3D11/Vulkan.
A forwarded zero asks for backend defaults, not a zero-depth queue.
D3D12's command-queue-to-IDXGIDevice1 path does not apply this field; GL has no
consumer. Other backends retain ctor defaults. Full device recreation is required
for another request; Vulkan's fence/release-ring change may reduce throughput.
Opt-in bounded log/trace packets record requested/forwarded fields, init bits and
CPU admission outcome. `latency_support` reasons: 1 D3D11/Vulkan forwarding,
2 D3D12 unsupported, 3 GL/GLES unsupported, 4 Noop inapplicable, 5 other/unknown.
`phase` distinguishes invalid refusal (0), init failure (1), CPU admission (2),
API refusal (3), headless entry (4) and software entry (5). Only phase 2 is a
successful CPU scope; all backend ACK and GPU completion fields remain unknown.
Default-off emits no new log/trace packets, and no frame loop reads these flags.
Actual complete attempt/prefix and reset CPU fixtures test parsing, latching,
forwarding, refused publication and bit preservation; they do not prove backend
acceptance, latency improvement, pixels or performance on any physical device.

## Identity

- **Layer**: L3 (Renderer) — sits above OS, below middleware.
- **Language**: **C99** (.c). Implementation TUs ending in `.cpp` only when bridging C++ third-party interfaces.
- **Public headers**: `engine/include/jce/renderer/jce_*.h`
- **Public umbrella**: `<jce/api_graphics.h>` (low-level) + `<jce/api_render.h>` (graph/scene/lighting/postfx).
- **Backend**: **bgfx** is the ONLY graphics backend. Do not introduce raw GL/Vulkan/D3D/Metal calls — go through bgfx.
- **ECS**: rendering data lives in flecs components (`jce_renderer_ecs.*`).
- **Internal headers**: under `engine/src/renderer/internal/` and `*_internal.h` — never include from outside this directory.

## File map (current)

| File | Role |
|------|------|
| `jce_renderer.c` / `jce_renderer_caps.c` | bgfx init, frame loop, capability query |
| `jce_renderer_bgfx_callback.c` / `jce_renderer_bgfx_profiler_internal.h` | Backend diagnostics, screenshots/capture and bounded per-thread CPU-profiler callback consumption; callback package availability and GPU completion remain separate |
| `jce_render_graph.c` / `jce_render_queue.c` | Render graph & sorted queue |
| `jce_scene_renderer.c` | Scene-level render path (ties ECS → passes) |
| `jce_gpu_scene.c` | Adaptive GPU Scene / MDI backend: per-model visibility groups, explicit compacted-indirect/fixed-count policy independent of capability, compute culling, bounded growth, frame counters, and CPU fallback |
| `jce_renderer_ecs.c` | flecs components for renderable, light, camera, etc. |
| `jce_camera.c` / `jce_views.c` | Camera + view matrices |
| `jce_mesh.c` / `jce_skinned_mesh.c` / `jce_model.c` / `jce_primitives.c` | Geometry |
| `jce_material.c` / `jce_pbr_material.c` / `jce_shaders.c` / `jce_shader_manager` | Materials + shader management |
| `jce_texture.c` / `jce_image.c` / `jce_stb_image_impl.c` | Texture upload + image decode + **runtime mip streaming** (P3-A.2: per-texture / global bias, residency requests, caps-floor matrix). Cooked format dispatch is fail-closed and includes `R/RG/RGBA 16F/32F`; unsupported backend formats never fall back to RGBA8. |
| `jce_lighting*.c` / `jce_light_cluster.c` / `jce_ibl.c` / `jce_lightmapper.c` | Lighting (forward+ cluster, IBL, baked).  `jce_ibl.c`'s two convolutions are pure functions of a DIRECTION and take their sampler as a parameter, which is how the reflection probe reuses them (`jce_ibl_convolve.h`) instead of carrying a second copy of the same integrals.  Two conventions live here and the difference is PI: the sky path passes PI, the probe path passes 1.0, and `fs_pbr_body.sh` wants the latter -- see the note on `ibl_convolve_irradiance_f32`. |
| `jce_csm.c` / `jce_shadow_filter.c` | Cascaded shadow maps |
| `jce_postfx.c` / `jce_taa.c` / `jce_ssao.c` / `jce_ssr.c` / `jce_volumetric_fog.c` | Post processing |
| `jce_render_pipeline.c` | Render Pipeline Asset (`.rp.json`) — JSON I/O, 4 tier presets, boot autopick, and the sole requested/active/pending authority. Material-look intent is distinct from effective postfx budgets; full apply replaces pending, tier re-resolution preserves it. |
| `jce_reflection_probe_bake.c` | Reflection probe bake worker (P3-E.3) — single-slot dedicated `jce_thread`, atomic status/progress/cancel.  The faces come from `jce_scene_probe_capture.c` (six real 90-degree renders); the CPU procedural sky gradient is now only the FALLBACK for a host with no renderer, and it has an f16 twin so an HDR bake that falls back still produces a container at the stride its header declares. Encode stage now writes a **real KTX1 cubemap** via the `jce_ktx2_writer` bridge + an `.irr.ktx` irradiance sidecar (placeholder source data shared with the specular write for v2). Rewrites caller-supplied `.cube`/`.ktx2` suffixes to `.ktx` in place. Stable public API in `<jce/renderer/jce_reflection_probe_bake.h>`.  **Steps 2 and 3 used to do no work at all** -- the irradiance sidecar was a copy of the specular faces and the container declared one mip, so every roughness read the mirror image and the diffuse term read a mirror of the scene.  They now call the real integrals.  Mip 0 is copied from the source, not integrated: roughness 0 is a mirror, and integrating it cost ~800M samples to compute a copy.  **`desc.hdr` carries RGBA16F through the whole worker** -- both convolutions, the chain allocation (`mipchain_bytes` counts ELEMENTS, so the byte count doubles and the element count does not) and both writer calls.  The one place that must agree and is easy to miss is the SUBMIT-time copy of the caller's faces: it sized itself at 4 bytes per texel while the worker read 8, so an HDR bake read faces 3-5 past the allocation and faces 0-2 looked right. |
| `jce_ktx2_writer.h` / `jce_ktx2_writer.cpp` | Private C/C++ bimg bridge. Owned, checked output uses typed KTX1 RGBA8/RGBA16F headers and host filesystem wrappers; original upstream stays unchanged. The preflighted reader accepts typed or exact legacy zero-type headers, then transposes mip-major wire bytes to side-major backend bytes. Single/full chains and integer limits match the encoder/upload contracts. CPU roundtrip and malformed controls do not certify external tools or GPU use. |
| `jce_particles.c` / `jce_gpu_particles.c` | CPU/GPU particles.  Both submit the SAME `fs_particle` program, so anything per-draw (texture flag, blend, the soft-particle depth fade) must be wired in BOTH or it is a setting that works for some emitters.  Soft particles: `jce_particles_set_soft_fade_distance()` is the process-global both hosts write; `JceGpuParticleSoft` carries the frame's depth into the GPU draw via `_render_soft` (`_render_ex` keeps its ABI and delegates with NULL). |
| `jce_sprite.c` / `jce_sprite_batch.c` / `jce_text.c` / `jce_skybox.c` / `jce_decals.c` | 2D/quad systems. **`jce_text.c` batches a glyph run into ONE draw** via `jce_draw_textured_quads_view` (`jce_renderer_internal.h`); it submitted one draw call PER GLYPH until 2026-09-01. The batch breaks only on an atlas change (`g->dynamic`), a full staging array, or the end of the run — all three preserve submit order, which IS draw order on the SEQUENTIAL UI views. `jce_sprite_batch.c` is unrelated: world-space, 4x4 transform per sprite. The quad geometry is checked headless by `jce_primitives_self_test()` (`jce_primitives_selftest.h`). |
| `jce_debug_draw.c` | Debug line/box/text drawing |
| `jce_occlusion_culler.c` | Occlusion culling |
| `jce_offscreen_target.c` | Offscreen rendertargets (editor scene view) |
| `jce_gltf_loader.c` | glTF mesh/skeleton loader (consumed by resource layer) |
| `jce_gpu_caps.c` | GPU caps query / feature tier |
| `jce_lowlevel.c` | Thin bgfx wrappers for graphics API |
| `jce_material_registry.c` | Dev-mode `.mat.json` mtime watcher (`jce_material_registry.h`); inert outside `--dev` mode. Callback patches MeshRenderer in-place on disk change. |

## Rules (renderer-specific)

GPUScene's survivor snapshot is an optional compacted-model diagnostic. Lazy
RGBA32F image export preserves uint32 counters as two exact 16-bit float words;
the existing typed readback queue owns CPU transfer and completion. The caller
orders the snapshot after cull and its same-frame readback after snapshot.
No snapshot resource or dispatch is created by ordinary rendering. Fixed-count
and unsupported routes return unavailable, never invented survivor totals.

1. **No SDL** — windowing belongs to `os/platform`. Renderer receives a native window handle via bgfx init.
2. **Do not include bgfx headers in public `jce/renderer/*.h`** — wrap handles as opaque `JceXxxHandle` typedef'd ints. bgfx headers only appear in `*.c` / `*_internal.h`.
3. **No game logic** — renderer reads ECS, writes nothing about gameplay. Components live in `middleware/scene`; renderer's own components are pure rendering data.
4. **Baseline budget** — must run on integrated/no-GPU hardware. Prefer forward-clustered over deferred by default; toggle expensive passes (SSR/SSAO/volumetrics/TAA) via render graph flags, default OFF on low-tier devices (check `jce_gpu_caps`).
5. **Shaders** live in `engine/shaders/`, cross-compiled via bgfx's `shaderc`. Per-backend variants generated at build time. Don't hand-write per-API shaders.
    - Sub-folders: `standard/` (normal builds compile for `glsl_120`/`essl_300`/`dx11_sm5_0`; the manually requested `essl1` compatibility subset is not in any shipped auto profile), `pbr/`, `decals/`, `particles/`, `postfx/`, `ssao/`, `ssr/`, `volfog/`, `weather/`, `imgui/` (editor-only).
    - Files: `vs_<name>.sc` + `fs_<name>.sc` (+ optional `cs_<name>.sc`), shared IO via `varying.def.sc`. Output is build-local under `<build>/generated/engine_assets/shaders/`; generated binaries never write into the source tree.
    - Add via `tools/compile_shaders.cmake` shader list; bump `JCE_SHADER_BUNDLE_VERSION` on breaking changes.
    - Uniforms: stable slot order via `uniform vec4 u_<name>;` — reorder breaks runtime binding. Size budget: `standard/` < 5 KB compiled / backend, postfx < 20 KB.
6. **Memory**: textures/buffers via `jce_alloc` + bgfx's own allocator interface routed to mimalloc.
7. **Math**: only `jce_math` (`jce_vec3`, `jce_mat4`, etc.). Never include cglm or roll your own.
8. **Profiling**: each pass `JCE_PROFILE_ZONE("pass_name")` so Tracy GPU/CPU traces line up.
9. **Adding a pass**: register it in `jce_render_graph.c`, allocate resources via the graph, integrate into `jce_scene_renderer.c` per-view path.
10. **File caps**: hot files like `jce_scene_renderer.c` approach the limit — split before adding large features (see `jce_render_queue.c` / `internal/` pattern).
11. **Mip residency intent** (`jce_texture_*_mip_*` API): `effective_top_mip = max(global_bias, per_texture_bias, caps_floor)`, clamped to the 4x4 tail. Low/Mid/High floors remain 1/0/0 and pressure requests remain 0/1/2. Initial creation applies supported quality choices; runtime requests preserve raw handles borrowed by value, dimensions, registry/source ownership and actual resident queries. This includes source-backed textures: retained pixels cannot repair existing material/UI handle copies. Runtime replacement is currently unsupported, logged once per entry and traced once per capture; it is not physical demotion or memory reclamation. A future stable handle/binding contract is required before that claim can return.

## Common tasks

The private readback deferred-destroy path never submits a frame. It transfers
the existing queue to closing/destroy-requested ownership while retaining CPU
destinations still borrowed by bgfx. Central submission collects these queues
only after its actual backend frame serial is published; half-range ambiguity
retains them. Only closing, destroy-requested queues of that renderer are freed;
caller-owned wrappers retired at shutdown survive until their public destroy.
After-backend-stop remains the final fallback. Public destroy's existing bounded
advance loop keeps its queue until its own deferred exit and is not changed into
a second collector owner. Probe admission reuses the existing retiring count
with the public queue capacity as backpressure; public queue creation policy is
unchanged. These ownership observations do not establish GPU idle/presentation.

`jce_renderer_reset_trace_internal.h` owns bounded CPU observations around the
actual void bgfx reset invocation. Resize/rebind/vsync/MSAA preserve their
existing guards, queries, flags and argument sanitation. Fixed scoped details
retain reason, raw requested extent, extent-known, exact forwarded extent/flags
and call-returned; backend-result-known and GPU-completion-known stay false.
Request-known describes observed raw arguments, including known zero resize
values and vsync fallback sentinels; MSAA supplies no extent and remains unknown.
Forwarded dimensions are not acknowledged backend dimensions. SUCCEEDED means
the void call returned, not swapchain recreation, successful presentation or GPU
completion. Unchanged/refused entries produce no reset span, so absence alone
does not establish why a request was skipped. Disabled/suppressed tracing adds
no detail/clock/id work or backend queries; no new logs or reset deduplication
policy is installed. Native event delivery and performance require real devices.

`jce_renderer_bgfx_callback.c` consumes bgfx's dynamic/literal CPU-profiler
callbacks through `jce_renderer_bgfx_profiler_internal.h`. A fixed 64-entry
compiler-TLS stack copies bounded `bgfx.cpu.*` labels and reuses `JceTraceScope`
for parent identity, current-thread IDs and paired completion. Disabled or
suppressed begins occupy zero-scope positions; overflow has separate skipped
depth and explicit `bgfx.cpu.stack_overflow` counters, so its ends cannot pop
admitted parents. Unsupported compiler TLS emits an unavailable counter instead
of sharing a mutable stack between threads. Capture switches/reset require
quiescent producers; the bridge does not discard open spans to fake completeness.
There is no new thread registry or explicit per-callback heap allocation, but
this does not assert that compiler TLS, trace or external profilers never allocate.
Actual callback use is separate from whether a particular upstream bgfx package
enabled its optional profiler. Missing callbacks remain unknown/unavailable,
not measured zero CPU work. These spans do not establish GPU completion.
The private `jce_rcb_cpu_profiler_witness()` separately reports the package's
`JCE_BGFX_CPU_PROFILER_COMPILED` value (missing/invalid is -1 unknown), compiler
TLS bridge support and an atomic count of actual dynamic/literal begins, even
with trace capture off. The count saturates at INT32_MAX as an explicit lower
bound. `jce_rcb_callback_interface()` resets it before each serialized init
attempt and publishes one `init_attempt` log/optional scoped trace record.
Every entered attempt retains a once-only terminal: `init_failed` after bgfx's
false init return has destroyed its Context and joined any owned render thread;
`api_refused` / `allocation_failed` after their actual shutdowns; successful
destruction uses `after_backend_shutdown`. Failure terminals are FAILED even
with zero callbacks and use a narrow finalizer without readback cleanup. An
unexpected open attempt is preserved before reset as FAILED
`superseded_unfinalized`; the factory itself proves neither quiescence nor join.
No failed/unattempted owner later publishes a successful shutdown terminal.
Neither zero observations nor bridge support infers
the package's compiled state. The witness neither registers bgfx-owned threads
nor establishes capture completeness. Software-fallback readback cleanup does
not invent a successful backend attempt; each phase names its actual boundary.
The complete-vtbl CPU fixture uses real JCE threads and trace events; its ordinary
same-owner old-no-op control must compile and fail unchanged assertions.

`jce_text_shape.c` owns a bounded 256-entry, four-way/64-set cache with 64 bytes
of admission cursors. Lookup examines at most four ways; round-robin replacement
skips the preceding borrowed run and advances only after successful fill. Keys
remain bounded; overlong strings use the existing scratch path. Forget/shutdown
release ownership. Actual hit/miss counters establish reuse for a workload.

Custom-program provenance stays inside the existing PBR cache-miss owner.
`material.custom_program.load` covers both path resolutions, reads, program
creation and actual cache admission. The original order (resolve both paths,
then read VS/FS) remains; nested `material.custom_shader.vs` / `.fs` cover source
read/fallback only, with existing host/VFS FS spans parented to those scopes.
Shared FNV-1a64 hashes identify the NUL-delimited original composite request and
each requested/resolved stage key in uint32 halves; they are diagnostic, not
content authentication. Routes 0/1/2/3 mean none/HOST_API/explicit VFS_API/engine
PAK. HOST_API may itself dispatch active VFS; nested FS dispatch owns that fact.
Blob reasons follow the private enum: read success, empty reader data, forbidden
fallback, absent engine, absent/empty entry, allocation failure or decompression
failure. Stage outcomes not attempted use -1; conditional byte halves exist only
where observed. A successful nonzero decompress preserves the existing admission
policy; size-match is separately observed. Program invalidity after an actual
create remains an opaque/unknown create failure. Failed results remain cached;
cache hits add no reload, log or trace. The cache owns valid programs, with
existing warnings, frees and shutdown unchanged. CPU provenance is neither a
successful GPU link/execute nor GPU completion or graphical correctness proof.

`jce_model_texture_pool.c/h` owns model-local image/mode staging and unique GPU
texture handles. Material slots borrow handles; they never destroy shared
textures. CPU cancellation frees staging once; upload consumes it once and
transfers the pool to the model for render-thread destruction. Color space and
sampler mode are part of resource identity. GPU byte summaries count each
owned texture once and remain RGBA8-equivalent estimates, not allocator queries.

HiZ reduction uses actual source dimensions at every level. Destination texels
cover floor/ceil normalized source intervals, including overlap at odd-sized
boundaries and a one-texel axis. Losing far-depth samples can falsely occlude
visible geometry; undersized maxima are not conservative. Shader bundles and
their CPU uniform producers must be regenerated together when this changes.

Text shaping preserves HarfBuzz's font-local glyph index independently of the
source Unicode character. The atlas reuses nominal ASCII/Unicode entries only
when their FreeType index matches; substituted/ligature glyphs use high-bit
tagged keys in the existing dynamic atlas. Rasterize those with FT_Load_Glyph,
not FT_Load_Char. HarfBuzz Y offsets point up; UI coordinates point down.
Keep 26.6 offsets/advances fractional in both measurement and drawing.

| Task | Steps |
|------|-------|
| Add a post-fx | New `jce_<fx>.c/h` under renderer; add public header to `<jce/renderer/>`; hook into `jce_postfx.c` chain; expose enable flag in render graph |
| Add a material feature | Extend `JcePbrMaterial` → add uniform in shader → update `jce_shader_manager` permutations → write inspector drawer (editor side) |
| Add a renderable component | Define struct in `jce_renderer_ecs.h`, register in `jce_renderer_ecs.c`, consume in `jce_scene_renderer.c` |
| Lower-tier fallback | Query `jce_gpu_caps`, branch in render graph build, never `#ifdef _WIN32` |

## Don't

- Don't expose bgfx types in `<jce/...>` public headers.
- Don't allocate per-frame on the hot path — use ring/pool allocators from `os/core`.
- Don't add a 5th shadow algorithm without removing/refactoring an existing one.
- Don't `#include <SDL3/...>` or `<windows.h>` here.

## P3-E.5 — Light Cookies + IES Profiles

Spot lights ship end-to-end (cookie texture + IES LM-63 photometric LUT) via `JceSpotLight` fields (`cookie_texture` / `ies_lut_texture` / `cookie_strength` / `cookie_path` / `ies_path`) and matching `JceSpotLightDesc` fields. Sampler slots **13 = `s_cookie`** (a 2D **ARRAY**) and **14 = `s_iesLut`**. New public header: `<jce/renderer/jce_ies_profile.h>` (`jce_ies_parse`, `jce_ies_bake_lut_from_file`, `jce_ies_bake_lut_from_memory`).

**EVERY light projects its OWN cookie** (2026-09-18). The v1 note that used to be here — "binds the FIRST spot light that has a cookie / IES" — is gone, and so is `u_cookieParams` (now `u_iesParams`: `.x` has_ies, `.y` ies_spot_index) and `u_cookieSpotVP`. What a light carries now:

- its **atlas layer** in `u_spotLights[i*4+3].y` / `u_dirLights[i*2+1].w`, registered by `jce_light_env_register_cookie`;
- its **strength** in `u_spotLights[i*4+3].z`;
- its **frustum**, DERIVED in `fs_pbr_main.sh` from its own position, direction and outer cone — reproducing `jce_m4_perspective(2*acos(outerCos), 1, ..) * jce_m4_look_at()` term for term, so per-light projection costs no uniform and the shared mat4 upload is gone.

The layers are filled by one `bgfx_blit` per (slot, texture) on `JCE_VIEW_MORPH_DEFORM` — a view id BELOW every scene base, because bgfx orders blits by view id exactly as it orders draws, and a blit on a higher id lands after the colour pass that samples it.

The modern stock sampler is a 2D array. Without array/layer support, or on
explicit compatibility GL/ES, the owner creates and binds no cookie resource.
Array-capable paths keep the atlas; disabled/unavailable atlases use a real
1x1 two-layer initialized white array. bgfx treats one layer as a 2D texture,
which cannot satisfy this sampler. `JCE_COOKIE_NO_ARRAY=1` forces the white-array
path when arrays are supported. Compatibility shader guards remove the array
declaration and both cookie blocks; ordinary shader formulas remain unchanged.
Actual driver compile/link and rendered output still require separate acceptance.

**Zero is a valid bgfx handle.** A `memset` desc therefore claims a cookie it never asked for, and the atlas used to treat handle 0 as its "empty slot" sentinel — both measured, both fixed. Build light descs with `jce_dir_light_desc_default()` / `jce_spot_light_desc_default()`; the atlas sentinels are `JCE_COOKIE_SLOT_EMPTY` / `_RESERVED`.

**Rebuild shaders** after touching `fs_pbr.sc`: run shaderc against `engine/shaders/pbr/fs_pbr.sc` to refresh `_cooked/*/shaders/*/fs_pbr.bin`.

## P3-E.5b — Directional cookie + multi-cookie atlas

Directional light cookies now project. CPU builds `u_cookieDirVP` as a **world-aligned ortho box** (±50 m half-extent, 0.1–200 m near/far) centred on the camera position and aimed along the chosen directional light's forward — v1 chooses stability over CSM-tight fit (CSM cascade-0 reuse is the v2 path). `fs_pbr.sc` adds a `u_cookieDirParams` vec4 + `u_cookieDirVP` mat4 and a directional-loop branch that samples `s_cookie` with clamp-to-zero outside `[0,1]`. Sampler 13 is no longer shared — see the section above. The directional VP is still ONE matrix, and legitimately so: there is at most one cookie-bearing directional light (the sun), and `u_cookieDirParams.x` gates the whole block. Its atlas layer comes from the light's own pack (`u_dirLights[i*2+1].w`); the dead array code read `u_cookieDirParams.w`, which is packed as literal 0, so it would have sampled the white layer forever.

The multi-cookie atlas uses a 16-slot LRU table, with slot 0 reserved white.
`jce_light_env_register_cookie()` returns each admitted texture's slice index;
modern shaders consume the per-light index through the shared array sampler.
The actual typed texture owner below gates array/layer/blit support. A source
atlas declaration is not proof of a successful upload, driver link or pixels;
rebuild affected stock shader families through the supported cook entry point.

## Shader cross-backend portability (`engine/shaders/*.sc`)

The mode2 water overlay retains its final alpha discard after procedural
alpha evaluation. Small positive or HDR masks can survive that threshold;
the sampled mask alone is not the final alpha. Shader branch movement needs
actual backend pixel controls and paired whole-frame measurements before
adoption; fewer instructions or an exact screenshot alone proves no gain.

Every normally shipped `.sc` compiles to HLSL s_5_0, SPIR-V, GLSL 330, ESSL 300,
Metal). Danger class: code that compiles everywhere but *means something different
per profile* — ships green, renders wrong on the backends nobody checked.
`tools/shader_lint.py` enforces the mechanical rules at build time (a finding fails
the build). (Relocated here from the former `engine/shaders/AGENTS.md`, which the
AGENTS.md placement lint excludes — rules for excluded dirs live in the parent.)

**Iron rules (lint-enforced):**
1. **Never use a raw multi-argument matrix constructor.** `mat3(a,b,c)` packs
   args as rows on HLSL, columns on GLSL — same source, transposed matrix per
   backend. Use `mtxFromCols(a,b,c)` / `mtxFromRows(a,b,c)` from `bgfx_shader.sh`.
   Single-arg forms (`mat3(someMat4)` truncation, `mat3(1.0)`) are portable/allowed.
2. **Never combine a matrix with `*`.** GLSL `*` is the matrix product; HLSL `*`
   is component-wise. Always `mul(a,b)`. Convention: `mtxFromCols` matrices
   multiply column vectors from the right — `mul(M, v)`. Escape hatch for a
   verified exception: `// shader-lint: allow` on the same line (justify it).

**Iron rules (review-enforced):**
3. **GLSL 330 / ESSL 300 are the shipped floors.** GL 2.1 and GLES 2.0 are not
   members of the runtime auto chain. Keep loops and uniform-array indexing
   conservative because the stable tier still targets old integrated drivers;
   compute shaders use GLSL 430 / ESSL 310 and are capability-gated.
4. **Y-flip / depth-range differences go through helpers + documented blocks.**
   NDC depth: `toShadowDepth()`. GL render-target sampling is bottom-left origin —
   every `#if BGFX_SHADER_LANGUAGE_GLSL` flip must comment which convention
   mismatch it fixes (see the atlas tile-row flip in `sampleLocalShadow`). Never
   copy a flip without re-deriving it for the new texture's writer.
5. **A lighting/shadow/normal change is not done until it passed a parity run:**
   `python tools/render_parity.py --backends d3d11,opengl` (boots the same binary
   per backend via `JCE_BACKEND`, captures the same frame, pixel-compares). D3D11
   vs OpenGL covers the HLSL-vs-GLSL split where every divergence has lived; add
   `vulkan` for SPIR-V when relevant. Use a static scene.

**Case study:** 2026-06-10 `fs_pbr.sc` built TBN as `mat3(T,B,N)` + `mul(n, TBN)`.
Correct on D3D for years; on GL/WASM the constructor packed columns not rows → the
shader applied the transposed (inverse) basis → ground normals sideways, light
pools cut in half at the light axis. One-line fix (`mtxFromCols` + `mul(M,v)`);
hours to hunt. Rules 1 + 5 each would have caught it pre-merge.

## render graph：已从公共 API 降级为内部头（2026-08-31）

`jce_rg_create()` 全树只有两个调用点，都在 `jce_rg_self_test()` 里，而
`jce_rg_self_test()` **一个调用者都没有**。也就是说 render graph 从未被实例化过。

自检第一次运行时两条用例全红，**同一个根因**：它用 `jce_rg_add_pass(rg, "shadow", NULL, NULL)`
建图，而 `add_pass` 第一行就拒绝 `fn == NULL` ⇒ 三次全返回 invalid ⇒ 空图 ⇒
`exec_count(0) == active_count(0)` ⇒ **compile 报告成功**。自检还从不检查 `add_pass` 的返回值，
所以这个失败是静默的。

**但「算法本身是对的」这句要限定**：排序只在 **read-after-write** 上成立。
`compile()` 建边时只走每个 pass 的 `reads`，**两个都写同一资源的 pass 之间没有边** ⇒
clear-then-draw 到同一目标的相对顺序未定义。

**这套 API 今天用不了**（全部实测，细节见 `jce_render_graph.h` 顶部）：
没有任何调用能把 `JceRGResource` 变成句柄；`execute` 从不 `bgfx_set_view_frame_buffer`；
它分配的 transient 纹理**无人可达**；`base_view_id` 曾硬编码为 100，同时撞进
Scene View 的点光立方体带（103..119）、Game View 的跨度、以及被 re-base 到 100 的 PostFX 链；
而它**从不调用 `jce_view_bands_claim`**，所以引擎自己的 view 归属守卫看不见它。
现在 `base_view_id` 没有默认值，`compile()` 会一直失败直到调用方
`jce_rg_set_base_view_id()` 自己选一段。

**处置：头已从 `engine/include/jce/renderer/` 移到 `engine/src/renderer/`，
从 `api_render.h` 摘除，15 处 `JCE_API` 去掉。** 公共面因此 299/301 → 298/300 个头、
3562 → 3547 个 `JCE_API` 符号，SDK 不再安装这个头。

**降级而不是删除**，是因为降级已经消除了真正的危害（向用户承诺一套用不了的能力），
同时保留那段正确的拓扑排序给后来人，而且**可逆**——重新公开一个头是一行，
恢复删掉的代码要考古。今天没有能工作的消费者，所以不存在被破坏的东西。

⚠️ **仍留给这个子系统的 owner**：`add_pass` 拒绝 `fn == NULL`，而执行路径（`:424`）
容忍它，两者不可能都是本意。要真正完成它，缺的是**资源→句柄访问器**与
**execute 里建 framebuffer**；两者都需要 bgfx 上下文才能测，而单测套件没有。


## 2026-09-01：OpenGL 的版本地板在哪（不在这个目录里）

`glsl` 的 shaderc profile 是 **120，这是对的，不要改**。它不是 GL 的版本地板：

* bgfx 在 `ShaderGL::create` 里**加载时重写 `#version`**——按源码用到的标识符在
  120/130 之间选，compute 的 430 原样保留，编译期 `MIN_VERSION >= 31` 时写 140。
  shaderc 写的那行会被丢掉。
* 这套 shaderc 把 `glsl <= 400` 交给 glsl-optimizer（只支持到 1.50），
  所以 **330 和 400 是它唯一取不到的两档**。2026-08-31 把它设成 330 的改动
  让整棵树的 OpenGL 构建挂了一天。
* `#version 140` 分支带着一串 `#define texture2DLod textureLod` 兼容宏，
  就是用来把 120 风格的源码向上适配的——bgfx 自己的 `shader.mk` 默认也是
  `GLSL_LEVEL=120`。

真正的地板是 bgfx 的编译期 `BGFX_CONFIG_RENDERER_OPENGL_MIN_VERSION`，
由 conan conf `user.jce:bgfx_graphics_tier`（stable / modern / current）选，
经 `conan/hooks/hook_bgfx_wasm_fix.py` 落到 bgfx 的 cache 变量。
三档 = **GL 3.1**/ES 3.0、4.3/3.1、4.6/3.2。**不设它不等于「自动」，等于回落到 1（GL 2.1）。**

stable 是 3.1 而不是更常见的 3.3，因为渲染器**不再跑在地板上**——它会向上爬到驱动肯给的
最高 core 版本。地板唯一剩下的职责是「还服务多老的机器」，而 3.1 是这套配置的真实下界
（bgfx 写 `#version 140` = GLSL 1.40 = GL 3.1）。实测 33→31 代价为零：三个硬 `#if` 块
（`>=40`/`>=41`/`>=31`）两档完全相同，12 处 `>=33` 全是扩展表种子、运行时 `GL_EXTENSIONS`
扫描会恢复，唯一真损失 `BGFX_CAPS_PRIMITIVE_ID`（`>=32` 时无条件给出）全树无人消费。

`tools/lint/check_gfx_api_tiers.py` 守这件事：它校验 hook 的表、
`jce_renderer_caps.c` 的预处理器断言、以及同一文件里运行时报出的 `gl[]/gles[]` 表
三者一致，并同时检查**两份** shaderc profile 表
（本目录的 `tools/compile_shaders.cmake` 与编辑器的 `jce_shadergraph_shaderc.cpp`）。

Vulkan 没有档：bgfx 运行时调 `vkEnumerateInstanceVersion` 取设备最高版本。

**选更高的档不会让它更快。** 梯子上线后，渲染器已经会爬到驱动肯给的最高 core 版本，
所以 stable 构建在有能力的卡上本来就跑 4.6。抬高档位实测只改动：
`>=40` 两个纹理格式宏、`>=41` 两个带默认值(16/128)的 limit 查询、
`>=43` 十二处其中十一处是运行时可恢复的扩展表种子（第十二处有 `KHR_debug` 兜底）——
**同时把最低要求抬高，把低于新地板的 GPU 全部拒之门外**。
档位是**支持范围**的决定，不是性能的决定。

## 2026-09-06 — 循环归组件，不归图集：`jce_sprite_player_set_loop()`

在此之前循环是**图集**的属性：`JceSpriteAnim.loop` 来自 Aseprite 的 frameTag，
`jce_sprite_player_update` 直接读它，而 `jce_sprite.h` 没有任何 player 侧的 setter
⇒ `JceSpriteAnimatorComponent.loop`（授权、序列化、画成复选框、**parser 与编辑器
Add Component 两侧默认都是 true**）**没有门可以走**。

三态：`JCE_SPRITE_LOOP_FROM_ATLAS(-1)` / `0` 一次 / `1` 循环。
`sprite_loops()` 是**唯一**把两个来源合起来的地方，不要在别处再判一次。

**打开循环会释放一个已经 finished 的 player。** 没有这一步，勾上 Loop 在动画切换前
什么都不会发生——而「什么都没发生」正是一个未接线控件的样子。

它为什么一直没被发现（两层）：加载器给它读到的**每一个** tag 以及整表回落 anim 都设
`loop = true`，所以基本上什么都在循环——**勾上没变化，取消勾选也没变化**；而
`check_component_field_consumed` 当时按整个文件匹配指针名，`jce_sr_anim.c` 里
`JceSkeletalAnimatorComponent *sa` 的 `sa->loop` 顶了这个组件的包。

## 2026-09-21 — `jce_renderer_request_screenshot_fbo()` 零调用点，以及**它不是黑窗口 P0**

全树（`git ls-files`）只出现四次：声明、定义、`contracts/abi-snapshot.txt` 里的一行、
以及 `jce_editor_scene_render.cpp:36` 的一句 **`#include` 注释**——
`/* jce_renderer_request_screenshot_fbo */`，解释这个头为什么被包含。

**一句解释「为什么 include 这个头」的注释，是「有人本来打算调用它」的最强证据，
同时它也不是一个调用者。** 与「四个被授权却从没被读过的 WheelCollider」同形。

它的 docstring 说了机制：D3D flip-model 交换链上，**Present 之后 backbuffer 拷不出来，
截图全黑**；截离屏 FBO 可靠。

**但不要把它写成黑窗口 P0 的原因——实测不成立。** 本机默认后端上 backbuffer 路径
**工作正常**：`app.runs_agree` 今天取到的是有场景内容的真实帧，外加 53089 像素的
profiler 条带，不是黑图。条件性机制解释不了无条件症状。
⇒ 现状只能说：**一个已知坏配置的、有文档的修法，躺在公共 ABI 里从没被调用过。**
这本身值得修，不需要它同时是 P0。

**我试过的接法是错的，写在这里省下一次。** 我加了
`jce_renderer_set_capture_source_fbo(uint16_t)`，让 `JCE_CAPTURE_FRAME` 走 FBO。
然后读到编辑器**已经有**一条正确的无 UI 截图路径：

```
jce_editor_game_render_screenshot()
  -> jce_editor_viewport_screenshot_submit(bridge, view_base, postfx_tex, w, h, path)
     source = postfx_tex，若无则 bridge 的 color texture
     yflip  随 source 而变（postfx RT 每个后端都是 bottom-up）
     -> jce_renderer_readback_capture_submit(source_TEXTURE, ...)
```

**被显示的那张图是一个 texture，且 y 翻转取决于它是哪一个**——
不是一个 FBO 索引能表达的。一个 `uint16_t fbo_idx` 的 setter 会去截
**bridge 的 color attachment**，而开着 postfx 时那不是屏幕上那张图。
「截到的图非空」和「截到的是被测物」又一次不是同一件事。

⇒ 要接的话，正确的缝是**宿主钩子**（宿主自己拿这一枪），不是 FBO 索引。
还要解决：readback 是异步的（`jce_renderer_readback_capture_poll` 每帧轮询），
而 `JCE_CAPTURE_FRAME` 之后进程会退出——`jce_accept.py` 用 `frames - 2`
就是为这个留的余量。

**另外：编辑器截图本身不是好证据源**，与上面是两件事。
profiler 条带的大小和位置来自 `~/.jce/imgui.ini`，一个**按用户**的文件，
determinism recipe 不钉它 ⟹ 同一台机器上两次一致，说明不了两台机器上一致。
`app.run` 现在会在结果里**明说**这件事（`evidence` / `evidence_reason`），
而不只是把 `executable_source` 记进一个没人打开的 manifest。

### 补（2026-09-21）：缝已经接上了，形状是**宿主钩子**

上面那段写「要接的话，正确的缝是宿主钩子」。已经接了：

```
jce_renderer_set_auto_capture_hook(fn, user)   公共，jce_renderer.h
jce_rcb_host_took_capture(path)                内部，本目录的头
```

`JCE_CAPTURE_FRAME` 命中时**先问宿主**，返回 false 再回落到 backbuffer。
编辑器在 `jce_editor_game_render_init` 装上，钩子转调
`jce_editor_game_render_screenshot`——也就是那条**已经正确**的无 UI 路径。

**三件容易漏的，都是「不会响的失败」：**

1. **回落不是错误路径，是正常路径。** Game View 面板关着就没有离屏目标，
   钩子返回 false，截图仍然要发生（从 backbuffer）。
   把「declined」当成「taken」会**一张图都不产出**，而且没有任何报错。
2. **shutdown 必须**先清钩子**，再销毁它读的东西**。
   `jce_editor_game_render_shutdown` 里清钩子那一行在所有 destroy 之前——
   一个活过自己状态的钩子不会大声失败，它会去读那个地址上**现在**是什么。
3. **用具名函数 + 显式 `JCE_CALL`，不要用 lambda。** `JceAutoCaptureFn` 带
   `JCE_CALL`（Windows 上是 `__cdecl`）；无捕获 lambda 转出来的函数指针用的是
   编译器**默认**约定，x64 上碰巧相同，**x86 上不同，而不同的表现是栈被写坏、
   不是编译错误**。

判据：`tests/application/test_jce_auto_capture_hook.c`，钉的是**缝**不是生产者
（生产者要 GPU 和真帧）：未装 / 装上 / **拒绝** / 清除，以及 path 和 user 指针是否完整到达。
**拒绝那一条是最重要的**——它是唯一一条「静默什么都不产出」的路径。

**续（同日，实测）：这个钩子在自动化路径上*从来不会触发*，而且它第一版还制造了一次回归。**

两趟独立 `app.run`（frames=90，第 60 帧截图）逐像素比对：

```
2560x1528  差异 34977 / 3911680 px
其中 rows 1080..1526  34977
band 之外              0        ← 和 2026-09-20 那次 53089 的结构完全一致
```

**时钟还在。** 引擎日志给出了机制本身，不靠推断：

```
game_render: [capture] Game View is 16x16 -- below 64 px, declining so the
capture falls back to the backbuffer rather than photographing a collapsed panel
```

> **更正（同日晚些，我自己写错过一次）：这里原本写的是「没有保存的布局」。**
> **布局在，而且是对的。** 查 `~/.jce/imgui.ini`（5267 bytes）：
>
> ```
> [Window][game_view]        [Window][scene_view]
> Size=1380,970              Size=1380,970
> Collapsed=0                Collapsed=0
> DockId=0x00000005,1        DockId=0x00000005,0
> ```
>
> 两个面板**共用同一个 dock 节点 `0x00000005`**，`scene_view` 是 tab 0，
> `game_view` 是 tab 1。
>
> **我据此提了第二个假说——「Game View 是背后那个没被选中的标签页」——
> 并且做了实验，实验*否定*了它。** 预先写下的判据是：把两个 tab 序号对调
> ⟹ 截图应变成 1380x970 且不再出现 decline 行。实测（备份 ini、对调、跑两趟、
> 从快照逐字节还原）：
>
> ```
> pass 1: size=(2560,1528)  bytes=1189109  decline_line=True
> pass 2: size=(2560,1528)  bytes=1189677  decline_line=True
> ```
>
> 尺寸没变、decline 行照旧、面板仍报 16x16。
>
> **但这个实验是无效的，而我是在把结论写进树之前发现的。**
> ImGui 决定哪个标签页在前，用的**不是**每个窗口那行 `DockId=<node>,<序号>`，
> 而是 dock 节点自己的 `Selected=`：
>
> ```
> [Docking][Data]
>   DockNode  ID=0x00000005 Parent=0x00000004 SizeRef=1380,1074 Selected=0x8D781F63
> ```
>
> **我对调的是序号，没动 `Selected=` ⟹ 这一趟根本没改变选中项。**
> 所以「Game View 是背后那个标签页」这个假说**既没被证实也没被否定**。
>
> 想真正测它，需要把该节点的 `Selected=` 设成 game_view 的 ImGuiID，
> 而**那个 ID 算不出来**：`ImHashStr` 不是普通 CRC32——
> `zlib.crc32("scene_view") = 0xE55BC08A`，而文件里写的是 `0x8D781F63`。
> （我是拿一个**已知答案**去校验算法才发现的；直接拿算出来的值去写，
> 就是第五个坏仪器。）
>
> **第二次实验（同日）：假说被*证实*了。**
> 不去算那个 ID，改成**把 `Selected=` 整行删掉**、并把 game_view 放到序号 0，
> 让 ImGui 的回退去选第一个标签页——**不需要任何 ID**。
>
> ```
> size=(1380,898)   decline_line=False     ← 钩子触发了
> ```
>
> **然后是这一行一直想要的那个测量**：同一个 input digest 跑两趟、钩子都触发，
>
> ```
> pass 1  762,774 bytes     pass 2  762,774 bytes
> 1380x898 (1,239,240 px)   差异: 0
> ```
>
> ⇒ **编辑器来源的自动截图，在 Game View 处于前台时是逐字节可复现的。**
> **阻塞点是布局，不是这条缝。** 钩子做的正是它该做的；
> 只是那条路径上从来没让它跑起来过。
>
> **要关掉这一行需要什么，现在是确定的**：确定性配方得钉住**哪个面板在前台**。
> `tools/jce_determinism.py` 的 `DETERMINISM` 只有四个键、没有一个碰布局。
> 而且这**不是**一个可以用容差框住的连续量——
> **哪个标签在前，决定了你拍的是哪个程序的输出。**

> **附带，且它救了一次**：写第二个实验脚本时我写了
> `io.open(INI,"w").write(edit(io.open(INI).read()))`——
> Python **先**求值 `open(...,"w")`（**截断文件**），**再**求值那个读它的参数
> ⟹ `edit()` 拿到的是它自己刚造出来的空字符串，**操作者的布局文件被清空了**。
> 是 `finally` 里的快照还原把它救回来的，并且还原后做了哈希校验。
> **快照+校验从这一刻起不再是形式**。
> 剩下的候选是**面板可见性**——`jce_panel_game_view.cpp:1140` 有
> `if (!*vis) return;`，而可见性存在 `~/.jce/editor-session.json` 的
> `panels_visible_mask` 位掩码里，**不在 imgui.ini 里**——
> 以及首帧布局未就绪时 `g.render_width` 被写入一个退化值后**再也没人重置它**。
> **下一个人请从这两条往下查，不要再去查前两条。**
>
> **错的那句有后果**：「没有保存布局」会让下一个人去找一个不存在的缺失文件，
> 而真正的阻塞点是**一个存在且正确的布局里的标签页 z 序**。两件要修的事完全不同。

⇒ **机制正确、且在这条路径上不可达**——`jce_editor_main.cpp:303` 无条件装上它，
而它永远没机会返回 true。**五条缝测试钉的是「被调用时会怎样」，没有一条钉「它被调用了」。**

**而且这条路径的不确定性比「尺寸」更深。** `tools/jce_determinism.py` 的
`DETERMINISM` 只有四个键——`JCE_FRAME_DT_FIXED` / `JCE_STREAM_SYNC` /
`JCE_TAA_JITTER_PHASE` / `JCE_WINDOW_HIDDEN`——**没有一个跟编辑器布局有关**。
所以编辑器来源的 `app.run` 依赖操作者的 `~/.jce/imgui.ini`，
**不只是面板尺寸，还包括哪个标签页在前面**。
前者是个可以用容差框住的连续量，**后者直接决定你拍的是哪个程序的输出**：
两台机器、同一个 input digest、不同的被摄物。

**第一版更糟：它只判了 `render_width == 0`，而 16 不是 0。**
于是它**接受**了那个退化面板，`app.run` 的截图从
**2560x1528 / 970 KB**（钩子上线前那一趟）变成 **16x16 / 328 bytes**。
一张「底部有时钟」的完整帧被换成了一张**什么都没有**的图——
时钟让帧不可复现，16x16 让帧**不再是证据**。

**没有任何东西拦住它**：那一趟被报成成功，而 `app.run` 自己的内容守卫返回
`has_content: null` + `"guard could not run: sampled no pixels -- the crop
window is outside the image (16x16)"`。

**而这个标签本身是错的——守卫其实跑了，而且*拒绝*了这一帧。**
`assert_has_content` 抛的是 `RuntimeError`（`jce_determinism.py:293`），
而 `observe.py:355` 的「被拒绝」分支 catch 的是 `SystemExit`、
「跑不起来」分支才 catch `Exception`。`RuntimeError` 不是 `SystemExit`
⟹ **每一次拒绝都落进「跑不起来」那一支**：

- **`has_content: false` 根本不可达**；
- 一句确定的「这帧是空的」被报成一句含糊的「我没能看」。

**这正是本节要讲的那个错误的镜像**：一个**其余取值都表示「我看了」的字段，
被用来承载「我没看成」**。而它最终怎么报都无所谓——
那个判决被 `guards.append(...)` 收进一个**全文件再无第二处引用**的列表。

⇒ 两条可复用的判据：

1. **退化值和缺失值不是一回事。** `== 0` 拦不住 `16`。凡是「尺寸/数量/长度」的
   守卫，问的应该是「这个值可能是真的吗」，不是「它是不是零」。
2. **阈值要带着它的两个实测数字一起写进代码**（这里是 64：比任何真实面板小一个数量级、
   比实测到的退化值大一个数量级）。没有证据的阈值活不过下一个人。

### 第二个洞与它的两个对照（`f676f70e`）

上面那条 64px 守卫只拦「**从没**画到可用尺寸」。另一半它拦不住：
Game View **曾经**在前台、写下 `1380x898`，之后切到背景标签页 ⟹
`jce_panel_game_view.cpp:1145` 的 `ImGui::Begin` 返回 false ⟹ 面板体不执行 ⟹
`jce_editor_game_render_frame()` 不再被调用 ⟹ **没有任何地方清掉那个尺寸**
（实测：`render_width` 只有一个赋值点、声明处初始化为 0、`g` 从不整体重置）。

于是那个值**又大又合理，描述的却是没人在画的一帧**。
⇒ **按尺寸分不开「陈旧但合理」和「活的」；只有它属于哪一帧能分开。**
修法：写尺寸时盖上 `jce_renderer_get_frame_index()`，太旧就拒绝
（`JCE_EDITOR_GAME_VIEW_EXTENT_MAX_AGE`）。

**两个对照都做了，而且必须来自同一个二进制的同一次运行：**

```
阳性（Game View 在前台）      size=(1380,898)   decline_line=False
阴性（把时间戳变异成永久陈旧）  size=(2560,1528)  decline_line=True
                              日志：age 60 > 1, written=1
```

`written=1` 是让它成为**对照**而不是巧合的那一项：尺寸检查和 `extent_written`
都通过了，**新鲜度比较是唯一还能拒绝的东西**——它拒绝了，并回落到 backbuffer。

### 这里踩到的三个坑，都不在被测代码里

1. **常数把我自己的解释否掉了。** 我带行号断言容差不可能是 0
   （`bgfx_frame()` 在 `jce_renderer.c:1878` 推进索引、捕获点在 `:1954`）。
   **强制设成 0 重建 ⟹ 照样触发**——`:1878` 是 `jce_renderer_begin_frame` 的尾巴，
   **编辑器不走的 2D 回退路径**；编辑器走 `begin_frame_3d`，唯一推进在 `:2037`、
   **在捕获之后**。age 实测 0，最终取 1（多出的一帧覆盖 2D 路径）。
   ⇒ **一个「足够大」的常数和一个「正确」的常数分不开；
   唯一测法是把它设成按自己的推理必然失败的值，然后看它没失败。**
2. **阴性对照第一次跑出来是「GUARD DEAD」，而守卫是好的。**
   检测器匹配的是 `"[capture] Game View is"`（为**尺寸**那条消息写的），
   而新消息是 `"[capture] Game View extent is from frame"`——**不是子串**。
   于是一次真实的拒绝被读成「没有拒绝」。
   **我此前已经承诺「GUARD DEAD 就把修法撤回」**，一个只打印结论的「整洁」脚本
   会让我撤掉一个能工作的守卫、并写一句说它不工作的提交信息。
   修法是**匹配 tag `"[capture]"` 而不是某一条消息的措辞**。
   ⇒ **原始日志与结论并排保留，是真相唯一可见的原因。**
3. **变异脚本只还原了源码，没有重建。** `jce_editor.exe` 会留着变异版本——
   **一个 `git status` 看不见、下一次测量会静默继承的二进制**。
   ⇒ **源码快照保护的是树，只有重建才保护树产出的东西。**

The CPU light cluster caches world-to-view once per camera update and converts
right-handed view Z to the positive depth used by PBR shaders. Projected clipped
sphere-AABB corners conservatively cover perspective, orthographic and shifted
projection matrices; near-plane intersections and tangent bounds remain visible.
Native tests use real right-handed camera/projection inputs, not positive-Z
identity-camera substitutes. This broad-phase may overassign, never shade beyond
the light influence tested by the shared fragment shader.

Forward+ keeps logical packed/upload row counts separate from the capacity GPU
texture height. Per-submit normalized sampling receives the physical height;
region offsets and actual light/grid/index bounds retain their logical meanings.

Forward+ update retires the previous GPU upload validity before validating the
new frame; failed updates must not bind old cluster uniforms or textures. The
last successfully packed CPU accessor remains diagnostic history. Cluster camera
reset first invalidates the prior camera and accepts only finite ordered positive
clip planes. NaN slice input returns zero; infinite depths clamp to end slices.

Camera authored poses retain their exact normalized forward/up basis, including
poles and roll. Euler yaw/pitch remain controller state, not the pose authority.
Only interactive FPS rotation/look-at paths apply the existing 89-degree clamp;
view, orientation getters and movement consume the same resolved direction.
Invalid pose setters are atomic. Public camera tests cover pole/near-pole/roll,
view-axis projection and the unchanged interactive clamp.

Nonzero FPS rotation and look-at switch to the stable world-Y-up controller
basis, so a former authored pole/roll cannot leave forward parallel to up.
Zero rotation preserves exact pose. Validate both normalized direction vectors
before publishing a pose; overflow during normalization rejects atomically.

Custom material programs in JcePbrMaterial apply to rigid model primitives as well as standalone meshes. The skinned-only draw override keeps its separate bone-palette contract. Raw-memory texture decoding and VFS/PAK decoding share one owned-byte decoder.

Exact poles preserve the prior interactive yaw because their horizontal heading
is undefined; signed-zero atan2 must not invent a 180-degree turn. Near-pole
nonzero horizontal components still determine their actual heading.

Scene GPU emitters consume the asset's full world-space box widths and velocity
stretch via private particle entry points. Public point emission and soft
billboard draw defaults retain their established semantics. Scene admission
requires compute, instancing, and D3D11, plus explicit scene or QA opt-in.

GPU particles allocate from a GPU-compacted list of actual dead slots. Reset the
uint counters, append one unique index per dead pool slot, then let each emit
invocation consume one list entry; non-atomic lifetime probing is forbidden.
Resource/program failure must return an unusable pool and enable real CPU
fallback. Optional private diagnostics export a post-allocation 24-bit RGB alive
count from scanned free slots plus actual accepted GPU writes, with alpha
validity, through the existing typed RGBA8 readback/debug-target contract.
Diagnostic dispatch is opt-in and cannot certify position/visual correctness.

## Worker texture preparation

Raw `JceTextureCpu` results own a complete original-resolution RGBA8 mip chain
prepared with the private pure `jce_texture_mips.h` helper. The image decoder's
allocation is released after mip-0 is copied; mip-0 remains an alias into the
JCE-owned chain with its original dimensions for the private LUT strip accessor.
Cancel/free releases that chain once. Worker preparation does not query renderer
policy, capabilities or call bgfx.

Upload chooses the existing hardware-tier mip suffix on the render thread and
copies only that suffix into bgfx-owned memory. Cooked payloads retain their
authored format and mip chain. RGBA byte-space box averaging, alpha averaging,
NPOT floor-halving and one-pixel tap clamps are preserved. Remaining bgfx copy,
texture creation and optional streaming source-cache copies are real render-thread
costs; moving mip arithmetic does not remove those costs. Chain allocation is
checked and bounded; allocation failure fails decoding rather than retaining a
partially initialized chain. Tests cover bytes, NPOT, suffix and bounds headlessly.

CPU authored particle bursts are pending one-shots consumed by the first
emitting update after origin synchronization. Stop delays pending emission;
start resumes it without rearming a consumed burst. Clamp finite rounded burst
counts to free capacity, ignore invalid amounts, and reuse ordinary spawn/event
logic so sub-emitter and parallel-update behavior stays shared.

Camera projection diagnostics are silent by default. Explicit JCE_LOG_CAMERA
retains WARN-level projection/pose witnesses in distribution builds; QA tools
that parse these records must opt in. Projection math is unaffected. The native
camera suite validates default silence and opt-in deduplication with a log sink.

Tracy render.reported_primitives sums the backend per-topology primitive array
with a64-bit accumulator; it is not an address or an exact pre-occlusion/GPU
indirect submission count. Backend reporting includes instances and repeated
passes. Actual submitted vertex counts remain unavailable through the public
GPU statistics API; authored asset sizes must not be presented as observed counts.

Camera projection diagnostics are opt-in (JCE_LOG_CAMERA), deduplicated by camera plus viewport aspect in a bounded cache. Alternating Scene/Game aspects must not spam unchanged poses.

Web owns one SDL canvas: never create a second hidden GL probe window. Negotiate and verify the actual bgfx WebGL context; native platforms retain their independent probe.

## Submitted resource payload telemetry

`jce_upload_metrics.c/.h` counts admitted static mesh vertex/index payloads
(including wireframe, LOD and meshlet auxiliary buffers) and texture initial or
update payloads. Counters belong to `JceRenderer`; the private active-device
pointer and context generation live in the existing singleton bgfx frame/device
owner in `jce_renderer.c`. Pure helper functions own no global state. The pointer
is borrowed, bound only after successful creation and cleared before
shutdown. All mutations and snapshots use the API-owner thread, not CPU decode
workers or the Android frame thread. Valid create handles indicate API admission;
void texture updates indicate command submission only. Cache hits and empty
texture allocations contribute zero. Context generation/reset and saturation
make cumulative deltas explicit. These are not PCIe bytes, GPU completion,
resident allocation totals or transient/GPUScene upload counters.

Web stock PBR programs are prepared as opaque deferred handles until actual
submission; stock capability queries must not link unused shader families.
The private jce_shader_deferred.h resolver is called by the shared encoder and
raw draw bridges. Prepared programs own copied source bytes; a PAK or development directory may
be released before first submission. Missing source stays invalid; program release accepts concrete or deferred handles and reload frees
old variant entries once per distinct handle. Preserve every shader feature.

Prepared shader state belongs to the existing singleton bgfx device owner, with no independent global registry. Device shutdown frees prepared bytes and concrete programs; reload releases old distinct handles. Native loaders remain eager.

## Fixed stock BRDF

The default 256-square split-sum LUT is a fixed first-party RGBA16F payload
linked into this archive through the shared binary wrapper. It works without
a PAK, writable cache or loose file. The offline baker and custom-size fallback
share the exact 1024-sample kernel, texel centers and half conversion.
`data/brdf_256.f16.json` pins payload and kernel hashes; renderer builds verify
these before linking. Regenerate with `python scripts/bake_ibl.py --out
engine/src/renderer/data/brdf_256.f16`. Generated wrappers remain under build.
Different libm hosts may regenerate slightly different values; the checked-in
reference pins delivery bytes, and CPU physical-value comparisons allow half
quantization rather than claiming all-host bitwise arithmetic identity.

Web PostFX pipelines opt into the same owned deferred-program registry. Checks
in execution paths resolve only requested programs; every fullscreen/pyramid
submission uses a concrete handle while destruction/reload keep the prepared
identity. Verify inactive chains, first activation, repeated use, custom shader
replacement, reload and destruction with actual target and pixel witnesses.

GLES HDR/TAA admission reads current sampled RGBA16F framebuffer capability and the active HDR request. Preserve conservative tier defaults and independently verified cost policies; a backend label is not evidence of unsupported attachments. Applied pipeline logs describe the resolved descriptor, not the original request. PostFX format selection must not retain hardware capability across device/context replacement.

TAA status belongs to each PostFX pipeline. Emit the existing on/off marker with
view_base only when its CPU resolve submission changes; a requested flag alone
is not activation evidence. Failed programs, missing quad buffers and invalid
targets report off. GPU completion and presentation need independent evidence.

TAA history is eligible only after a successful history-copy submission in the
preceding apply. Disabled applies, resize, skipped applies and failed copies invalidate
reuse. First/resumed frames and unavailable motion bind zero feedback without
mutating authored parameters; the resolve shader returns current colour before
sampling undefined history. CPU copy submission is not GPU completion evidence.

Consume matrix and external-motion inputs at apply entry, before validation,
so early returns cannot retain a prior frame. Reset output and effect-active
status at entry; motion blur and DOF become active only after their composite
submission succeeds. Camera motion without fresh matrices/depth is unavailable.

A temporary end-frame disarm followed by re-enable without an intervening apply
preserves the immediately preceding history; only an actual skipped/disabled
apply consumes it without replacement. Do not reset on the toggle alone.

Renderer-owned SDF text programs and all four explicit SDF uniform references
are released before bgfx shutdown in both windowed and NOOP lifetimes. Shader
reflection ownership does not replace each create_uniform reference. Validate
real WebGL object deletion before context destruction; GC is not teardown proof.

The shared PBR binder and light binder own their explicit uniform references
and default textures for one renderer device lifetime. Release them before
bgfx shutdown, reset their lazy initialization flags, and recreate them on the
next device. Light environments borrow shared bindings; destroying one
environment must not invalidate other environments on the same device.

The private directional/ambient light upload in
`jce_lighting_system_internal.h` uses the same packed freshness and receiver-layer
selection as the ordinary full upload. It publishes the same ambient, used
directional lanes and complete light-count vec4, preserving shadow slot indices
and layer-excluded zero intensity. It owns no camera or unrelated point/spot/area,
cookie or IES writes. Standalone scene shading owns its final camera value once.
The full public upload retains all previous fields and camera ownership. Exact
CPU comparison controls include zero directional lights and a masked second
shadow-casting directional slot; they do not establish rendered equivalence.

Renderer lifecycle instrumentation lives in jce_renderer_trace.c over private
jce_renderer_lifecycle_internal.h entry points. It wraps actual device creation,
view configuration, CPU submission and teardown for every host, including the
editor. Core service spans are idle when capture is off. These CPU observations
never establish GPU completion or presentation; null frame/cleanup attempts
retain cancellation, creation failures retain failure.

Headless and windowed initialization share `jce_rcb_callback_interface()` so
bgfx fatal records remain observable through the existing logger owner. Headless
initialization does not activate the windowed file-capture success hook. Noop
lifecycle controls must fail on any observed fatal, including records emitted
during backend shutdown; returning from the callback is not a successful outcome.

Startup transient budgets are selected by jce_renderer_budget_internal.h:
Web and native low-memory defaults use 6/2 MiB vertex/index pools; ordinary
native defaults use 8/2 MiB. Native multithreaded bgfx has two Frame pool pairs,
and WebGL orphans full GPU capacity every frame. CPU pool storage is reserved
at initialization even before submissions use it. Logical reserved capacity is
not process RSS or GPU residency. JCE_TRANSIENT_VB_MB / JCE_TRANSIENT_IB_MB
accept explicit 1..256 MiB budgets for large geometry/UI workloads. Encoder
headroom is selected independently. The same private budget owner sets
minUniformBufferSize to 128 KiB per encoder per bgfx Frame; bgfx retains its
on-demand growth, so this is an initial capacity rather than a hard limit.
Sixteen encoders and two native Frames start with 4 MiB of logical uniform
command capacity rather than the backend default's 32 MiB. The theoretical
28 MiB difference is not measured heap, private commit, RSS or GPU residency.
Startup renderer.uniform_initial_encoder_frame_bytes records the per-encoder,
per-Frame initial capacity, never a process total. test_jce_renderer_budget
compiles the complete actual apply_transient_limits body with typed ports; its
missing_uniform_wire control removes only the real limit assignment while
retaining the counter and must fail ordinary unchanged runtime assertions.
These CPU controls do not certify backend growth or rendered workloads.
Capacity changes preserve authored shader and asset settings; verify actual
pool use, exhaustion, pixels and uninterrupted timing before claiming acceptance.
The single ShaderSet owner aliases named PBR members into its variant table.
jce_shader_set_owned_internal.h retires every unique named/variant handle once,
including SDF and partially loaded reload sets, while bgfx remains alive.

Capture-only renderer snapshots record actual resource counts, transient used
bytes, estimated texture/RT bytes and backend timer availability at 60-frame
intervals. The private C99 snapshot emitter also records frame-acquired encoder
handles and effective backend capacity. Startup `renderer.encoder_capacity`
remains the requested limit; `renderer.encoder_effective_capacity` reports the
backend-clamped limit, or -1 without caps. `renderer.encoder_frame_used` counts
the reserved default/API slot 0 (even without draws), plus successful new handle
acquisitions until the next frame boundary. It is neither OS thread count nor
peak simultaneous occupancy. Backend observations can trail the CPU submission
index. Selected adapter vendor/device IDs and raw bgfx renderer type share this
caps observation; the type is not the JCE backend enum. Reported zero values
remain zero; missing caps yields -1 for all three identifiers.
Adapter-reported GPU memory is not process heap; unavailable counters remain
negative. A snapshot is not proof of GPU completion or presentation.

The same private snapshot preserves timing provenance as integer counters:
`renderer.api_frame_number` is the actual bgfx_frame return (the legacy
`renderer.cpu_submission_index` spelling stays compatible), host submission
ordinal is separate, and the existing upload context_generation scopes device
epochs. Unknown/zero or non-int64-representable epochs are explicitly unavailable.
Frame and per-view gpuFrameNum identify the provider frames that generated their
raw begin/end ticks; source frame zero is valid. Frequencies and ticks remain
int64 through the existing JSON exporter, including values above double precision.
A plausible nonempty interval requires positive frequency, nonnegative ordered
ticks and not both timestamps zero. Invalid/initial intervals preserve their raw
values and report duration unavailable; this is not absence of GPU cost. Repeated
(epoch, view/source frame, ticks) tuples remain latched samples, not new GPU work.
All borrowed provider views are emitted at the existing first/60-frame cadence
without another registry or heap; absent view metadata is explicitly unavailable.
Accepted end_frame dispatches the private snapshot independently of trace scope.
Null/fallback/headless and the existing first/60-frame cadence precede the single
PERF_LOG environment read; with both observers off, no stats/caps are queried.
Canonical allocator/global/per-view emission remains trace-only. Explicit
PERF_LOG independently enables the existing provenance and bounded view-name
logs at that same node; enabling both observers consumes one provider snapshot.
Trace off skips per-view counters. Explicit JCE_PERF_LOG records one
bounded integer provenance line per sampled node in DIST, never per-view logs.
Dynamic per-view counter names use the existing canonical ring-only writer,
which synchronously copies each name and preserves the current task span as
id and parent. They never enter optional Tracy plots, whose asynchronous queue
borrows the name pointer; only static global counter names retain that route.
The two-view CPU control checks every suffix, value and copied name plus full-width
parent span after formatter reuse. It does not certify a Tracy-enabled application.
Explicit PERF_LOG also emits one view-name mapping line at that same node,
with epoch/API/global source and each view-specific source ID. Its224-byte
stack mapping stays below the existing512-byte logger payload with the full
integer prefix. Complete entries percent-encode unsafe provider bytes (including
UTF8/control bytes) and never partly append a name. actual/listed/start/truncated/
unavailable distinguish bounded selection from a complete list. The deterministic
start uses the existing submission ordinal/60, without registry/history, so
later nodes cover views that did not fit the first list. Names come only from
the borrowed256-byte provider fields; this cannot recover an upstream name
already clipped by bgfx. Formatting/logging is inside explicit PERF_LOG only,
never trace/Tracy plots. Exact-capacity, encoding, rotation and missing-provider
controls share the existing snapshot fixture; actual name observations still
require a fresh native capture.
Public overlay/raw-stats POD layouts stay unchanged and omit these timing IDs.
Private emitter/actual JSON controls and the compiled missing-source-frame
ordinary-failure mutation do not prove real query latency, device fences,
resource retirement, optical presentation, OS memory or cross-clock alignment.

Stock ShaderSet preparation records only actual loader-call acquisitions in a
bounded stack batch. The shared jce_shader_stock_table_internal.h loader reuses
identical (vs,fs) name pairs: its borrowed table cells do not acquire. The nine
scalar PBR aliases also borrow. Cached duplicate IDs from distinct actual loads
cancel only those extra acquires before publication, retaining one live ref per
unique handle. Manual set_shaders aliases are not normalized. Web deferred IDs
retain independent resolve-time refs and retire per prepared ID. Trace records
actual stock duplicate-acquire counts; zero is valid. CPU refcount controls do
not replace actual backend startup, draw, reload and teardown acceptance.

Actual offscreen keep and overlay configuration also emit renderer.view_configure
through the same private core scope authority. Full prepare delegates to keep and
records once. A true result is CPU configuration success; non-null rejected targets
record failure, null attempts cancellation, and parent scope ownership is restored.
Production Noop controls verify real APIs without a GPU. These spans do not prove
GPU completion, framebuffer residency or presentation.

## Screenshot and readback file ownership

`jce_renderer_bgfx_callback.c` owns one bounded file request and the existing
serial PNG writer. Main-thread request/poll/shutdown operations share a short
SDL lock with render-callback/writer completion; it protects only state and
bounded path copies. No allocation, GPU API, encode, IO or wait runs under that
lock. Paths exceeding 511 bytes reject instead of truncating. A monotonic
identity-bearing bgfx pseudo-path rejects stale/duplicate callbacks, including
repeated output paths; identity exhaustion rejects rather than aliasing.

File screenshots and texture-readback PNGs share admission and real write
outcomes. Raw FBO/recording sinks do not complete a file request. The readback
poll returns pending until the actual writer completes, and its RGBA surface
has one owner. Writer-unavailable backbuffer and allocation/full-queue
fallbacks retain checked inline encoding; unavailable readback workers fail
explicitly. `renderer.screenshot_write` traces actual encode/write CPU work
through the existing core scope, including real FAILED outcomes.

The private screenshot ledger uses RENDER COUNTER names
`renderer.screenshot.admit` and `renderer.screenshot.terminal`, both keyed by
the accepted uint64 file identity in event.id with parent zero. Admit values
are backbuffer=0/readback=1; terminal values are the actual
JCE_TRACE_TASK_SUCCEEDED/FAILED/CANCELLED enum. A traced admission records once
before submission; only its matching pending-to-terminal transition records
success/failure or shutdown cancellation. A matched backend screenshot callback
publishes `renderer.screenshot.cpu_pixels` once after pixel layout validation
and before conversion or encoding. It carries the same file identity, parent
zero and the requested uint16 framebuffer index (65535 is the default
backbuffer). Staged texture readbacks, video callbacks, invalid payloads and
stale requests do not publish it. This is a CPU-readable pixel observation;
source-frame, GPU completion and Present correlation need separate evidence.
State and the admission trace flag
are copied/cleared under the existing lock; ring publication runs outside it.
Rejected, stale and repeated completions do not record. These counters create
no task lifetime, TLS ownership or active-work accounting; public poll still
maps both failure and cancellation to 2. Complete ledger evidence requires a
continuously enabled capture from admission through terminal and the Core
quiescence/loss rules. An admission made with tracing off cannot create an
orphan terminal when tracing later turns on; crossing capture toggles does not
certify a complete pair. No per-frame/poll work or pixel ownership is added.

PNG shutdown closes callback admission, waits only for callbacks already
owning pixels and bounded queued CPU work, then joins the writer. It never
waits for a GPU callback not yet delivered. An unfinished read_texture
destination stays allocated until `jce_rcb_after_backend_shutdown`, called
after bgfx joins; its handle retires in command order. That private hook frees
CPU scratch without calling a stopped graphics backend. The success hook
activates file capture only for a real windowed renderer callback. New
contexts reopen the service without resetting request identities.

This is a single main-thread file-consumer contract, not general concurrent
consumer tickets or a blanket recording-control thread-safety claim. Shutdown
is a blocking drain of actual callback/encoder/file IO; the host filesystem
can determine its wall time. Focused tests use real SDL IO and a deliberate
early-release mutation to verify the borrowed-buffer guard, without a GPU.

## Classic material power boundary

`fs_classic_lit.sc` maps roughness to nonnegative Blinn-Phong shininess. Its
three shininess lobes share a local `classic_lobe_power`: exponent zero is a
constant lobe of one, zero/nonpositive base with a positive exponent is zero,
and positive inputs retain the original pow. Bare pow(0,0) is not portable
across shader languages or GPU implementations. Do not substitute an epsilon
that biases every highlight, or mask NaN output by forcing side faces black.
The fixed-exponent Fresnel powers and material/uniform ABI remain unchanged.
This finite-domain boundary is not a cure for non-finite input vectors.

`tests/cmake/verify_classic_lit_power.py` compiles the exact source and a
runtime-uniform witness through the existing project-shader recipe, all five
modern profiles at shipping optimization. The witness imports the production
helper from the same byte snapshot as the compiled source and hash. Legacy
witnesses derive their expression from an archived source. Compilation and
source/witness provenance controls are not numeric GPU acceptance; real float
output or shader debugging on the actual driver is still required.

## Render-pipeline request lifecycle

The existing requested descriptor owns unclamped user intent; active owns the
resolved hardware/GLES cost budget; pending owns the next frame's user edits.
The cheap stylized material gate reads requested quality: explicit LOW remains
neutral, MID/HIGH/ULTRA honors scene values even if active bloom is budgeted LOW.
The existing boolean QA force remains highest priority; do not invent a numeric
post_quality force knob. All other feature/capability clamps remain unchanged.

Pending setters begin from requested, or the existing LOW preset on cold calls.
Frame-end copies and clears pending before the sole apply implementation,
retaining capability clamps, existing numeric forces and observer-queued work.
A newer full apply replaces old pending; tier re-resolution preserves it. No
request/state duplicate or public ABI is needed. Record parsed asset identity,
requested quality and active gates separately; scene JSON alone cannot prove
actual submitted GPU quality. The sun_soft_size accessor uses angular degrees,
matching the member and PCSS, not a shadow-map texel distance.

Offscreen targets register with their renderer. Retired FBOs age by submitted
renderer frames (eight frames), including inactive viewports; prepare calls
do not advance their age. Full retirement slots reject resizing without replacing
the current target. Renderer teardown releases GPU handles and detaches caller-owned
wrappers, which can subsequently be destroyed safely. Maintenance runs on the
owner thread; the private synchronous submission seam also supports Noop tests.

`jce_offscreen_target_get_extent` reads the current live allocation's admitted
dimensions, including the existing 16..8192 clamp, without a backend query,
allocation, mutation or trace event. Both outputs are required; unavailable
targets and invalid arguments clear supplied outputs to zero. A failed resize
can retain an older live target when retirement is full, or leave no current
target after retirement and failed creation. Renderer shutdown detaches the
wrapper and makes the query unavailable. Success is allocation observation,
not preparation in this frame, draw submission, GPU completion or presentation.
The actual offscreen-owner fixture checks these distinctions and asymmetric
clamped dimensions through real Noop and counted creation ports.

Offscreen retirement uses a private submission ordinal incremented once per
synchronous host submission, independent of bgfx initialization/internal frame
indices. The public raw frame index remains unchanged for readback scheduling.

Offscreen resource observations remain in the existing target and its eight
retired slots. Creation serials come from the core trace authority and are
qualified by their original capture generation; handle 0 and handle reuse are
valid and never resource identities. Reset/late capture records an origin gap,
rather than fabricating an earlier creation or retaining a completed task span.
Actual create/rollback/retire/destroy-submit scopes include attachment/FBO
handles, clamped dimensions, RGBA16F-or-RGBA8/D24S8 formats and single sampling.
Logical bytes describe uncompressed attachment estimates and CPU owner transfer,
not physical GPU memory or completion. Destroy-submit includes the actual
submission ordinal, reason and remaining logical bytes after submission.
Capture off emits no resource events; unchanged dimensions emit no resource
lifecycle events (the existing view-configuration scope is independent).
The private immutable provider is copied into each target; its user context
must outlive the target. Public APIs use one default bgfx allocation/cleanup
chain, and every release uses that same provider. Counted test handles never
enter actual Noop view submission. The existing Noop test also drains the whole
bounded core ring and rejects cursor loss; it does not certify GPU execution.

## Shared CPU instance uploads

jce_instance_upload.c/.h is the private renderer lifetime owner for qualified
32/64/80-byte instance layouts. Reuse its provider and existing device context
generation and submission ordinal; do not add scene-specific allocation policy
or a second mutable global identity. The 128 MiB logical budget includes live
lanes, pending candidates and same-epoch retired handles. It is not a physical
GPU or process memory bound. CPU staging and backend-deferred reclamation have
separate lifetimes and trace observations.

Begin and commit run on the owner thread. Commit publishes the admitted cursor
and candidate only after upload acceptance, transferring staging once. Tokens
retain their admitted handle and context/epoch identity. Already committed
tokens may bind through an encoder in the same epoch; workers do not reserve,
commit, grow or retire the shared owner. Overflow, allocation failure and partial
admission must retain honest counts and cleanup/retry behavior.

The renderer supplies an explicit transient reserve for later non-instance
submissions (1 MiB by default, configurable by
JCE_INSTANCE_TRANSIENT_RESERVE_KB). This design budget does not guarantee space
for every UI workload. Queue dispatch checks aggregate bytes and this reserve
before parallel recording; pressure falls back to the serial owner route.
Keep material/state/stencil, transparent order, index ranges and encoder TLS.
Explicit authored instance payloads retain every admitted item when automatic
merging is disabled. Queue statistics count actual accepted submissions, not
requested work or hardware invocations.

Model uploads retain node transforms, material and LOD traversal. Impostor cards
retain their two-vec4 packing and rebind complete state after every submit.
Scene particle adapters consume this owner without private device ownership.
Exact provider/production-TU controls live in test_jce_instance_upload.c,
test_jce_render_queue_capacity.c and test_jce_impostor_upload.c under
tests/renderer/. These CPU controls require separate
artifact-bound GPU pixels, instance populations and timing acceptance.

Instance resource traces use the existing core scoped-counter encoder, retaining
context generation, token/allocation serials, submission epoch, encoder and
retirement identity. The optional staging observation state has 256 fixed leases
and a copied allocator context; references survive owner destruction until actual
backend release callbacks finish. Tracing off allocates no observation state.
Metadata allocation failure or saturation leaves rendering ownership unchanged
and records unavailable evidence; it cannot certify complete resource tracing.
A complete resource observation requires zero capture loss, metadata-unavailable
and origin-capture gaps, paired transfers/releases or cancellations, and balanced
handle creation/destruction. These observations concern CPU acceptance and backend
ownership callbacks, not GPU fences or physical memory reclamation. Counter scope
IDs are real producer identities; never assume an unscoped ID of zero.
Do not clean up worker TLS from a resource callback that does not own that thread.

TSR history, motion and auto-exposure metering textures belong to the PostFX
pipeline. Their framebuffers
borrow attachments with destroyTextures disabled; resize and destruction release
framebuffers before the pipeline's explicit texture references. Keep this single
owner on validation rejection, framebuffer-handle exhaustion and partial creation.
Exact production-TU controls check these CPU reference requests with the pinned
bgfx contract, including failure followed by retry. Auto-exposure staging readbacks
remain a separate lifetime. Actual draws and resize/shutdown still need GPU
validation.

Shader load failure diagnostics belong to `jce_shaders.c`: preserve the actual
asset key/stages/backend through source allocation, upload, shader creation and
program link failure. Deferred realization reports failure once per prepared slot
and preserves its attempted state; prepared handles are not proof of successful
GPU realization. Reject zero/oversized/truncated blobs before uint32 upload.

Stock `jce_shaders_load_all_fs` delegates each pair to the existing public
`shader_load_program_overlay_named` owner. Each existing overlay stage wins;
an absent stage quietly uses the PAK/embedded fallback, while an existing but
invalid stage fails closed instead of replacing the entire pair with baseline.
The shared owner retains stock deferred preparation and creator-ref cleanup;
stock pair reuse, acquire normalization and borrowed aliases remain unchanged.
The existing real-stock/Noop shader-link target selects only a compiled profile
through a typed renderer-type port and forwards actual host-file reads. It
verifies fragment-only overlay consumption, invalid-stage rejection, null/empty
dev-dir equivalence and independent cached acquires returning shader/program/
uniform counts to baseline. The suffix port does not change the actual Noop
backend or certify native GPU compilation, pixels or a performance gain.

The shared private `link_program_owned` in `jce_shaders.c` links with bgfx's
automatic shader destruction disabled, then releases each creator reference
exactly once on either outcome. A program retains its own stage references;
cached program acquisitions retain independent references. bgfx can reject a
stage-interface mismatch before its automatic ownership branch, so relying on
`destroyShaders=true` leaks shader/uniform ownership on that path. Native PAK,
filesystem, overlay and deferred realization all consume this same transaction.
`renderer.shader.link` records VS/FS/program handle values, actual creator-release
count, success and rejection (1 invalid stages, 2 failed link); handles are not
stable resource identities, and these counters do not prove GPU completion.
Keep trace-disabled ownership identical and deferred attempted/log-once policy.
Counted production-owner controls include an ordinary compiled old-behavior
contrast. `test_jce_shader_link_owned` additionally consumes actual public stock
PAK bytes on Noop, verifies matched/mismatched pairs, cached refs and shader,
program and uniform baselines. It calls the private link owner because Noop has
no public loader suffix; do not call this physical backend/pixel acceptance.

## Shader reload, custom-program and asynchronous bake owners

`jce_renderer_reload_shaders_fs` rejects null, fallback and headless renderers
before loading a candidate. A failed required color program releases every
unique candidate handle and leaves the installed set intact. A successful
reload installs the candidate before retiring every unique old named/variant
handle through `jce_shader_set_release_owned`; SDF and table aliases follow the
same owner. `jce_renderer_set_shaders` admission must agree with reload admission.

`jce_pbr_material.c` owns custom graph programs in the existing 64-entry cache;
returned material handles borrow them. Cache hits remain usable at capacity.
A new key at capacity rejects before blob IO or linking, rather than returning
an uncached program without a destroy owner. The entry-budget warning fires once
per device session and includes count, limit and material path. Failed links
retain the existing negative-cache policy. Shutdown destroys admitted valid
programs once and resets both count and warning state; no public budget changed.
Entries, admission count and the once-warning flag reside in the same private
cache object. A new cache/device lifetime may warn once again; repeated shutdown
remains idempotent. Actual-owner CPU controls cover both epochs and a compiled
runtime contrast with only the warning reset removed; they do not prove GPU work.

`jce_render_readback.c` owns each staging texture and submitted CPU destination.
Its private backend-open/before-stop/after-stop hooks share the actual renderer
lifetime; NULL, unrelated and detached renderer admission rejects before a
dereference. Cancellation changes intent, never the backend borrow. Readback,
screenshot and impostor readiness consume the shared uint32 serial-order helper:
zero and UINT32_MAX are valid, related observations must be less than 2^31
advances apart, and exact half-range remains pending rather than proving order.
Release invalidates a ticket through its generation only after its borrow ends.
Queue destruction advances through the real central submission owner, including
its ordinal, offscreen retirement and instance-upload fan-out. It attempts at
most eight advances, stops on ambiguity/no progress, then retires handles while
retaining CPU destinations until the actual backend-stop hook. Stop detaches
live wrappers; after-stop releases pixels without any GPU call. A caller may
destroy a detached wrapper after a new backend starts, without affecting it.
Transition-only trace counters report service admission, attached/retiring queues
and owned CPU destinations/bytes; backend_stop_cpu_releases counts CPU releases
after shutdown. They allocate nothing, do not scan queues with trace disabled,
and never report GPU residency, a fence or presentation.

The central frame scope measures CPU API submission/handshake. Latched global
and per-view GPU timestamps describe provider intervals and their source frame;
they do not establish a full-device completed watermark. Public bgfx readTexture
readiness proves the particular CPU destination is available. Screenshot flush
joins queued CPU encoding/file work, not pending GPU writes. Public frame FLUSH
omits present; it is not a GPU-idle contract. D3D11 Flush remains asynchronous.
The legacy leakstats fields remain a pre-shutdown backend handle snapshot with
explicit gpu_completion=unavailable; queued destruction/nonzero counts cannot
alone prove a leak. Actual presented/whole-device completion need a backend
contract and physical evidence, not relabeled CPU scopes or guessed frame age.

`jce_renderer_submission_trace_internal.h` emits four static counters inside
each accepted central `renderer.frame_advance_cpu` scope: the still-owned
renderer context generation, actual backend serial before/after the call, and
the submission ordinal before the existing retirement/upload fan-out. These
reuse the scope ID as both counter ID and parent, so ordinary submissions and
teardown drains retain their actual CPU caller. No GPU query, allocation, log,
flush or extra frame is introduced. Trace-off scopes skip identity-field reads;
the emitter returns before counter calls. The original perf-phase timer excludes
the counter writes;
the trace scope retains the normal instrumentation overhead.
Generation survives upload unbind as retained renderer identity; it does not
restore backend availability. Zero or non-int64-representable generations emit
-1; backend serial zero/wrap and ordinal zero remain valid. This per-call packet
does not join latched GPU sample frames, identify individual readback tickets,
or establish physical presentation/completion. Resource collection and ordinal
advancement remain after the API-call scope in their original order.

`jce_impostor.c` has one bake slot with explicit invalid GPU handle sentinels;
zero is a valid owned handle. Shutdown retires its FBO/staging handles while
bgfx is alive. Unissued pixels free immediately; pixels already passed to
`bgfx_read_texture` remain in that same slot with cleared borrowed model/renderer
pointers and closed submit/poll admission. The private
`jce_impostor_after_backend_shutdown` hook runs only after `bgfx_shutdown` has
joined the producer, frees retained CPU pixels without backend calls and resets
the slot to IDLE for the next session. Repeated shutdown/finalize is idempotent.
Do not replace this completion boundary with cancellation, handle destruction,
a guessed frame delay or a second retirement queue.

Ownership controls use exact production owners in
`tests/renderer/test_jce_render_readback_contract.c`,
`tests/renderer/test_jce_impostor_upload.c` and
`tools/tests/test_material_owned_cleanup.py`. Counted CPU ports cover real
submitted destination pointers, zero handles, partial failures, aliases,
capacity and repeated teardown; they do not prove GPU pixels, physical memory
reclamation, driver completion or host device-loss recovery.

Device-loss callback handoff uses an existing JceAtomicI32 exchange, not a
volatile load/store. Notifications raised after consumption remain pending;
multiple pending notifications may coalesce. Backend attempt/after-join resets
prevent a previous session notification from reaching a new host. This CPU
notification contract is separate from recovery or hardware device-loss proof.

Font atlas failure transactions: static nonempty-glyph bitmap allocation failure
aborts build_atlas/font_open_from_memory rather than publishing a partial font.
Its diagnostic names source, font, codepoint and phase. Dynamic pixel/map/texture
creation failures release unadmitted staging and retain retry. Rejected updates
restore unused CPU pixels and publish neither glyph, count nor shelf cursor,
preserving admitted glyphs. One diagnostic per font/failure stage records identity
and codepoint. Update acceptance is command admission, not GPU completion.
Exact-owner CPU probes in tools/tests/test_font_atlas_failure.py cover failures,
retry and compiled historical negatives; actual glyph pixels remain separate.

Mesh creation transactions: jce_mesh_create_internal.h supplies immutable private
creation ports to the actual constructor. Each mesh copies provider identity;
its user context must outlive it. jce_mesh_index_data.h packs the original
triangle prefix and wireframe suffix into one immutable IB for procedural,
PBR and skinned meshes. Triangle counts/getters remain unchanged; every wire
bind starts after that prefix. Preserve unique procedural edges and the
existing PBR/skinned line pairs. No second static IB or retained CPU topology
is needed. Optional topology/staging failure keeps a solid-only upload.
Initialize owned handles to INVALID before acquisition; required staging or
GPU buffer failure releases acquired handles once and returns NULL, including
PBR/skinned uploads. renderer.mesh.create records CPU outcomes. Counted ports
in tests/renderer/test_jce_mesh_create_lifecycle.c cover fixed-capacity
admission and lifetime; these are not GPU completion or frame-time evidence.

Scene directional/local depth surfaces are owned by
`middleware/scene/jce_sr_shadow_targets.c` and admitted by the corresponding
shadow pass, rather than by resolution configuration. The dynamic atlas is
created only for a requested dual pass with dynamic casters. Renderer counters
report logical backend estimates; a source byte calculation or CPU/Noop owner
control does not certify process Private/RSS or physical VRAM savings.

`jce_image.c` remains the single raw-memory image dispatcher. Signature formats
keep SDL_image auto-detection priority. A failed auto decode may retry TGA only
after bounded header/ID/palette/pixel or RLE packet admission; no filename hint,
second codec or pixel decoder is introduced. Both SDL streams close once, and
only the final failed decode logs its stage, byte count and codec reason. The
shipped SDL TGA subset excludes right-origin/interleaved images and nonzero
indexed-palette origins. `test_jce_image_tga` checks actual SDL pixels/alpha,
malformed inputs, and a real `.tga` cooked into a Bundle with zero raw warnings;
these CPU decode/cook controls do not certify GPU upload or presentation.

## Runtime texture residency reliability

`jce_texture.c` owns the single static texture registry and its handle-to-slot
map. Published `JceTexture` values are raw bgfx indices copied by materials,
scene components and UI consumers; there is no binding indirection. Runtime
residency requests retain desired intent but must not destroy/recreate those
handles. Existing dimensions, resident mip, registry mapping and retained
source ownership stay unchanged. Initial upload quality/size/mip selection
still applies before handle publication. A source mirror alone cannot safely
replace a borrowed handle. Dynamic replacement is unsupported until an
explicit stable-handle contract reaches every binding consumer.

Unsupported requests log once per entry and emit a cancelled service scope
once per capture with requested/resident mip counters. This is a reliability
fix, not completed streaming or proven memory reclamation. The exact-owner
CPU regression uses counted allocation/create/destroy ports, actual registry
lookup/swap-removal and retained-source release; it does not create a GPU.

TGA admission rejects decoded RGBA byte/pitch totals beyond INT_MAX before
palette/RLE traversal or typed codec allocation. The pinned SDL 15/16-bit path
discards attribute alpha, and palette32 reduces alpha to a single colourkey;
attribute-alpha 15/16-bit inputs and nonopaque palette32 therefore fail closed
with an explicit tga-admission reason. Opaque palette32 and true-colour32 alpha
remain supported. Counted-decoder controls use complete 32768-square RLE input
without allocating decoded GiB buffers; they complement actual small-image and
Bundle pixels rather than replacing those native decode/cook controls.

Offscreen FBO construction normalizes the pinned bgfx ownership boundary
through one private helper. Raw creation uses destroyTextures=false: invalid
creation leaves both creator references with target rollback, including
validation and handle-pool failures. A valid FBO has acquired attachment
references before the helper drops each creator reference once. Retirement,
query failure and teardown then release the FBO final references. Counted
controls model these transitions and the pinned destroyTextures=true failure
behavior through this same helper. The old-owner mutation changes only the
fixture helper, never the real Noop backend. Logical release observations
are CPU owner submissions, not physical GPU completion.

Explicit JCE_BGFX_ALLOC_TRACE defaults off and owns a fixed 4096-slot
allocation ledger inside the existing singleton device. Addresses remain
private lookup keys; emitted backend_epoch/allocation_id identify a logical
allocation across resize and address reuse without exposing pointers or paths.
Alloc, realloc, failed resize and zero-size free preserve the actual raw
mimalloc provider contract. The short OS-atomic metadata gate covers only
fixed metadata; provider calls and ring-only trace writes happen outside it.
The immutable raw provider and allocation-free ring writer avoid self-tracing;
no profiler, task registration or TLS setter is called from this closure.

The former 512-request prefix no longer truncates lifecycle events. Operations
use the existing bounded ring and continuous capture authority. The identity
ledger continues across trace-off intervals while event-history completeness
fails explicitly. Full storage, unknown input, duplicate live addresses,
identity/byte exhaustion and invalid begin/end mark pairing incomplete without
changing allocation forwarding. Realloc reserves its old slot until the
provider result, allowing an unrelated birth to reuse the old address safely.
Failed realloc restores identity and size. The observed accounting peak can
include reserved in-flight records; it is not a physical-residency peak.
Tracked bytes mean requested live allocation sizes, not mimalloc usable size,
OS Private/RSS or GPU memory. Nonquiescent snapshots expose pending operations.

Begin is permitted only at quiescent backend initialization; end runs after
failed bgfx init cleanup or the real shutdown join, including API rejection and
renderer-object OOM exits. End preserves residual live identities/count/bytes.
Fallback/null destruction does not close an unrelated backend. A complete
ledger can still report unreleased allocations. all_released requires a closed,
quiescent and complete ledger with zero live records; event_history_complete
also requires an uninterrupted original capture. Consumers must additionally
require existing capture_complete/write_ok, stable generation and zero
lost/unexported events. No counter by itself certifies a global capture.

Renderer snapshots retain the first-submission plus sixty-frame cadence.
test_jce_bgfx_allocator_trace consumes the actual private owner, real core ring,
OS atomics and aligned allocator with failure/move/pool-reuse ports. Original
forwarding/capture/saturation controls remain; ledger controls add reuse,
reservation overlap, capacity, unknown/duplicate input, identity exhaustion,
invalid begin/end and concurrent release. The exact-source Python positive and
runtime mutants reject lost resize identity, address-derived identity and
publication under the metadata gate. The existing profiler negative remains.
These CPU controls do not certify bgfx/GPU execution or physical reclamation.

The singleton device owns atomic idle/creating/active admission. Both public bgfx create adapters reject an active or pending device before platform/caps/allocator initialization; failed creation releases only after quiescent cleanup, and active admission remains held until destroy returns after backend join. Fallback/null destruction never releases a live bgfx admission. Registered-main policy rejects foreign creation; unmarked standalone callers retain the bgfx API-owner contract. Atomic admission does not enable arbitrary-thread rendering. Opted-in allocator observation warms jce_time_ticks_ns on this quiescent owner even while capture is off, avoiding SDL timer first-use initialization inside the callback. Concurrent capture reset remains unsupported; request/result gap diagnostics do not promise an atomic event group. Refused creates record one ring-only renderer.create.rejection_reason counter (1 busy, 2 registered foreign) correlated with the existing create task. Trace-off remains silent; no repeated INFO/WARN path is added.

The public VSync wrapper delegates dimensions to the existing size-aware owner.
Null/fallback renderers and unchanged state return before querying bgfx or
resetting the swapchain. Only an actual state change resolves current dimensions;
explicit complete dimensions remain query-free. Actual wrapper/size-owner CPU
ports cover these guards and the historical unconditional-query runtime failure.
This does not certify device VBlank timing or every-other-VBlank scheduling.

The font subsystem has one `s_text` owner for its FreeType library, next-open
SDF setting and live immutable source references. Fonts remain subject to the
existing single-thread create/draw/close contract. Each font owns an independent
FT/HB face, size, shaping key and static/dynamic glyph atlas. Source bytes may
share only after the existing host/PAK/VFS read succeeds and actual size,
`jce_archive_content_hash` and byte comparison all match. Paths, labels, mount
addresses and stored PAK hashes are not content authority; failures never fall
back to cached bytes. A hash collision must not merge different bytes.

The blob registry contains live references only. Each node has a monotonic
internal serial, independent of addresses. Acquisition consumes its temporary
owned input once; the memory API still retains no caller-owned pointer. Every
failed FT/Font/atlas transaction drops the acquired reference. Close destroys
HB/FT before releasing bytes, and the last reference unlinks/frees immediately.
Shutdown retains its existing closed-font precondition and never clears a live
registry to fabricate closure. Serial/reference/live-byte overflow rejects
admission with a bounded reason diagnostic rather than saturating into success.
Common acquire/hit/release scopes report identity, references and unique live
raw bytes; trace-off ordinary operations emit no events or routine logs.

`test_font_atlas_failure.py` consumes actual source owners with counted ports,
including content-collision and missing-release runtime negatives. The existing
real-font fixture includes the production text owner once and tests actual
FreeType/HarfBuzz independence and Noop texture teardown/reopen using licensed
stock bytes. These controls establish byte ownership and font behavior, not
default Print identity, physical GPU completion or an OS RSS/IO saving. Source
reads and temporary buffers still occur on open.

`jce_postfx_taa_supported` exposes the same current target-format predicate
used by `jce_postfx_set_taa`: live backend RGBA16F framebuffer capability and
the active pipeline's HDR policy. Projection drivers query before jitter;
the setter still rejects incompatible direct requests. Do not cache this
across backend/pipeline changes or claim it certifies resolve shaders, depth,
external motion, target creation or GPU completion.

Renderer startup records selected bgfx vendor/device identity directly through the logger in DIST. Requested or enumerated adapters are separate observations and never substitute for the active device; unknown/no-capability identity remains unavailable. The startup record is bounded and never emitted per frame. GPU Engine LUID attribution and memory ownership still require actual native evidence.

Volumetric fog render/composite own their bgfx view labels after admission, so per-view GPU diagnostics retain the pass identity without changing reserved view IDs, render order, parameters or quality.

View names persist in the backend until another name is submitted. Splash owns
view0 as Renderer/Splash only during startup; each admitted normal begin-frame
restores Renderer/Main before its ordinary clear/touch. Admitted offscreen
texture composites label their supplied view Renderer/OffscreenComposite. In
the Game View this includes PostFX fold-back at120+51=171;170 is only the PostFX
chain tail and its active meter/present paths label themselves. These labels
use the pinned C99 INT32_MAX NUL sentinel and do not change IDs, order or quality.
The F9 recorder instead uses ImGui250 plus1/2 (251/252): its resolve is already
UI/Recording; staging-blit naming remains separate, and a generic shared
readback label must not overwrite UI/ImGui250 used by screenshot capture.
Provider ViewStats names can trail CPU submissions with the corresponding
source frame. A correct label is attribution metadata, never GPU completion.



Native Windows D3D11/D3D12 Auto startup first considers the hardware adapter
ranked by DXGI high-performance preference. Its exact LUID must map to the
pinned classic bgfx first vendor/device key and fit Caps::gpu. If unavailable
or not representable, use the existing window display adapter and driver
fallback. If the preferred device's init or API admission fails, retire that
attempt and retry its backend once with the legacy selection. Explicit
vendor/device requests, non-D3D and headless retain their prior policy.
Window observation also runs for explicit D3D requests, preserving the original
init keys, effective request and applied=false. Its result remains diagnostic;
Auto-only selection limits never rewrite that explicit-request observation.
Non-D3D attempts do not query the window display.
The private per-create state queries each needed platform observation at most
once across the backend fallback chain. The startup log and trace distinguish
the preferred render request from the observed display owner and selected
caps. No process-global adapter cache or device migration is introduced. A candidate beyond the actual pinned bgfx Caps::gpu array is
refused. Inexact or unavailable matching retains the previous fallback. The
existing adapter parser/enumeration code is included by the same renderer TU
from `jce_renderer_adapter.inc.h`; this is not a second device owner. Startup
records observed key/LUID, effective requested key, selected caps key, refusal
reason and exact-match flag once through the DIST logger and common trace.
Trace-off has no ring work; selection mismatch is WARN rather than silent.
`test_jce_renderer_display_adapter` consumes the exact private policy, extracted
actual init prefix and actual startup diagnostic body. The old-device and
missing-wire mutants, plus the old-explicit-observation whole-owner control,
must fail ordinary assertions after compiling. These CPU
fixtures do not certify native adapter selection or prove lower Copy traffic.
The process NvOptimus/AmdPower exports and vendor aliases remain unchanged.

Cooked texture rejection diagnostics belong to `jce_texture.c` and its private
`jce_texture_diagnostics_internal.h`, using the existing logger and trace ring.
The opaque CPU decode object carries authored path hash and cooked source hash
to API-thread upload; decoder workers never query GPU capabilities. Header,
chunk, metadata, pixel decode, layout, format, capability, allocation and create
refusals keep distinct stages. Missing caps are unavailable, not observed zero.
Short metadata and size truncation refuse before copying or allocating upload
memory. Successful handle zero remains valid; upload success is API acceptance,
not GPU completion. The direct public cooked upload has unknown asset identity.
Identity hashes are correlation metadata, not authentication or collision-proof
content authority. One warning is emitted per rejected decode/upload request;
existing scene/UI failure caches govern retries and invalidation, with no new
registry or per-frame diagnostic scan. Repeated explicit public requests remain
separate attempts. Existing Pak lookup and raw image errors retain their owners.

Web API version admission lives in `jce_renderer_web_api_internal.h`, sharing
the existing `jce_renderer_caps.c` API authority and one private context identity.
The real post-bgfx-init decision refuses missing/unknown/mixed-epoch/below-floor
API or GLSL observations before accepting the backend and uses existing
shutdown/join/context-release cleanup. Parse the actual embedded OpenGL ES
version rather than WebGL's leading version namespace. Native missing-version
policy and actual feature/format caps remain unchanged. Public API info, running
backend name and begin-frame observe that admitted epoch; changes clear verified
flags and require actual renderer recreation before new admission. Cache reset
also detaches the current platform listeners. The private frame-admission gate
consumes that same status in Begin/View/End and central submission; rejected
work cannot call bgfx_frame or advance submission/retirement/instance ordinals.
Private implementations return real results to the trace adapter while public
void ABI remains unchanged. Non-null refusal is FAILED, not pointer-only success;
null calls remain cancelled and Noop/fallback retain their existing policies.
Engine failure stops callback/asset/draw work through its existing cleanup owner.
No automatic context recovery, resource reconstruction or GPU completion is certified.
The 11-case CPU provider fixture consumes this exact owner, real version
authority and the actual post-init decision/cleanup tail; four ordinary failing
controls cover wrong namespace, missing publication, stale epoch and missing
init wire. Real Web context-loss/recreation remains separate acceptance.

## Public GPU timing observations

`jce_gpu_caps.c` owns the independent size-bounded
`jce_renderer_get_gpu_timing_observation` API. Existing `JceGpuStats` layout and
active getter values remain unchanged. The legacy `jce_renderer_get_gpu_stats`
also uses the existing active-context bridge before querying bgfx: before bind,
after unbind, invalid generation/payload or Web context loss returns zeroed
unavailable output without touching the backend. Bound Noop/headless statistics
remain valid even when timers are zero/unavailable; no GPU completion or release
is inferred from either result. The private renderer-device bridge reads the
existing active payload generation and CPU-side host frame; unbind or Web
context invalidation refuses backend access. No allocation, query, frame advance
or additional device registry lives in that bridge.
The API copies signed raw timer frequency/begin/end and bgfx's GPU sample frame,
with explicit context/backend/frequency/nonforward-interval unavailable reasons.
Negative or equal/reversed ticks are not a zero-ms performance observation.
Read-time context/host-frame identity does not join the possibly older GPU sample
or prove GPU completion, presentation or resource retirement.

## Complete camera value clones

`jce_camera_clone` lives with camera allocation and state in `jce_camera.c`. It
allocates an independent tracked CPU object and copies the entire private value
state, including rolled/pole pose, FPS control basis and both orthographic
extent modes. NULL source and OOM return NULL; neither mutates the source nor
requires a renderer. Consumers must destroy each owned camera independently.
Do not reconstruct a complete clone from partial public getters.

GPU annotation package capability is published through the existing bgfx
attempt log and same-scope trace counters bgfx.gpu.annotation.request/option/
compiled. These are generated package metadata, not native marker observations:
request -1 means auto, while option/compiled -1 mean unknown. Missing or invalid
macros remain unknown; CPU callbacks and runtime debug flags cannot infer them.
The upstream build enables Debug even with option OFF and excludes WindowsStore.
No new per-frame logging, callback API, device query or GPU acknowledgement is
introduced. Actual D3D/Vulkan/GL marker availability requires backend captures.

Startup shader-root ownership lives in jce_renderer_shader_dev.inc.h, included
after the opaque Renderer definition. The existing device state borrows its
actual instance at the same two successful metrics bindings. A Renderer owns an
optional copied root plus a distinct startup-bound fact; empty/unset binds no
allocation. API-owner, current device, instance identity and Web-context
admission precede binding or root access. Repeated or foreign bindings preserve
the snapshot. Destruction detaches the matching device instance before freeing
the root. Failed copies publish nothing, and failed boot uses normal teardown.
The private named loader delegates unbound/empty state to the unchanged explicit
PAK loader; configured state delegates to the existing per-stage overlay owner.
Caller PAK, independent VS/FS names, invalid-present-stage refusal and program
creator reference ownership remain unchanged. There is no extra environment
read, per-frame root polling, global registry or shader-manager lease. The
existing initialized-backend/API-owner precondition still applies to unbound
PAK delegation. Noop controls are CPU evidence; Web loss refuses this private
load. This startup snapshot does not update valid Scene caches on Reload.

## PostFX output target allocation contract

`jce_postfx_get_output_target` is a versioned owner-thread CPU query. Exact
size/version mismatch does not write; unavailable output is cleared. Empty
chains report borrowed input with unknown extent/format and no writable FBO.
Owned output requires the current apply slot's complete texture/FBO pair and
allocation-time uint16 extent/format; handle zero is valid. Ping-pong allocation
keeps the FBO attachment reference and releases the caller texture reference
on success or failure. Metadata publishes only after both handles succeed and
clears before retirement, on resize and partial failure. This is no GPU
completion, context-generation or residency guarantee. Borrowed handles expire
at the next apply/resize/destruction. Color-only overlay consumers must clear
NONE, require no depth/stencil, and submit before that expiration.

The private encoder preparation owner retires an unresolved deferred program's
already recorded draw state with BGFX_DISCARD_ALL against its captured explicit
encoder or implicit recording. Implicit-only routes pass NULL independently of
unrelated TLS. Successful program, depth and flags retain existing behavior;
a rejected submission is not counted. All three existing submit routes share
this tail. Complete-header CPU controls and the ordinary missing-abort mutant
cover owner routing and clean next-draw reuse, not GPU acceptance/completion or
the cause of any specific application failure.

The private lighting camera binder reuses the existing named camera uniform
and its initialization/retirement owner. It publishes the caller's actual
position once without changing the light environment or uploading other lights.

Foliage model LOD resolves the effective material per actual band. A missing
material table or out-of-range material index binds the ordinary default inside
that band before the existing frame callback; present authored materials retain
their behavior. The scene adapter does not prebind another default material.
Exact-owner model controls check both missing-material cases and preserve the
existing comparison-PCF routing assertions.

## Cookie texture compatibility and ownership

`jce_light_cookie_internal.h` owns the per-device cookie texture create, typed
bind and idempotent destroy used by `jce_lighting_system.c`'s existing `s_lu`.
The shared shader-profile predicate excludes cookie arrays on compatibility
GL/ES even if the actual driver advertises arrays. Missing array/layer support
also performs no cookie allocation or stage-13 bind. Authoring descriptors stay
intact; the unavailable effect is diagnosed once per lighting lifetime.
Array-capable modern paths retain the 16-layer 256-square white atlas and
per-light sampling. Disabled/unavailable atlases use two initialized white
layers: bgfx treats one layer as 2D, which cannot satisfy the array sampler.
NULL allocation never reaches texture creation; failed handles never bind or
destroy, and a legitimate handle zero is owned and destroyed exactly once.
The IES texture remains a separate 2D owner. Counted CPU controls establish
ownership and calls only, not graphics execution, pixels or performance.

## Composite LUT resource ownership

`jce_postfx_grade_internal.h` owns the composite's uniforms and initialized
3D placeholder inside each private pipeline; its LUT/size/strength are borrowed
authoring state. Compatibility GL/ES uses the shared shader-profile predicate
and performs no LUT resource creation or stage-2 bind. A requested unavailable
3D LUT warns once per pipeline; the default neutral state is quiet. There is
no 2D LUT conversion in this compatibility path. Modern grade params, tonemap
gating and placeholder sampler type remain unchanged. Failed acquisition
releases valid partial resources; NULL memory never reaches texture creation.
Handle zero is valid; release is idempotent and never owns the authored LUT.
CPU owner controls and GLSL payload compilation do not certify driver output.

Decal pool creation uses the shared renderer shader-profile predicate before
any allocation, suffix lookup or shader acquire. Compatibility GL/ES refuses
with an unsupported WARN on the actual create attempt; scene authoring data
remains intact. Allocation/link/sampler failures return NULL with error
diagnostics and valid partial resources are released. Program linking retains
bgfx destroyShaders ownership; zero is a valid program/uniform handle. The
scene's per-owner retry latch and explicit reset define retry frequency.

Explicit bgfx trace mirroring remains opt-in through the process-latched
`JCE_GFX_DEBUG` selector. Its callback sink uses direct public INFO writes so
DIST does not erase the requested diagnostics; ordinary runtime log-level
policy remains authoritative. API-version lines still feed metadata while
mirroring is off. A prefix marks the callback's own 1024-byte formatting
truncation; the canonical logger independently marks any further truncation.
Source basenames use comparisons only within the same non-NULL path string.
This sink cannot restore a compiled-out upstream BX_TRACE producer and never
classifies arbitrary driver text as fatal or mutates device-error state.

Underwater absorption samples the active scene depth and writes the existing
HDR color with two RGB blends in one sequential view. A managed offscreen
destination acquires a lazy color-only framebuffer alias when its attached
depth is the sample source. The alias owns one reference to the existing
color texture. It creates no texture or copy and shares the parent allocation
generation and bounded retirement slot. Submit alias destruction before parent
destruction. Allocation refusal preserves the parent and the next eligible
submission may retry. The alias and draw scopes describe CPU calls. Verify
actual SRV, RTV and DSV bindings separately. For a caller-owned framebuffer
outside this bridge, the caller supplies a depth texture independent of the
output attachments.


Cloud density uses world metres on all three noise axes and an independent
height envelope. Cellular F1 is a distance: the Perlin remap uses F1-1 so
billow potential increases toward cell seeds, not toward their walls. The
12-cell base frequency describes approximately 341-metre primary billows in
the 4096-metre tile. Coverage carves potential without multiplying surviving
optical density; extinction and coverage remain separate controls. Split
shape/detail combination is identical to canonical CPU density and ground
shadow density. The authored coverage number is a control, not a measured
screen-space cloud fraction; authored scenes need visual calibration after
morphology changes. Keep period, deterministic, erosion, precipitation and
split-vs-combined property tests when changing this field.

The private jce_pbr_cull_override.h owns a render-thread two-sided scope beside
the existing PBR UV override. Apply it to final mesh/model/skinned color and
shadow states and the PBR normal-side uniform. Neutral scope is byte-identical;
never mutate imported material storage or leak the scope to a subsequent draw.

Underwater construction admits all required buffers, shaders, programs and
uniforms before publication. Any failure releases only acquired ownership and
returns NULL; render independently guards every required handle. Index-buffer
statistics diagnose allocation refusal without assuming VRAM exhaustion or
changing backend capacity. CPU injected failures are not native GPU results.

glTF CPU upload admits a model only after every nonempty primitive's required
vertex/index source and GPU mesh succeeds. On refusal, the existing model and
CPU destructors unwind transferred and untransferred ownership once. Publish
node/primitive counts only with allocated arrays. Empty primitives and the
existing optional material/LOD policies remain independent; do not cache a
partial missing-geometry model as successful admission.

`jce_pbr_depth_internal.h` declares private MASK depth variants. Their lazy
program cache lives with the PBR material owner and is released at shutdown;
a failed variant is refused for that lifetime, never replaced by opaque depth.
`jce_pbr_material_bind_depth` reuses the stock albedo, factor, cutoff and active
UV transform, binding only the alpha source at sampler 0. Opaque draws retain
their existing programs and require no alpha uniforms or texture binding.
`jce_model_depth_bind.inc.h` selects the effective material per primitive;
`jce_model_depth.inc.h` owns ordinary, skinned, crowd and velocity submissions.
Other model depth routes use the same selector, including every instance chunk
after DISCARD_ALL. Preserve existing cull scopes and triangle index spans.
This owner does not add custom graph alpha, per-instance tint alpha, imported
material double-sided policy or procedural FoliageCluster camera-depth support.
CPU binding controls do not replace shader compilation or native image checks.


## Cloud noise integer reuse

`jce_cloud_noise.c` owns the shared XY-prefix and Z-tail integer hash helpers.
The Worley neighbourhood reuses nine wrapped XY prefixes across its three
Z planes. Preserve all 27 cell visits, the unwrapped feature positions, integer
wrap semantics and the existing floating-point operation order. Hash reuse
must not change density samples, noise frequencies, shadow resolution, slice
budgets or publication cadence. Existing cloud-noise and shadow property owners
remain authoritative; complete-buffer CPU parity is separate from native GPU
pixels and whole-frame timing.
