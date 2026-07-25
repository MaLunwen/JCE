# JCE — Java Cat Engine

Cross-platform, data-driven, native-callable game engine with a C99 first-party core, a stable C ABI, privately encapsulated C++ dependencies, and a C++20 ImGui editor.

**Targets**: Windows · macOS · Linux · Android · iOS · WASM · consoles (planned) — x86_64 / arm / arm64.

**Core/headless baseline**: single-core CPU, 512 MB RAM, no discrete GPU (~2010-class hardware). The editor and rendered games use higher, tiered requirements according to workload and enabled features.

## Design Pillars

1. **Cross-platform first** — anything that runs on the baseline scales up.
2. **Native-callable from any language** — flat C API; editor and JNI bridge are *consumers*, not core.
3. **Data-driven ECS** — scenes, prefabs, and components are pure data; no game logic in the engine.
4. **Thin, sanctioned dependencies** — every dep is load-bearing; platform APIs only through OS wrappers.
5. **High cohesion, low coupling** — one TU, one job. No god files.

## Hardware Baselines

The 512 MB / single-core / no-discrete-GPU target applies to the **core and headless services**, not to every JCE consumer.

| Tier                   | Intended use                                                                       | Baseline                                                        |
| ---------------------- | ---------------------------------------------------------------------------------- | --------------------------------------------------------------- |
| Core / headless        | Dedicated server, tools, simulation services, non-rendering runtime                | Single-core CPU, 512 MB RAM, no discrete GPU                    |
| Lightweight runtime    | Simple 2D/3D games and embedded interactive applications                           | Integrated graphics and workload-dependent memory               |
| Editor                 | Full asset import, inspectors, previews, profiling, and authoring workflows        | Multi-core CPU, modern graphics API support, and additional RAM |
| Advanced rendered game | Open worlds, high entity counts, advanced lighting, post-processing, and streaming | Project-defined minimum and recommended tiers                   |

The low baseline is a compatibility and efficiency target. It must not prevent the editor or games from scaling up to modern hardware.

## Layered Model

```
┌──────────────────────────────────────────────────────────────┐
│ L7 — Consumers: caged_kingdom/, editor/, engine/java/        │
├──────────────────────────────────────────────────────────────┤
│ L6 — Application:  engine/src/application/  (app loop)       │
├──────────────────────────────────────────────────────────────┤
│ L5 — Runtime:      engine/src/runtime/      (editor↔game)    │
├──────────────────────────────────────────────────────────────┤
│ L4 — Middleware:   ai · animation · audio · net · physics    │
│                    save · scene · ui · video · world         │
├──────────────────────────────────────────────────────────────┤
│ L3 — Renderer:     engine/src/renderer/  (bgfx, RenderGraph) │
├──────────────────────────────────────────────────────────────┤
│ L2 — OS:           engine/src/os/{core,platform}             │
│                    alloc · log · math · fs · threads · input │
├──────────────────────────────────────────────────────────────┤
│ L1 — Compat:       jce_compat.h · jce_defs.h · jce_version.h │
└──────────────────────────────────────────────────────────────┘
```

**Rule**: lower layers never `#include` upper layers — enforced at review.

## Public API

`engine/include/jce/api.h` is the single entry point. Per-layer umbrellas:

