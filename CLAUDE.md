# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

JCE ("Java Cat Engine") is a cross-platform game engine consisting of three top-level binaries:

- **`engine/`** — C99 static library (the engine itself), plus optional Java/JNI bridge sources under `engine/java/com/jce/`.
- **`caged_kingdom/`** — C99 sandbox game executable (or shared library when `JCE_BUILD_JNI=ON` / Android).
- **`editor/`** — C++17 desktop-only editor executable (`JCE_Editor` target, output `jce_editor`).

Targets supported: Windows x64/ARM64, Linux x64/ARM64, macOS x64/ARM64/universal, iOS ARM64, Android arm/arm64, WebAssembly. Most cross-targets need a host-built `jce_pak` (and optionally host `shaderc`) — see "Cross-compilation" below.

There is **no test suite** — no `enable_testing()`, no CTest, no gtest/Catch2. Don't fabricate test commands.

## Build system

CMake 3.20+ with Conan 2.x supplies all third-party deps (SDL3, bgfx, flecs, miniaudio, imgui, RmlUi, freetype/harfbuzz, ozz-animation, behaviortree.cpp, box2d, bullet3, enet, protobuf, dav1d, libvpx, opus, zstd, xxhash, mimalloc, physfs, cglm, enkitS, tracy, cgltf, cjson, assimp). Ninja is the standard generator. Compile DB is always exported (`compile_commands.json`).

### Two-step flow (every platform)

1. **Conan install** → produces `<conan-dir>/build/<Cfg>/generators/conan_toolchain.cmake`.
2. **CMake configure + build** referencing that toolchain.

Per-platform Conan profiles live in `conan/profiles/` (e.g. `windows-x64`, `linux-x64`, `linux-arm64`, `macos-arm64`, `android-arm64`, `wasm`, `ios-arm64`, `windows-x64-debug`).

`CMakePresets.json` provides preset configurations matching these profiles (`windows-x64-release`, `windows-x64-debug`, `windows-x64-asan`, `jni-release`, `linux-x64-release`, `macos-arm64-release`, `wasm-release`, etc.). Output dirs follow `build/<class>/<arch>/<variant>/` where `<class>` is `desktop`/`mobile`/`web`/`host`/`jni`.

### Build wrappers

The `scripts/` directory has end-to-end wrappers that handle Conan + CMake + post-build (and host-tool bootstrapping for cross builds):

- Windows (`.bat`): `build-desktop`, `build-editor`, `build-editor-debug`, `build-editor-asan`, `build-windows-arm64`, `build-android`, `build-web`, `build-host-tools`, `package-jni-jar`.
- Linux (`scripts/linux/`): `build-editor.sh`, `build-host-tools.sh`, `build-linux-x64.sh`, `build-linux-arm64.sh`.
- macOS (`scripts/macos/`): `build-editor.sh`, `build-macos-{x64,arm64,universal}.sh`, `build-ios-arm64.sh`, `build-host-tools.sh`.

All scripts accept `--clean` and most accept `--dist`. The Windows scripts call `scripts/lib/jce_finish.bat` for completion beep.

### Build variants

- `JCE_BUILD_VARIANT=release` (default) — normal build with logging, Tracy optional.
- `JCE_BUILD_VARIANT=dist` — distribution build. **Forces** Tracy OFF and patented codecs OFF (royalty-free redistribution), strips logging, emits SHA-256 integrity hash. Windows dist additionally builds `caged_kingdom.exe` as `WIN32_EXECUTABLE` (no console).
- `JCE_BUILD_VARIANT=debug` / `asan` — used by debug/ASan presets only.

Output binaries land in `<build>/release/`, `<build>/dist/`, `<build>/debug/`, etc. — the variant string IS the leaf directory.

### Important CMake-only switches (NOT exposed in `conanfile.py`)

These are intentionally kept out of Conan options to keep `conan install .` legally neutral. Set them with `-D` at configure time:

- `-DJCE_BUILD_JNI=ON` — build shared library for Java (`jce.dll`/`libJCE.so`/`libjce.so`). Editor and standalone exe are skipped.
- `-DJCE_ENABLE_PATENTED_CODECS=ON|OFF` — compile vendored fdk-aac / OpenH264 / libhevc adapters (AAC, H.264, H.265). Default ON for release; dist forces OFF without mutating the user's preference.
- `-DJCE_ENABLE_PROFILING=ON|OFF` — Tracy instrumentation. Default ON; dist forces OFF.
- `-DJCE_ENABLE_CPPCHECK=ON|OFF` — adds a `cppcheck` target that runs as part of `ALL`. Default ON, but Visual Studio generators don't get along with it (use Ninja, or pass `OFF`).
- `-DJCE_ENABLE_ASSET_COOKING=ON|OFF` — runs `jce_cook` to pre-decode raw textures/audio into `.jceasset` containers before packing.
- `-DJCE_SHADER_PROFILES="dx11;spv"` — override auto-detected bgfx backends (default depends on platform: `dx11;spv;glsl` on Windows, `mtl;spv` on Apple, `essl;spv` on Android, `essl` on Web, `spv;glsl` on Linux).
- `-DJCE_PAK_EXECUTABLE=<path>` — **required** when `CMAKE_CROSSCOMPILING` is true. Points to a host-built `jce_pak`.
- `-DJCE_SHADERC_EXECUTABLE=<path>` — host shaderc when cross-compiling.

### Static analysis (cppcheck)

When `JCE_ENABLE_CPPCHECK=ON` and `cppcheck` is on PATH (or in `D:/CMake/Cppcheck`), the build creates a `cppcheck` custom target that runs as part of `ALL` and writes both text and XML reports to `<build>/reports/cppcheck-report.{txt,xml}`. It uses the project compile DB and explicitly excludes `*/third_party/*`, `*/.conan2/*`, and `engine/src/middleware/{audio,video}/third_party`.

## Architecture

### Layered engine (single rule: lower layers never depend on upper)

`engine/CMakeLists.txt` defines per-layer static libs via `_jce_add_layer()`. Public headers all live under `engine/include/jce/` so any layer can `#include <jce/...>`. The aggregate `JCE` target is an `INTERFACE` library that links all layers together, plus the platform-specific frameworks/libs (Metal/Cocoa/IOKit on Apple, EGL/GLESv3/log/android on Android, psapi on Windows, etc.) — downstream targets just `target_link_libraries(... PRIVATE JCE)`.

Layers (and their primary deps):

- **L0 `jce_core`** — `engine/src/os/core/` — allocator, log, json, fs, threads, timers, profiler, handles, i18n, math. Public-links SDL3. Tracy is a compile-time gate via `JCE_TRACY_ENABLED`.
- **L1 `jce_platform`** — `engine/src/os/platform/` — windowing, input, gamepad, host dialogs, single-instance, JNI bridge.
- **L3 middleware** (each depends on `jce_core` only):
  - `jce_video` (`dav1d`, `libvpx`, `webm`, `Opus`; AVX2 enabled per-file for `jce_yuv_convert_avx2.c`)
  - `jce_audio` (`miniaudio`, `Opus`, `Ogg`; depends on `jce_video` for muxed streams; on Apple, `jce_miniaudio_impl.c` is compiled as Objective-C)
  - `jce_physics` (`box2d` + `bullet3`, C++)
  - `jce_animation` (`ozz-animation`, `bgfx`, C++)
  - `jce_ai` (`behaviortree.cpp`, C++)
  - `jce_net` (`enet`, `protobuf`, C++) — owns `engine/proto/jce_net_messages.proto`; protobuf cpp files are generated into `<build>/proto_gen/`.
  - `jce_scene` (`flecs`, `cjson`)
- **L3 graphics** — `jce_renderer` (`bgfx`, `freetype`, `harfbuzz`, SDL3_image; depends on `jce_core`, `jce_platform`, `jce_animation`).
- **L3 UI** — `jce_ui` (`rmlui`, `bgfx`; depends on core/renderer/platform/audio/application — note: links `jce_application` later, intentional design choice for self-contained settings UI).
- **L2 bridge** — `jce_resource` — asset reader/loader/registry/streaming, gltf, shader manager, pak loader, async pool. Public-links most other layers because it loads for them.
- **L5 `jce_application`** — `engine/src/application/` — engine bootstrap (`jce_engine.c`, `jce_main_sdl.c`), config, subsystem registry, screenshot, camera controller. Picks up `JCE_BUILD_JNI=1` define when JNI mode is on.

Patented codec adapters (`engine/src/middleware/{audio,video}/third_party/{fdk-aac,openh264,libhevc}`) are added as `add_subdirectory()` only when `JCE_PATENTED_CODECS_ENABLED` is true; they're a vendored fallback because upstream Conan recipes don't cross-build cleanly to Android/WASM.

### Asset pipeline

