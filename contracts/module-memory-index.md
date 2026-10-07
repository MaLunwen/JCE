# Module ownership index

AGENTS.md is the root engineering authority. Each charter below owns the
rules for its module; follow the closest applicable charter. This index is a
maintained map, not a delivery plan or a snapshot of feature counts.

| Scope | Charter |
| --- | --- |
| Repository | [AGENTS.md](../AGENTS.md) |
| conan | [conan/AGENTS.md](../conan/AGENTS.md) |
| editor | [editor/AGENTS.md](../editor/AGENTS.md) |
| editor/resources | [editor/resources/AGENTS.md](../editor/resources/AGENTS.md) |
| editor/src/core | [editor/src/core/AGENTS.md](../editor/src/core/AGENTS.md) |
| editor/src/dialogs | [editor/src/dialogs/AGENTS.md](../editor/src/dialogs/AGENTS.md) |
| editor/src/game_modules | [editor/src/game_modules/AGENTS.md](../editor/src/game_modules/AGENTS.md) |
| editor/src/gizmo | [editor/src/gizmo/AGENTS.md](../editor/src/gizmo/AGENTS.md) |
| editor/src/io | [editor/src/io/AGENTS.md](../editor/src/io/AGENTS.md) |
| editor/src/panels | [editor/src/panels/AGENTS.md](../editor/src/panels/AGENTS.md) |
| editor/src/panels/material_graph | [editor/src/panels/material_graph/AGENTS.md](../editor/src/panels/material_graph/AGENTS.md) |
| editor/src/scene | [editor/src/scene/AGENTS.md](../editor/src/scene/AGENTS.md) |
| editor/src/shadergraph | [editor/src/shadergraph/AGENTS.md](../editor/src/shadergraph/AGENTS.md) |
| editor/src/ui | [editor/src/ui/AGENTS.md](../editor/src/ui/AGENTS.md) |
| editor/src/viewers | [editor/src/viewers/AGENTS.md](../editor/src/viewers/AGENTS.md) |
| editor/src/widgets | [editor/src/widgets/AGENTS.md](../editor/src/widgets/AGENTS.md) |
| engine/include/jce | [engine/include/jce/AGENTS.md](../engine/include/jce/AGENTS.md) |
| engine/include/jce/middleware/net | [engine/include/jce/middleware/net/AGENTS.md](../engine/include/jce/middleware/net/AGENTS.md) |
| engine/include/jce/middleware/physics | [engine/include/jce/middleware/physics/AGENTS.md](../engine/include/jce/middleware/physics/AGENTS.md) |
| engine/include/jce/middleware/scene | [engine/include/jce/middleware/scene/AGENTS.md](../engine/include/jce/middleware/scene/AGENTS.md) |
| engine/include/jce/renderer | [engine/include/jce/renderer/AGENTS.md](../engine/include/jce/renderer/AGENTS.md) |
| engine/include/jce/runtime | [engine/include/jce/runtime/AGENTS.md](../engine/include/jce/runtime/AGENTS.md) |
| engine/java | [engine/java/AGENTS.md](../engine/java/AGENTS.md) |
| engine/resources | [engine/resources/AGENTS.md](../engine/resources/AGENTS.md) |
| engine/shaders/graph | [engine/shaders/graph/AGENTS.md](../engine/shaders/graph/AGENTS.md) |
| engine/src/application | [engine/src/application/AGENTS.md](../engine/src/application/AGENTS.md) |
| engine/src/middleware/ai | [engine/src/middleware/ai/AGENTS.md](../engine/src/middleware/ai/AGENTS.md) |
| engine/src/middleware/animation | [engine/src/middleware/animation/AGENTS.md](../engine/src/middleware/animation/AGENTS.md) |
| engine/src/middleware/audio | [engine/src/middleware/audio/AGENTS.md](../engine/src/middleware/audio/AGENTS.md) |
| engine/src/middleware/net | [engine/src/middleware/net/AGENTS.md](../engine/src/middleware/net/AGENTS.md) |
| engine/src/middleware/physics | [engine/src/middleware/physics/AGENTS.md](../engine/src/middleware/physics/AGENTS.md) |
| engine/src/middleware/save | [engine/src/middleware/save/AGENTS.md](../engine/src/middleware/save/AGENTS.md) |
| engine/src/middleware/scene | [engine/src/middleware/scene/AGENTS.md](../engine/src/middleware/scene/AGENTS.md) |
| engine/src/middleware/ui | [engine/src/middleware/ui/AGENTS.md](../engine/src/middleware/ui/AGENTS.md) |
| engine/src/middleware/video | [engine/src/middleware/video/AGENTS.md](../engine/src/middleware/video/AGENTS.md) |
| engine/src/middleware/world | [engine/src/middleware/world/AGENTS.md](../engine/src/middleware/world/AGENTS.md) |
| engine/src/os/core | [engine/src/os/core/AGENTS.md](../engine/src/os/core/AGENTS.md) |
| engine/src/os/platform | [engine/src/os/platform/AGENTS.md](../engine/src/os/platform/AGENTS.md) |
| engine/src/renderer | [engine/src/renderer/AGENTS.md](../engine/src/renderer/AGENTS.md) |
| engine/src/resource | [engine/src/resource/AGENTS.md](../engine/src/resource/AGENTS.md) |
| engine/src/runtime | [engine/src/runtime/AGENTS.md](../engine/src/runtime/AGENTS.md) |
| engine/tools_include | [engine/tools_include/AGENTS.md](../engine/tools_include/AGENTS.md) |
| engine/ui | [engine/ui/AGENTS.md](../engine/ui/AGENTS.md) |
| examples/caged_kingdom | [examples/caged_kingdom/AGENTS.md](../examples/caged_kingdom/AGENTS.md) |
| tests/consumers/benchmark | [tests/consumers/benchmark/AGENTS.md](../tests/consumers/benchmark/AGENTS.md) |
| tests/consumers/elemental_serenity | [tests/consumers/elemental_serenity/AGENTS.md](../tests/consumers/elemental_serenity/AGENTS.md) |
| tests/consumers/minesweeper | [tests/consumers/minesweeper/AGENTS.md](../tests/consumers/minesweeper/AGENTS.md) |
| tests/consumers/snake_seven | [tests/consumers/snake_seven/AGENTS.md](../tests/consumers/snake_seven/AGENTS.md) |
| scripts | [scripts/AGENTS.md](../scripts/AGENTS.md) |
| tests | [tests/AGENTS.md](../tests/AGENTS.md) |
| tools | [tools/AGENTS.md](../tools/AGENTS.md) |
| tools/audit | [tools/audit/AGENTS.md](../tools/audit/AGENTS.md) |
| tools/build | [tools/build/AGENTS.md](../tools/build/AGENTS.md) |
| tools/coverage | [tools/coverage/AGENTS.md](../tools/coverage/AGENTS.md) |
| tools/lint | [tools/lint/AGENTS.md](../tools/lint/AGENTS.md) |