| Header                   | Layer | Covers                                          |
| ------------------------ | ----- | ----------------------------------------------- |
| `<jce/api_core.h>`       | L1+L2 | alloc, log, math, fs, threads, time, profiler   |
| `<jce/api_platform.h>`   | L2    | window, input, gamepad, clipboard, dialog, watch |
| `<jce/api_graphics.h>`   | L3    | bgfx wrapper, textures, shaders                 |
| `<jce/api_render.h>`     | L3    | render graph, scene renderer, lighting, postfx  |
| `<jce/api_animation.h>`  | L4    | skeleton, blend tree, state machine, IK         |
| `<jce/api_audio.h>`      | L4    | mixer, decoder, occlusion, reverb               |
| `<jce/api_physics.h>`    | L4    | Bullet wrapper, collider components             |
| `<jce/api_ai.h>`         | L4    | navmesh, behaviour tree, steering               |
| `<jce/api_scene.h>`      | L4    | ECS components, prefab, sequencer, terrain      |
| `<jce/api_resource.h>`   | L4    | asset loader, pak, glTF, image decode           |
| `<jce/api_streaming.h>`  | L4    | world streamer                                  |
| `<jce/api_net.h>`        | L4    | snapshot, replication                           |
| `<jce/api_ui.h>`         | L4    | RmlUI, HUD, settings                            |
| `<jce/api_app.h>`        | L6    | engine descriptor, app interface, screenshot    |
| `<jce/api_middleware.h>` | L4    | aggregate of all middleware umbrellas           |

## Dependency Ownership

Every capability below has exactly one authoritative owner. The tables are the
human-readable form of `docs/architecture/dependency-ownership.yml`, which
`tools/audit/check_dependency_boundaries.py` enforces.

### Core, Platform, and Foundations

| Capability                                                | Authoritative owner                                     | Allowed scope / helpers                                                             | Do not use as an alternative                                                          |
| --------------------------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| Window, events, input, gamepad, clipboard, host dialogs   | **SDL 3.4.0**, behind `jce_window_*` / `jce_input_*` / `jce_host_*` | `engine/src/os/` plus the application layer's SDL_main callbacks and event pump      | Raw Win32/Cocoa/X11 windowing, or SDL Renderer                                        |
| Portable C-runtime substrate                              | **SDL 3.4.0** utility headers                           | Sanctioned engine-wide as a portable libc — see `scripts/lint/check_engine_native_io.py` | Platform-specific stdio/threading shims added per module                              |
| Entity storage, queries, systems, component metadata      | **flecs 4.1.5**, behind `JceEntity` + `jce_scene_*`     | `ecs_*` stays private to the engine implementation                                  | A second entity registry, `ecs_*` in public headers, or flecs in `editor/` and games  |
| Virtual filesystem and archive mounts                     | **PhysFS 3.2.0**, behind the JCE VFS/resource path API  | All resource access resolves through normalized virtual paths                       | `fopen`/`std::filesystem` for resource access, or a second archive system             |
| Host/editor filesystem access                             | **JCE OS filesystem adapter** (first-party, SDL-backed) | `jce_fs_host_*` / `SDL_IOStream`, confined to `engine/src/os/`                       | Raw `fopen`/`CreateFile` outside `os/`, or host paths used as resource IDs            |
| General allocation                                        | **mimalloc 3.3.2**, behind `JCE_MALLOC`/`jce_alloc` and `ED_ALLOC` | Editor and engine each use their own wrapper family                                  | Untracked `malloc`/`new`, or freeing across allocators                                |
| Task scheduling and worker threads                        | **enkiTS 1.11**, behind `jce_thread_pool_*`             | Blocking work uses a private pool; the frame pool stays non-blocking                | Per-module thread pools, `std::async`, or detached threads                            |
| HTTP/HTTPS transport                                      | **libcurl 8.21.0**, behind the JCE HTTP service policy  | The ai_dispatch transport; native TLS (schannel) on Windows                          | Handwritten HTTP or TLS                                                               |
| Profiling and instrumentation                             | **Tracy 0.13.1**, behind the JCE profiling macros       | `on_demand=True`; lightweight counters/logging may coexist; profiling must compile out cleanly | A second event profiler or direct Tracy calls spread through public/game APIs         |
| Linux Wayland dependency resolution                       | **wayland 1.24.0** as a transitive Conan override       | Dependency graph compatibility only                                                 | Direct first-party Wayland usage outside the SDL/JCE platform backend                 |

### Rendering, Images, Fonts, UI, and Assets