1. **`raw_assets/`** (gitignored) — designer inputs.
2. **`tools/jce_cook.c`** → **`jce_cook`** host tool — decodes PNG→RGBA8 / WAV-OGG→PCM s16, writes `.jceasset` containers into `<build>/_cooked/{game,editor}/`. Skipped when cross-compiling. Disabled with `-DJCE_ENABLE_ASSET_COOKING=OFF`.
3. **`tools/jce_pak.c`** → **`jce_pak`** host tool — packs cooked + raw assets + UI (`*.rml`/`*.rcss`) + compiled shaders + `THIRD_PARTY_LICENSES.md` into per-module PAKs (engine, game, editor). Compresses with zstd, hashes with xxhash.
4. **Embedding** — On MSVC, `jce_pak` emits a COFF `.obj` linked directly into the executable. On GCC/Clang, `tools/assets_embed.S.in` is configured into a `.S` file using `.incbin`. PAK locations and embedding glue live in the root `CMakeLists.txt` (search `EDITOR_PAK_OBJ_FILE`, `embedded_assets.h`).
5. **Shaders** — bgfx `shaderc` is invoked via `tools/compile_shaders.cmake` for each profile in `JCE_SHADER_PROFILES`, output to `engine/resources/assets/shaders/<profile>/`. Stale binaries from a previous platform build are excluded from the pack via `--exclude-suffix _<sfx>.bin`.

When cross-compiling, the `_cook_*` and `jce_pak` host steps require pre-built host tools; use `scripts/build-host-tools.{bat,sh}` first, then pass `-DJCE_PAK_EXECUTABLE=<path>` (and `-DJCE_SHADERC_EXECUTABLE=<path>` for shader compilation).

### JNI mode

`JCE_BUILD_JNI=ON` switches `caged_kingdom` from an executable to a SHARED library named `jce` (host) or `JCE` (Android, since the Java side calls `System.loadLibrary("JCE")`). Editor is skipped. Java entry point is `engine/java/com/jce/Main.java`, runtime loader is `JceRuntime.java`. `scripts/package-jni-jar.bat` produces `build/jni/dist/jce-jni.jar`.

## Conventions

- **C99 strict** for engine/game (`CMAKE_C_EXTENSIONS OFF` everywhere except Emscripten). Editor is **C++17 strict**. Don't mix — engine code that needs C++ deps lives behind C wrappers or in C++ TUs that have `LINKER_LANGUAGE CXX` set on the layer.
- Warnings: `jce_target_warnings(<target> [STRICT])` from root `CMakeLists.txt`. Engine layers all use `STRICT` (`-Wpedantic -Wconversion -Wshadow -Wdouble-promotion`, MSVC `/W4 /utf-8 /Zc:preprocessor`). Don't suppress without justification.
- Public include root is always `engine/include/`; new headers go under `engine/include/jce/<layer>/...`. Cross-layer access to peer `engine/src/` directories is allowed via the `_ENGINE_SRC` private include — but prefer public APIs.
- Vendored single-header libs live at `engine/src/<layer>/internal/`. The stb_image vendor (`engine/src/renderer/internal/stb_image.h`) is explicitly excluded from style/CI scans via `.gitignore`/`.gitattributes`.
- The git short hash and timezoned UTC build timestamp are baked into `jce_version.h` at configure time (regenerated each CMake configure).
- Don't add patented codec or JNI options to `conanfile.py` — those switches are CMake-only by policy (legal neutrality of `conan install .`). The matching note is in `conanfile.py` itself.
- `Protobuf_PROTOC_EXECUTABLE` is force-overridden in the root `CMakeLists.txt` to use the Conan-shipped protoc (with cross-compile fallback that searches `~/.conan2/p/proto*/p/bin/` for a host-runnable `protoc.exe`). Don't rely on a system/conda protoc.

## Common pitfalls

- **Re-running cmake after switching profiles**: pass `--clean` to the build script, or delete the matching `build/<class>/<arch>{,-conan}` dirs. Mixing toolchains in the same build dir corrupts CMake state.
- **`cppcheck` target fails under Visual Studio generator**: configure with `-DJCE_ENABLE_CPPCHECK=OFF`, or switch to Ninja.
- **`jni.h` not found in editor IDE**: ensure `.vscode/c_cpp_properties.json` includes `openjdk-8/include` and `openjdk-8/include/win32`, then reload.
- **Stale shader binaries leaking into PAK**: trust `JCE_PAK_SHADER_EXCLUDE_FLAGS` — don't manually copy shader output between platform builds.
- **Apple frameworks linker errors on iOS bgfx**: handled in `conanfile.py:generate()` by stripping `Cocoa`/`IOKit` from the bgfx component before generation.