Native standalone editor build and PE verification: tools/build/standalone.py,
owned by tools/build/AGENTS.md; editor selection: JCE_EDITOR_STANDALONE.

Model-local CPU/GPU texture ownership: engine/src/renderer/jce_model_texture_pool.c/h;
owned by engine/src/renderer/AGENTS.md; material slots borrow unique resources.

Core-5 editor measurements: tools/benchmark_suite.py and tools/benchmark_scene.py;
process telemetry: tools/process_sampler.py; owned by tools/AGENTS.md.
Native module dependency loading: engine/src/os/platform/jce_library.c;
regression: tests/os/platform/test_jce_library.c and its sibling DLL fixtures.

Public ownership and ignore boundaries: [source-layout.json](source-layout.json).
Public skill: [JCE en-US](../skills/jce/SKILL.md);
[zh-CN translation](../skills/jce-zh-cn/SKILL.md). Locale inventory: [skill-locales.json](skill-locales.json). `docs/`, `.docs/` and `private/`
are local-only and are not required by public source checks or builds.

Shared editor/Player stress fixtures: engine/src/middleware/scene/jce_scene_stress*.c;
public API: engine/include/jce/middleware/scene/jce_scene_stress.h.
Scene animation storage/lifecycle: engine/src/middleware/scene/jce_sr_anim_storage.*
and jce_sr_anim_lifecycle.c own growing generation-keyed instance records and
their retirement. Per-frame evaluation remains in jce_sr_anim.c.
Scene hardware-query history validity: engine/src/middleware/scene/jce_sr_occlusion_history.h.
Shared camera-depth identity and TAA-independent Hi-Z matrix:
engine/src/middleware/scene/jce_sr_depth_history.c.
Optional shared color-model GPU survivor export: engine/src/middleware/scene/jce_sr_gpu_diagnostics.c;
GPU snapshot generation stays in engine/src/renderer/jce_gpu_scene.c and uses
the existing typed asynchronous render-readback queue for CPU transfer.
SDK-only Player host and capture policy: tests/consumers/benchmark/src/main.c.
Stock SDK host effect witnesses: tests/consumers/benchmark/src/default_main.c and
tools/postfx_suite.py, postfx_transition_suite.py and report_postfx.py under
that project. Typed stock target readback: src/default_rt.c; custom host target
sampling: src/rt_witness.c. No renderer implementation is copied there.
Complete frame CSV serialization: engine/src/application/jce_frame_kpi.c;
clock/lifecycle authority remains in jce_engine.c. Native sink regressions live
in tests/application/test_jce_frame_kpi.c.

