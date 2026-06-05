# JCE — Java Cat Engine

Cross-platform, data-driven, native-callable game engine in C99 with a C++17 ImGui editor.

**Targets**: Windows · macOS · Linux · Android · iOS · WASM · consoles (planned) — x86_64 / arm / arm64.
Baseline: single-core, 512 MB RAM, no discrete GPU (~2010-class hardware).

## Design Pillars

1. **Cross-platform first** — anything that runs on the baseline scales up.
2. **Native-callable from any language** — flat C API; editor and JNI bridge are *consumers*, not core.
3. **Data-driven ECS** — scenes, prefabs, and components are pure data; no game logic in the engine.
4. **Thin, sanctioned dependencies** — every dep is load-bearing; platform APIs only through OS wrappers.
5. **High cohesion, low coupling** — one TU, one job. No god files.

## Layered Model

``````
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
│ L1 — Compat:       jce_compat.h · jce_defs.h · jce_version.h│
└──────────────────────────────────────────────────────────────┘
``````

**Rule**: lower layers never `#include` upper layers — enforced at review.

## Public API

`engine/include/jce/api.h` is the single entry point. Per-layer umbrellas:

| Header                   | Layer | Covers |
|--------------------------|-------|--------|
| `<jce/api_core.h>`       | L1+L2 | alloc, log, math, fs, threads, time, profiler |
| `<jce/api_platform.h>`   | L2    | window, input, gamepad, clipboard, dialog, watch |
| `<jce/api_graphics.h>`   | L3    | bgfx wrapper, textures, shaders |
| `<jce/api_render.h>`     | L3    | render graph, scene renderer, lighting, postfx |
| `<jce/api_animation.h>`  | L4    | skeleton, blend tree, state machine, IK |
| `<jce/api_audio.h>`      | L4    | mixer, decoder, occlusion, reverb |
| `<jce/api_physics.h>`    | L4    | Bullet wrapper, collider components |
| `<jce/api_ai.h>`         | L4    | navmesh, behaviour tree, steering |
| `<jce/api_scene.h>`      | L4    | ECS components, prefab, sequencer, terrain |
| `<jce/api_resource.h>`   | L4    | asset loader, pak, glTF, image decode |
| `<jce/api_streaming.h>`  | L4    | world streamer |
| `<jce/api_net.h>`        | L4    | snapshot, replication |
| `<jce/api_ui.h>`         | L4    | RmlUI, HUD, settings |
| `<jce/api_app.h>`        | L6    | engine descriptor, app interface, screenshot |
| `<jce/api_middleware.h>` | L4    | aggregate of all middleware umbrellas |

## Middleware (Layer 4)

| Module    | One-liner |
|-----------|-----------|
| ai        | Recast/Detour navmesh, behaviour tree, steering |
| animation | Skeletal blend tree, IK, state machine |
| audio     | miniaudio mixer + AAC/M4A/Opus decode + reverb zones |
| net       | Snapshot + replication primitives |
| physics   | Bullet 3D + Box2D-style 2D |
| save      | Save-game / snapshot persistence |
| scene     | ECS, prefab, terrain, sequencer, vcam, LOD |
| ui        | RmlUI HTML/CSS + game HUD + debug HUD |
| video     | MP4/WebM + AV1/H.264/H.265/VP8 codecs |
| world     | Roads, spawners, weapons, trigger volumes |

## Repository Layout

``````
JCE/
├── engine/
│   ├── include/jce/         # api*.h + os/ + renderer/ + middleware/ + runtime/ + application/
│   ├── src/                 # mirrors include/ layout
│   ├── shaders/             # bgfx shaders
│   ├── resources/assets/
│   ├── proto/
│   ├── ui/
│   └── java/com/jce/        # JNI bindings
├── caged_kingdom/           # sandbox game
├── editor/                  # C++17 ImGui editor
├── tools/                   # asset packer, code-gen, build tools
├── conan/profiles/
├── scripts/
└── docs/
``````

