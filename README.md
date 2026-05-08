# JCE — Java Cat Engine

A cross-platform, data-driven, native-callable game engine written in C99
with a C++17 ImGui editor on top.

- **Cross-platform**: Windows, macOS, Linux, Android, iOS, Web (WASM),
  consoles (planned). x86_64 / arm / arm64.
- **Cross-architecture, cross-vintage**: baseline target is a single-core,
  512 MB, no-discrete-GPU machine (~2010-class hardware).
- **Native-callable from any language**: engine ships as a flat C API
  (`<jce/api.h>`); a JNI bridge demonstrates Java embedding.
- **Data-driven ECS**: scenes, prefabs, components are pure data; no
  inheritance, no scripts hard-wired into the engine.

> **Architecture reference**: see [`ARCHITECTURE.md`](ARCHITECTURE.md)
> for the layered model, module map, ECS doctrine, and conventions.
> This README focuses on **build flows**.

## Canonical Dependencies

JCE keeps its third-party surface small and cross-platform. Every entry
below is sanctioned and architecturally load-bearing — *not* an incidental
transitive pull. New direct deps require explicit owner approval, an
update to this list, and an entry in `THIRD_PARTY_LICENSES.md`.

| Dep             | Role                                            | Layer                  |
|-----------------|-------------------------------------------------|------------------------|
| SDL3            | Window / input / platform abstraction           | os/platform            |
| bgfx            | Graphics backend abstraction                    | renderer               |
| PhysFS          | Virtual filesystem / archive I/O                | os/core                |
| enkiTS          | Task scheduler                                  | os/core                |
| mimalloc        | Allocator                                       | os/core                |
| Tracy           | Profiling                                       | os/core                |
| flecs           | Data-driven ECS                                 | scene / renderer / ui  |
| Recast & Detour | Navmesh generation + pathfinding                | middleware/ai          |
| miniaudio       | Audio device + mixing                           | middleware/audio       |
| Bullet          | 3D rigid-body physics                           | middleware/physics     |
| RmlUI           | HTML/CSS UI for in-game HUD                     | middleware/ui          |
| ImGui           | Editor UI (editor target only)                  | editor                 |
| cJSON           | JSON parsing                                    | os/core                |
| stb_image       | Common image decode                             | resource               |