Normal/albedo shader encoding: engine/shaders/pbr/fs_gbuffer_body.sh; shared
current/previous skin math: vs_gbuffer_skin_body.sh in that directory. Minimal
normal-only and velocity wrappers declare their own complete IO interfaces.
Original Box/Fox normal-buffer controls: tests/consumers/benchmark/tools/gbuffer_asset_suite.py.
Normal-buffer evidence export: tests/consumers/benchmark/tools/report_gbuffer_assets.py.
Root AGENTS.md owns graphical low-end, full single-application and public AI CLI
consumer boundaries; synchronized Skill references provide their procedures.

Global LOD renderer eligibility: engine/src/middleware/scene/jce_sr_draw.c.
SDK global-LOD route witness: tests/consumers/benchmark/src/lod_witness.c/.h and
tests/consumers/benchmark/tools/lod_suite.py (fixture correctness, not streaming).

Global LOD evidence export: tests/consumers/benchmark/tools/report_lod.py.

Clustered-light camera projection: engine/src/renderer/jce_light_cluster.c.
Forward+ fast-path admission: engine/src/middleware/scene/jce_sr_draw.c and
jce_sr_environment.c (unsupported fragment families use existing fallbacks).
Independent light/shadow pixel controls: tests/consumers/benchmark/tools/light_suite.py
and shadow_suite.py; these are consumer correctness fixtures, not full timing.
Light participation evidence export: tests/consumers/benchmark/tools/report_lights.py.
Public Forward+ warm transitions: tests/consumers/benchmark/src/light_transition.c/.h.

Shared directional receiver depth bias: engine/shaders/pbr/shadow_bias.sh.
Static cascade inspection: engine/src/middleware/scene/jce_sr_debug_targets.c.
Cooperative build resource ownership: tools/build/cache_lock.py, integrated
by tools/build/jce.py; original dependency trees remain untouched.

Exact authored camera pose and interactive FPS policy: engine/src/renderer/jce_camera.c;
public contract engine/include/jce/renderer/jce_camera.h. Explicit forward/up
remain authoritative; FPS operations own their pitch limit and safe basis.
Consumer particle diagnostics: tests/consumers/benchmark/src/particle_witness.c and
tests/consumers/benchmark/tools/particle_suite.py; no second particle renderer.

GPU particle free-slot allocation and optional typed population diagnostics:
engine/src/renderer/jce_gpu_particles.c, jce_gpu_particles_internal.h and
engine/shaders/particles/cs_particle_free.sc, cs_particle_stats.sc.
Scene particle limits: engine/src/middleware/scene/jce_sr_particle_limits.h;
public existing debug-target enumeration owns the consumer inspection boundary.
Public SDK load-phase consumers: tests/consumers/benchmark/src/load_witness.c/.h and
async_load_witness.c/.h; synchronous and worker paths share CPU decode helpers.
Rendered residency routes: tests/consumers/benchmark/src/stream_route.c/.h and its
project-owned grid fixture tools; tracked model bytes are not physical VRAM.

CPU particle instance upload arena: engine/src/middleware/scene/jce_sr_particle_batch.c
and jce_sr_particle_batch.h; bounded lazy dynamic fallback and frame-safe offsets.
Pure capacity planning/state: jce_sr_particle_batch_plan.h (no graphics dependency). Scene diagnostics keep simulation and submitted
billboard totals separate in jce_sr_particles.c / jce_sr_debug_targets.c.

Verified single-blob shader compilation cache: tools/shader_cache.py, called by
tools/compile_shaders.cmake; tools/build/cache_lock.py owns kernel coordination.
Native output directories remain isolated and compiler/input bytes own validity.

Early allocator boot policy: engine/src/application/jce_engine_memory_policy.h;
owned by engine/src/application/AGENTS.md. JceEngine owns the resolved mode and
periodic trim clock; tests/application/test_jce_engine_frame_cap.c guards precedence.
Generic editor relocation probe: tools/editor_portability_probe.py; owned by
tools/AGENTS.md. tools/tests/test_editor_portability_probe.py validates complete
frame sequences and explicit bounded run controls without claiming low-end readiness.

Public bundle command: tools/jce_bundle_pack.c; owned by tools/AGENTS.md.
Driver composition: tools/build/bundle.py; owned by tools/build/AGENTS.md.
CPU argument and SDK failure controls: tools/tests/test_bundle_command.py.

Private UTF-8 host-tool bootstrap: tools/cli/jce_tool_main_sdl.c and jce_tool_main.h;
owned by tools/AGENTS.md. It serves bundle/pak/cook/bin2obj. Private
stream bridge tools/cli/jce_tool_io_sdl.c and jce_tool_io.h owns UTF-8 host IO;
directory enumeration and atomic replacement reuse existing public filesystem APIs.

Delivery profile authority: contracts/delivery-profile.json; shared configure
policy: cmake/JCEDeliveryProfile.cmake; selected public content-graph admission:
tools/build/delivery.py, owned by tools/build/AGENTS.md. SDK consumer admission
is exported by cmake/JCEConfig.cmake.in; static native configuration reuses
tools/build/standalone.py without a second dependency recipe.