| Capability                                                                                       | Authoritative owner                                            | Allowed scope / helpers                                                                                             | Do not use as an alternative                                                                                           |
| ------------------------------------------------------------------------------------------------ | -------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| Graphics backend abstraction, GPU resources, command submission, backend selection               | **bgfx 1.146.9306-550**, behind JCE graphics/render APIs | SDL supplies the native window handle; RenderGraph, material, lighting, and post-processing policy remain JCE-owned | SDL Renderer, raw OpenGL/Vulkan/D3D/Metal/WebGPU calls, or bgfx handles/types in public APIs and game code             |
| Shader compilation                                                                               | **bgfx shaderc** plus JCE shader build pipeline          | Host `shaderc` is supplied separately for cross-compilation; generated binaries are packaged as assets             | Runtime source compilation with unrelated compilers or per-project shader build scripts that bypass JCE                |
| Common runtime image formats                                                                     | **SDL_image 3.4.0**, behind the JCE image loader         | Normal LDR textures and formats supported by the configured SDL_image build                                         | Calling SDL_image directly from scene/game code or adding another general-purpose image loader                         |
| HDR and lightweight environment/skybox image decode                                              | **stb_image 2.30**, behind the same JCE image loader     | HDR, floating-point environment maps, and explicit cases assigned by the image-dispatch layer                       | Using stb_image as an uncontrolled second general loader or selecting SDL_image/stb_image directly at call sites       |
| Font file parsing, glyph metrics, outlines, rasterization                                        | **FreeType 2.14.3**                                      | JCE font cache/atlas owns GPU upload and lifetime                                                                   | Independent font rasterizers or direct FreeType state in game code                                                     |
| Text shaping and glyph sequencing                                                                | **HarfBuzz 12.3.0**                                      | Works with FreeType through the JCE text system; line breaking, fallback policy, and layout remain JCE-owned        | Handwritten shaping, one-codepoint-one-glyph assumptions, or bypassing shaping for supported complex scripts           |
| Editor interface                                                                                 | **Dear ImGui 1.92.6-docking**                            | Direct ImGui usage is permitted only in `editor/` and editor integration code                                      | ImGui for shipped game HUD/UI, or SDL/bgfx calls inside individual editor panel files                                  |
| Runtime/game interface                                                                           | **RmlUI 6.2**, behind `jce_ui_*`                       | Game HUD, menus, settings, documents, styles, and runtime debug UI                                                  | Shipping ImGui as game UI or creating a parallel retained-mode UI framework                                            |
| Canonical glTF/GLB parsing                                                                       | **cgltf 1.15**                                           | Primary glTF path for runtime and import tools; JCE converts parsed data into engine-owned assets                   | Parsing glTF by hand or routing ordinary glTF through multiple importers with different semantics                      |
| Non-glTF source asset import                                                                     | **Assimp 6.0.2**, primarily editor/offline tools         | Fallback importer for supported authoring formats, followed by conversion into JCE canonical data                   | Using Assimp as a competing runtime scene system or as the default glTF path without a documented compatibility reason |
| Mesh remapping, vertex/index optimization, simplification, LOD generation, mesh codec operations | **meshoptimizer 1.0**                                    | Invoked by the asset pipeline and selected runtime decode paths                                                     | Handwritten mesh optimizers or different LOD generation algorithms in individual projects                              |
| Convex decomposition for imported collision geometry                                             | **V-HACD 4.1.0**                                         | Offline/editor import step; generated hulls are stored as engine assets                                             | Expensive runtime decomposition during normal play or project-specific convex decomposition implementations            |

The image loader is a single JCE service. Callers request an image by usage/format; the service dispatches internally to SDL_image or stb_image. Callers must never choose the decoder themselves.

### Animation, AI, Physics, and Scripting