Codec sources (fdk-aac, libhevc, openh264) are pulled locally on demand
and live under `engine/src/middleware/{audio,video}/third_party/`. They
are **not committed** to the repository — see
[`ARCHITECTURE.md`](ARCHITECTURE.md#vendored-third-party).

Platform-specific native APIs (Win32, POSIX, Cocoa, NDK) must always be
accessed through one of the canonical wrappers above; they are never
called directly from engine source.

## Project Structure

```
JCE/
├── engine/                # C99 static library — the engine
│   ├── include/jce/       # Public API (api.h + per-layer umbrellas)
│   ├── src/               # Layered implementation (os → renderer → middleware → application → runtime)
│   ├── shaders/           # Engine shaders (bgfx)
│   ├── resources/assets/  # Engine assets
│   └── java/com/jce/      # JNI Java bindings
├── caged_kingdom/         # Sandbox game (engine consumer)
├── editor/                # C++17 ImGui editor (engine consumer)
├── tools/                 # Build-time tools (asset packer, etc.)
├── ARCHITECTURE.md        # Layered model & module map (read this first)
├── conan/profiles/        # Per-platform Conan profiles
└── scripts/               # Build orchestration scripts
```

## Hosting Modes

The engine can be driven by three different hosts:

```
        ┌──────────────┐  flat C API
        │  jce engine  │ ─────────────────────────────┐
        │   (C99 .a)   │                              │
        └──────┬───────┘                              │
               │                                      │
   ┌───────────┼─────────────────┐                    │
   │           │                 │                    │
   ▼           ▼                 ▼                    ▼
 game.exe   editor.exe        jce.dll              (your app)
 (caged_*)  (ImGui driven)    + JNI Java host     (any C-ABI lang)
```

The editor *embeds* the engine; game modules register through
`jce_game_module_register` and are driven by `JceAppDesc` lifecycle hooks
identically in both standalone and editor-Play modes.

---

# Build Guide (Windows)

This guide covers:

1. Native C/C++ executables (standalone game + editor)
2. Java + JNI mode (Java drives the native engine)
3. Cross-compilation (Android / Windows-ARM64 / WebAssembly)

All commands below are tested in PowerShell from the repository root.

## Prerequisites

1. Visual Studio 2022 (Desktop C++ workload)
2. CMake 3.20+
3. Conan 2.x
4. Java 8+ JDK (current setup uses `openjdk-8`)

Optional:

1. Ninja (recommended for faster builds)
2. cppcheck (if you want static analysis during build)

## Repo Root

```powershell
Set-Location JCE
```

## A. Build Native Standalone Game (caged_kingdom)

Builds the standalone game executable (`caged_kingdom.exe`).

### A1) Install dependencies and generate toolchain

```powershell
conan install . --output-folder=build/desktop/windows-x64-conan --build=missing -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64
```

### A2) Configure

```powershell
cmake -S . -B build/desktop/windows-x64 -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=build/desktop/windows-x64-conan/build/Release/generators/conan_toolchain.cmake `
    -DCMAKE_BUILD_TYPE=Release `
    -DJCE_BUILD_VARIANT=release
```

Use `-DJCE_BUILD_VARIANT=dist` for distribution builds (no logging,
SHA-256 integrity).

### A3) Build

```powershell
cmake --build build/desktop/windows-x64
```

### A4) Run

```powershell
./build/desktop/windows-x64/release/caged_kingdom.exe
```

Or use the build script:

```powershell
./scripts/build-desktop.bat          # release build
./scripts/build-desktop.bat --dist   # distribution build
```

## B. Build Editor (jce_editor)

```powershell
./scripts/build-editor.bat           # release build
./scripts/build-editor.bat --dist    # distribution build
```

Output: `build/desktop/windows-x64/release/jce_editor.exe`

## C. Build Java + JNI Mode

Builds `jce.dll` and runs the app from Java (`com.jce.Main`).

### C1) Install dependencies and generate toolchain

```powershell
conan install . --output-folder=build/jni/desktop-conan --build=missing -o "&:jce_jni=True" -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64
```

### C2) Configure JNI build

```powershell
cmake -S . -B build/jni/desktop -G Ninja `
    -DCMAKE_TOOLCHAIN_FILE=build/jni/desktop-conan/build/Release/generators/conan_toolchain.cmake `
    -DJCE_BUILD_JNI=ON -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=release
```

### C3) Build JNI library target

```powershell
cmake --build build/jni/desktop
```

Expected output: `build/jni/desktop/caged_kingdom/jce.dll`

### C4) Compile Java classes

```powershell
New-Item -ItemType Directory -Force -Path build/jni/desktop/java-out | Out-Null

& javac -d build/jni/desktop/java-out `
    engine/java/com/jce/JceRuntime.java `
    engine/java/com/jce/Main.java
```

### C5) Run Java app with JNI

```powershell
& java "-Djava.library.path=build/jni/desktop/caged_kingdom" `
    -cp build/jni/desktop/java-out com.jce.Main
```

### C6) Package runnable JAR

```powershell
./scripts/package-jni-jar.bat
```

Output: `build/jni/dist/jce-jni.jar`

## D. Cross-Compilation

### Android

```powershell
./scripts/build-android.bat [ndk_path] [sdk_path] [arch] [--clean]
```

### Windows ARM64

```powershell
./scripts/build-windows-arm64.bat [--clean] [--dist]
```

### WebAssembly

```powershell
./scripts/build-web.bat [emsdk_path] [--clean] [--dist]
```

## Common Issues

### `jni.h` not found in editor

Ensure `.vscode/c_cpp_properties.json` includes:

1. `openjdk-8/include`
2. `openjdk-8/include/win32`

Then run `C/C++: Rescan Workspace` or reload window.

### `cppcheck` target fails in VS generator

Configure with `-DJCE_ENABLE_CPPCHECK=OFF` for Visual Studio builds, or
switch to Ninja generator for compile database workflows.
