# Capability liveness

A declaration, menu item, dependency or passing unit test does not prove a feature is used by a shipped application. Classify the capability and cite actual definitions and callers.

| State | Evidence |
| --- | --- |
| absent | Searches of public headers, implementation and consumers find no supported path |
| header-only | Public declaration exists without usable implementation |
| stub | An implementation returns a placeholder or reports success without doing the work |
| implemented-unwired | Usable implementation exists without product callers |
| wired-editor-only | Editor callers exist, shipped runtime callers do not |
| shipped | The required product path executes in the tested build variant |

Search the public umbrellas, component registry, implementation, editor, scripting and the relevant examples project before declaring absence. An internal implementation is evidence that machinery exists, even when a consumer API is missing.

```bash
rg --files engine/include/jce
rg -n 'jce_runtime_create|jce_runtime_step' engine/src editor/src examples scripting
```

Classify definitions, declarations, comments, tests and product callers separately. tests/ is tracked; test callers still do not establish product wiring. Inspect compile-time gates and configuration for the requested release/dist variant.

Measure both editor and shipped runtime when a parity claim covers both. Prove the relevant path executed before evaluating output quality. Historical counts and old captures must be remeasured against the current source. Use concrete repository paths; an ellipsis is prose, never a path whose existence is evidence.

Font and foliage acceptance must exercise first use, missing-to-present retry,
VFS switching, authored edits and destruction. Canvas resource generation is
owned by resources.c; explicit public invalidation is required for an otherwise
unobserved same-path replacement. Grass current preparation and complete caster
inventory precede shadow-cache admission; a populated cache is not proof that
the current request prepared or rendered successfully. Compare colour/depth
deformation on a real backend. A CPU failure-reason/trace receipt is distinct
from an observed pixel result; preserve both and retain current gaps.

Custom project asset roots must reach render-settings, resource resolution and
packaging through the existing project manifest owner; testing only default
resources/assets misses a real editor consumption failure. Terrain's public
32-byte vertex and Mesh's 48-byte vertex are distinct contracts: the renderer
converts explicitly before mesh creation, with owned allocation failure and
retirement, instead of a pointer cast. Exercise visibility and cast-shadow
controls plus Undo/Redo on the same current native scene, then compare GPU output.
A before-fix screenshot or a successful CPU conversion is not a post-fix pixel
receipt. Process working set is separate from allocator-owned bytes and GPU bytes.

Owned backend handles start with an explicit invalid sentinel, including lazy
resources: zero is a valid bgfx handle. Test a cold scene that never prepared
the lazy resource, then destroy it and compare texture/framebuffer baselines
before backend teardown. Route backend fatal diagnostics through the shared
callback and assert zero fatals; receiving a diagnostic must not turn an invalid
release into success. Uniform symmetry and texture ownership are separate
checks. Editor initialization failure paths release every owner already acquired,
including Canvas resources, and clear destroyed handles for repeated cleanup.

Readback cancellation is a logical state, not completion of a backend borrow.
Repeated cancel/release must retain CPU storage until the submitted ready frame
or backend join. Shutdown retires GPU owners before joining the backend and
releases retained CPU storage afterwards; the next session starts idle.
Callback-to-host notifications use atomic read-and-clear; do not assume a fatal
will repeat to compensate for a lost event. Counted interleave controls and
Noop resource baselines remain separate from hardware/GPU completion evidence.

FoliageCluster transient submission retains whole-cluster admission: insufficient capacity, NULL allocation or short allocated count reject before copying. Trace receipts retain requested, actually submitted, rejected and reason with owner/view/entity; a bounded renderer reason bitset suppresses repeated WARNs. CPU buffer preparation is separate from command submission and from GPU/pixel acceptance. Keep shader/cache and authored alpha-mask fallbacks separately observable; transient controls alone do not certify those earlier paths.

Model callback ownership follows JceSceneRendererCallbacks.load_model: success
transfers an independently owned model to the requesting scene/pick cache; never
return a shared borrowed object. Current Editor callbacks consume predecoded CPU
results or create a new model from bytes. Cache invalidation/teardown retires
those results. The pick pass borrows a scene-renderer result for the current ID
render only, then re-resolves next request; standalone fallback owns its loads.
Current callback-mode model VRAM charges/eviction remain untracked/disabled by
policy despite that ownership. Do not use has_cbs, a zero GPU-byte query or stale
comments to infer borrowing or physical residency. Cache recovery CPU controls
still need actual shader/model and pixel validation in the declared host.

The Editor's Scene and Game views consume one SceneRenderer with distinct view
bases. Camera prepass MRTs belong to a private four-view cache, with current
handles borrowed for each render. Stable view/extent/layout requests must retain
their handles across interleaving; resize affects only that view. A complete
candidate must exist before replacement or LRU eviction. Failure retires only
the candidate, preserves prior ownership and rejects the current prepass; reset
and destroy retire every resident set once. Trace CPU spans and correlated
owner/view/handle/generation counters establish requests and logical ownership.
Counted owner controls and Noop baselines do not prove GPU completion, pixels,
or a Copy-engine performance cause. When SSAO/SSR/TAA/water/SSGI/planar consumers
are disabled, this path cannot explain that scene's observed Copy utilization.

Probe artifacts need independent container fields, face/mip order and radiance
checks. A half-float target or HDR filename does not establish linear output:
gamma/dither can change the stored values while preserving directions and values
above one. Keep known emissive markers and the same numeric oracle after fixes.
Distinguish a pinned writer's exact legacy header convention from standard KTX1
typed fields; accepting legacy reads is not external-tool compatibility. Adapt
owned bridge output and retain upstream bytes. A public unversioned config must
not gain an appended flag under an assumption that old binary padding is zero;
prefer an ABI-compatible entry with explicit per-call ownership.

### Scene-model preparation and residency

`jce_scene_renderer_prepare_resources` admits the preparation/poll operation.
Its successful return describes that call. `model_cache_count` counts resident
models, and a null `get_model` result does not distinguish unrequested, pending
and failed entries. Validate the project's required model closure and actual
admission, decode, upload and teardown separately. Generic AssetManager status
belongs to that manager; the built-in SceneRenderer model cache has a separate
owner. Editor loader callbacks and the built-in asynchronous loader can use
different preparation policies. Record that boundary when comparing cold starts.