| Capability                                                                               | Authoritative owner                                                | Allowed scope / helpers                                                                                                                                            | Do not use as an alternative                                                                                                       |
| ---------------------------------------------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------- |
| Runtime skeletal animation sampling and optimized animation data                         | **ozz-animation 0.14.1**                                     | Importers extract source skeleton/clip data; JCE converters produce runtime animation assets; JCE owns state machines, blend trees, IK policy, and ECS integration | A second skeletal sampler, per-project bone evaluators, or Assimp-driven runtime animation playback                                |
| Behaviour-tree execution                                                                 | **BehaviorTree.CPP 4.9.0**, behind `jce_ai_*`              | JCE/editor owns node registration, serialization, blackboard conventions, and engine bindings                                                                      | A second behaviour-tree runtime or game-specific tree interpreter                                                                  |
| Navmesh generation, tiled navigation, path queries, crowd primitives                     | **Recast & Detour 1.6.0**, behind `jce_ai_*`               | JCE world streaming owns navmesh tile lifetime; higher-level steering/decision logic may use query results                                                         | Handwritten A* over world geometry, a second navmesh representation, or direct Detour objects in game code                         |
| 3D rigid bodies, collision detection, constraints, character/vehicle physics integration | **Bullet 3.25**, behind `jce_physics_*`                    | JCE defines components, handles, layers, determinism policy, threading policy, serialization, and world lifetime                                                   | A second 3D physics engine, custom broadphase, or Bullet types crossing the C ABI                                                  |
| 2D rigid bodies and collision                                                            | **Box2D 3.1.1**, behind `jce_physics2d_*`                  | Used only for 2D worlds/components; shared JCE collision-layer conventions may be reused                                                                           | Emulating 2D physics with Bullet without an explicit project decision, mixing one body between both engines, or a second 2D solver |
| Embedded gameplay scripting                                                              | **Lua 5.4.8**, behind the language-neutral JCE binding model | Lua is one consumer of the stable C ABI/reflection schema; C++, Java/JNI, and future languages must expose equivalent engine concepts                              | Lua-only engine features, scripts directly owning engine memory, or separately designed APIs that drift from the C ABI             |

### Audio and Video

| Capability                                                                             | Authoritative owner                                        | Allowed scope / helpers                                                   | Do not use as an alternative                                                                                            |
| -------------------------------------------------------------------------------------- | ---------------------------------------------------------- | ------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| Audio device, playback graph, mixing, spatialization, streaming, engine audio lifetime | **miniaudio 0.11.22**, behind `jce_audio_*`        | Specialized codec modules feed decoded PCM/streams into miniaudio         | SDL audio, OpenAL, platform audio APIs, or a second mixer in first-party code                                           |
| Opus audio encode/decode                                                               | **Opus 1.5.2**                                       | Voice, network audio, and Opus assets/streams as defined by JCE           | Handwritten Opus wrappers at call sites or an alternate Opus decoder                                                    |
| Ogg container parsing/writing                                                          | **libogg 1.3.5**                                     | Ogg framing for codecs such as Opus                                       | Handwritten Ogg page parsing or treating Ogg as a codec                                                                 |
| AV1 video decode                                                                       | **dav1d 1.5.3**                                      | Video module supplies decoded frames to the renderer                      | A second AV1 software decoder in the same build                                                                         |
| VP8/VP9 encode/decode                                                                  | **libvpx 1.16.0**                                    | WebM video paths and explicitly supported encode/decode workflows         | Platform-specific VP8/VP9 paths with different behavior unless selected behind the video backend                        |
| WebM container demux/mux                                                               | **libwebm 1.0.0.31**                                 | Container handling; codec payloads go to dav1d/libvpx/Opus as appropriate | Handwritten EBML/WebM parsing or assuming libwebm itself decodes media                                                  |
| Patent-sensitive AAC/H.264/H.265 paths                                                 | **Optional JCE codec modules**, disabled in `dist` | Enabled only by explicit CMake switches and license review                | Silent enablement through Conan, automatic fallback to patented codecs, or shipping them in royalty-free `dist` builds |

### Networking and Serialization