## Dependencies

New direct deps require owner approval + `THIRD_PARTY_LICENSES.md` entry.

| Dep             | Role                              | Layer                 |
|-----------------|-----------------------------------|-----------------------|
| SDL3            | Window / input / platform         | os/platform           |
| bgfx            | Graphics backend abstraction      | renderer              |
| PhysFS          | Virtual filesystem / archive I/O  | os/core               |
| enkiTS          | Task scheduler                    | os/core               |
| mimalloc        | Allocator                         | os/core               |
| Tracy           | Profiling                         | os/core               |
| flecs           | Data-driven ECS                   | scene / renderer / ui |
| Recast & Detour | Navmesh + pathfinding             | middleware/ai         |
| miniaudio       | Audio device + mixing             | middleware/audio      |
| Bullet          | 3D rigid-body physics             | middleware/physics    |
| RmlUI           | HTML/CSS in-game HUD              | middleware/ui         |
| ImGui           | Editor UI only                    | editor                |
| cJSON           | JSON parsing                      | os/core               |
| stb_image       | Image decode                      | resource              |

Heavy codecs (fdk-aac, libhevc, openh264) are not committed — pulled on demand into
`engine/src/middleware/{audio,video}/third_party/` (gitignored).

## Conventions

| Topic | Rule |
|-------|------|
| Engine language | C99. No C++, exceptions, or RTTI. |
| Editor language | C++17. ImGui only — no SDL/bgfx in panel files. |
| Naming | `jce_<module>_<verb>()` · `JceXxx` types · `JCE_UPPER` macros |
| Platform symbols | `_WIN32`, `<windows.h>`, `<unistd.h>` etc. **forbidden** in first-party source — use `os/platform` wrappers. |
| File size | Soft cap ~2000 lines; hard cap ~3000 triggers refactor. |

## Where to Look First

| Task | Location |
|------|----------|
| Add a public engine call | `engine/include/jce/api*.h` → matching layer `src/` |
| Add an ECS component | `engine/include/jce/middleware/scene/jce_scene.h` + serializer + inspector drawer |
| Add a renderer pass | `engine/src/renderer/jce_scene_renderer.c` |
| Add an editor panel | `editor/src/panels/` → register in `jce_editor_panels.cpp` → dock in `jce_editor_layout.cpp` |
| Add a hotkey | `editor/src/core/jce_editor_hotkeys.*` |
| Localize a string | `editor/src/core/jce_editor_i18n.cpp` |

---

# Build Guide (Windows)

All commands are PowerShell from the repo root. Prerequisites: **VS 2022** (Desktop C++), **CMake 3.20+**, **Conan 2.x**, **Java 8+ JDK**. Ninja is recommended.

## A. Standalone Game

``````powershell
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
``````

Or use the standardized scripts (thin wrappers over `scripts/jce.py`): `./scripts/windows/build-editor.bat` (editor) or `./scripts/windows/build-project.bat caged_kingdom` (game, via the SDK). Add `--dist` (shorthand for `--variant dist`) for a distribution build.

## B. Editor

``````powershell
./scripts/windows/build-editor.bat                 # release
./scripts/windows/build-editor.bat --dist          # distribution (== --variant dist)
# (Linux/macOS: scripts/linux|macos/build-editor.sh, same flags)
``````

Output: `build/desktop/windows-x64/release/jce_editor.exe`

## C. Java + JNI

``````powershell
# 1. Dependencies
conan install . --output-folder=build/jni/desktop-conan --build=missing `
    -o "&:jce_jni=True" -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64

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
``````

## D. Cross-Compilation

``````powershell
./scripts/build-android.bat [ndk_path] [sdk_path] [arch] [--clean]
./scripts/windows/build-editor.bat --arch arm64 [--dist] [--clean]   # Windows ARM64
./scripts/build-web.bat [emsdk_path] [--clean] [--dist]
``````

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