| Capability                                                   | Authoritative owner                                   | Allowed scope / helpers                                                                                                                                                          | Do not use as an alternative                                                                                                          |
| ------------------------------------------------------------ | ----------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| Low-latency UDP transport, channels, sequencing, reliability | **ENet 1.3.18**, behind `jce_net_transport_*` | JCE adds connection policy, encryption/authentication integration, replication, metrics, and platform adaptation                                                                 | A second reliable-UDP implementation, raw UDP in gameplay code, or exposing `ENetPeer*` through the public ABI                       |
| Schema-defined binary messages and generated bindings        | **Protocol Buffers 7.35.0**                     | Handshake, control/RPC messages, tool interchange, metadata, and other schema-oriented data                                                                                      | Handwritten tagged-message parsers or incompatible per-language message definitions                                                   |
| High-frequency realtime snapshots                            | **JCE network bitstream/replication codec**     | The only sanctioned custom binary path; may use quantization, delta compression, bit packing, and xxHash where appropriate; format is versioned and tested inside the net module | General-purpose custom serializers scattered across components or using Protobuf objects directly for every per-frame entity snapshot |
| Human-editable serialized data                               | **cJSON through JCE JSON APIs**                 | Scenes, prefabs, materials, settings, manifests, and localization                                                                                                                | Using Protobuf merely to replace editable project JSON, or handwritten JSON                                                           |
| Compressed binary payloads                                   | **zstd through JCE compression APIs**           | Saves, network blobs when beneficial, caches, and packages according to format policy                                                                                            | Codec selection at arbitrary call sites or undocumented compressed blobs                                                              |

### Canonical Data and Path Rules

| Data kind                        | Canonical mechanism                                                                                                          |
| -------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| Public/runtime path              | Normalized UTF-8 **virtual path** resolved by the JCE VFS/PhysFS layer                                                  |
| Host/editor import path          | Opaque/normalized **host path** handled only by the JCE OS filesystem adapter, then imported into virtual project space |
| Human-authored structured text   | JCE JSON API backed by cJSON                                                                                                 |
| Typed cross-process/tool message | Protobuf schema and generated code                                                                                           |
| Realtime replicated state        | Versioned JCE snapshot/bitstream codec                                                                                       |
| General compressed payload       | JCE compression API backed by zstd                                                                                           |
| Asset/cache identity             | JCE hash API backed by xxHash; collision handling is mandatory for persisted identifiers                                     |
| Runtime entity/component state   | flecs world accessed through JCE scene/runtime APIs                                                                          |
| Render resource identity         | JCE generation-checked handles; never raw bgfx handles outside the renderer                                                  |

### Header and Call-Site Boundaries

- Public headers under `engine/include/jce/` expose only C-compatible JCE types.
- Third-party headers belong in the owning module's private implementation or bridge translation units.
- Game projects must not include SDL, bgfx, PhysFS, flecs, cJSON, Bullet, Box2D, ENet, Protobuf runtime internals, FreeType, HarfBuzz, miniaudio, or other engine dependencies directly.
- The editor may include ImGui directly, but panels still call JCE services for rendering, assets, filesystem, ECS, undo/redo, and runtime operations.
- Offline tools may use C++ standard-library conveniences internally, but project path semantics, asset formats, IDs, compression, and serialization must still go through the same JCE-owned policies.
- A dependency exception requires: documented reason, exact owning module, public-boundary impact analysis, tests, license entry, and a migration/removal plan when it duplicates an existing owner.

## Conan Dependency Inventory

The following direct requirements are authoritative for the current dependency graph:

| Dependency                | Assigned responsibility                                                  |
| ------------------------- | ------------------------------------------------------------------------ |
| SDL3 3.4.0                | Platform/window/input/event foundation                                   |
| SDL_image 3.4.0           | Common runtime image decoding                                            |
| bgfx 1.146.9306-550       | Complete graphics backend abstraction and shader toolchain               |
| HarfBuzz 12.3.0           | Text shaping                                                             |
| FreeType 2.14.3           | Font parsing and glyph rasterization                                     |
| miniaudio 0.11.22         | Audio device, mixing, playback, and spatial audio                        |
| Opus 1.5.2                | Opus audio codec                                                         |
| libogg 1.3.5              | Ogg container framing                                                    |
| dav1d 1.5.3               | AV1 video decoding                                                       |
| libvpx 1.16.0             | VP8/VP9 codec                                                            |
| libwebm 1.0.0.31          | WebM container                                                           |
| Dear ImGui 1.92.6-docking | Editor UI only                                                           |
| flecs 4.1.5               | ECS storage, queries, systems, and metadata foundation                   |
| cJSON 1.7.19              | JSON DOM parsing/writing                                                 |
| Assimp 6.0.2              | Non-glTF offline/editor source import fallback                           |
| cgltf 1.15                | Canonical glTF/GLB parser                                                |
| meshoptimizer 1.0         | Mesh optimization, simplification, LOD, and mesh codecs                  |
| V-HACD 4.1.0              | Offline convex decomposition                                             |
| ozz-animation 0.14.1      | Runtime skeletal animation data and sampling                             |
| BehaviorTree.CPP 4.9.0    | Behaviour-tree execution                                                 |
| Recast Navigation 1.6.0   | Navmesh generation and pathfinding                                       |
| Box2D 3.1.1               | 2D physics                                                               |
| Bullet 3.25               | 3D physics                                                               |
| Lua 5.4.8                 | Built-in scripting language consumer                                     |
| RmlUI 6.2                 | Runtime/game UI                                                          |
| PhysFS 3.2.0              | Virtual filesystem and archive mounts                                    |
| zstd 1.5.7                | General compression                                                      |
| xxHash 0.8.3              | Fast non-cryptographic hashing                                           |
| mimalloc 3.3.2            | General engine allocator                                                 |
| enkiTS 1.11               | Job scheduler and worker pool                                            |
| libcurl 8.21.0            | HTTP/HTTPS transport                                                     |
| ENet 1.3.18               | Realtime reliable UDP transport                                          |
| Protocol Buffers 7.35.0   | Schema-defined messages and generated bindings                           |
| Tracy 0.13.1              | Profiling and instrumentation                                            |
| Wayland 1.24.0            | Linux transitive version override; not a direct first-party platform API |

`stb_image 2.30` is vendored rather than declared as a Conan direct requirement. It remains an approved, narrowly scoped helper for HDR/environment decoding behind the JCE image service.

## Conventions

| Topic            | Rule                                                                                                                                                                                                      |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Engine language  | First-party runtime core and public ABI use C99. Private C++ dependencies are allowed only behind C-compatible wrappers; C++ types, exceptions, RTTI, and STL containers must never cross the public ABI. |
| Editor language  | C++20. ImGui only — no SDL/bgfx in panel files.                                                                                                                                                          |
| Naming           | `jce_<module>_<verb>()` · `JceXxx` types · `JCE_UPPER` macros                                                                                                                                     |
| Platform symbols | `_WIN32`, `<windows.h>`, `<unistd.h>` etc. **forbidden** in first-party source — use `os/platform` wrappers.                                                                               |
| File size        | Soft cap ~2000 lines; hard cap ~3000 triggers refactor.                                                                                                                                                   |

## Where to Look First

| Task                     | Location                                                                                             |
| ------------------------ | ---------------------------------------------------------------------------------------------------- |
| Add a public engine call | `engine/include/jce/api*.h` → matching layer `src/`                                             |
| Add an ECS component     | `engine/include/jce/middleware/scene/jce_scene.h` + serializer + inspector drawer                  |
| Add a renderer pass      | `engine/src/renderer/jce_scene_renderer.c`                                                         |
| Add an editor panel      | `editor/src/panels/` → register in `jce_editor_panels.cpp` → dock in `jce_editor_layout.cpp` |
| Add a hotkey             | `editor/src/core/jce_editor_hotkeys.*`                                                             |
| Localize a string        | `editor/src/core/jce_editor_i18n.cpp`                                                              |

---

# Build Guide (Windows)

All commands are PowerShell from the repo root. Prerequisites: **VS 2022** (Desktop C++), **CMake 3.20+**, **Conan 2.x**, **Java 8+ JDK**. Ninja is recommended.

## A. Standalone Game

```powershell
# 1. Dependencies
conan install . --output-folder=build/desktop/windows-x64-conan --build=missing `
    -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64

# 2. Configure
cmake -S . -B build/desktop/windows-x64 -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=build/desktop/windows-x64-conan/build/Release/generators/conan_toolchain.cmake `
    -DCMAKE_BUILD_TYPE=Release -DJCE_BUILD_VARIANT=release

# 3. Build & run
cmake --build build/desktop/windows-x64
./build/desktop/windows-x64/release/caged_kingdom.exe
```

Or use the standardized scripts (thin wrappers over `scripts/jce.py`): `./scripts/windows/build-editor.bat` (editor) or `./scripts/windows/build-project.bat caged_kingdom` (game, via the SDK). Add `--dist` (shorthand for `--variant dist`) for a distribution build.

## B. Editor

```powershell
./scripts/windows/build-editor.bat                 # release
./scripts/windows/build-editor.bat --dist          # distribution (== --variant dist)
# (Linux/macOS: scripts/linux|macos/build-editor.sh, same flags)
```

Output: `build/desktop/windows-x64/release/jce_editor.exe`

## C. Java + JNI

```powershell
# 1. Dependencies
# JNI is a CMake-only switch; Conan intentionally has no jce_jni option.
conan install . --output-folder=build/jni/desktop-conan --build=missing `
    -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64

# 2. Configure + build
cmake -S . -B build/jni/desktop -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=build/jni/desktop-conan/build/Release/generators/conan_toolchain.cmake `
    -DJCE_BUILD_JNI=ON -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=release
cmake --build build/jni/desktop
# Output: build/jni/desktop/caged_kingdom/jce.dll

# 3. Compile Java
New-Item -ItemType Directory -Force -Path build/jni/desktop/java-out | Out-Null
javac -d build/jni/desktop/java-out `
    engine/java/com/jce/JceRuntime.java engine/java/com/jce/Main.java

# 4. Run
java "-Djava.library.path=build/jni/desktop/caged_kingdom" -cp build/jni/desktop/java-out com.jce.Main

# Or package a runnable JAR
./scripts/package-jni-jar.bat        # Output: build/jni/dist/jce-jni.jar
```

## D. Cross-Compilation

```powershell
./scripts/build-android.bat [ndk_path] [sdk_path] [arch] [--clean]
./scripts/windows/build-editor.bat --arch arm64 [--dist] [--clean]   # Windows ARM64
./scripts/build-web.bat [emsdk_path] [--clean] [--dist]
```

## Common Issues

**`jni.h` not found** — add `openjdk-8/include` and `openjdk-8/include/win32` to
`.vscode/c_cpp_properties.json`, then run `C/C++: Rescan Workspace`.

**`cppcheck` fails in VS generator** — configure with `-DJCE_ENABLE_CPPCHECK=OFF`
or switch to Ninja.

**Shader compile disabled / shaderc not found** — CMake warns instead of failing so
a pre-baked pak still links the editor. To re-enable shader builds, point CMake or your
shell at a host shaderc binary via **either** mechanism (env takes priority):

```powershell
# Option A — environment variable (CI-friendly, no CMake reconfigure needed)
$env:JCE_SHADERC_EXECUTABLE = "C:\path\to\shaderc.exe"
cmake --build ...

# Option B — CMake cache variable
cmake ... -DJCE_SHADERC_EXECUTABLE="C:\path\to\shaderc.exe"
```

The native Windows build produces `shaderc.exe` under the bgfx Conan package folder
and is picked up automatically when `bgfx` is built with `tools=True` (the default).
