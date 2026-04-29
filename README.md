# JCE (JAVA CAT ENGINE) Build Guide (Windows)

This document provides working build flows for:

1. Native C/C++ executables (standalone game + editor)
2. Java + JNI mode (Java drives the native engine)

All commands below are tested in PowerShell from the repository root.

## Canonical Dependencies

JCE deliberately keeps its third-party dependency surface small and
cross-platform. Every entry below is a sanctioned, architecturally-load-
bearing dependency — *not* an incidental transitive pull. New direct deps
are only added with explicit project-owner approval and require updating
this list plus `THIRD_PARTY_LICENSES.md`.

| Dep      | Role                              | Layer       |
|----------|-----------------------------------|-------------|
| SDL3     | Window / input / platform abstraction | os/platform |
| bgfx     | Graphics backend abstraction      | renderer    |
| cglm     | SIMD math (wrapped by `jce_math`) | os/core     |
| PhysFS   | Virtual filesystem / archive I/O  | os/core     |
| enkiTS   | Task scheduler                    | os/core     |
| mimalloc | Allocator                         | os/core     |
| Tracy    | Profiling                         | os/core     |
| flecs    | Data-driven ECS                   | scene / renderer / ui |
| Recast & Detour | Navigation mesh generation + pathfinding | middleware/navigation |

Platform-specific native APIs (Win32, POSIX, Cocoa, NDK) must always be
accessed through one of the canonical wrappers above; they are never
called directly from engine source.

## Project Structure

```
JCE/
├── engine/              # C99 static library (core engine)
│   ├── include/jce/     # Public API headers
│   ├── src/             # Engine implementation
│   ├── shaders/         # Engine shaders
│   ├── resources/assets # Engine assets
│   └── java/com/jce/    # JNI Java sources
├── caged_kingdom/       # Sandbox game executable
│   ├── src/             # Game code (main.c + game/)
│   └── resources/assets # Game-specific assets
├── editor/              # C++17 editor executable
│   ├── src/             # Editor code
│   ├── shaders/         # Editor shaders
│   └── resources/assets # Editor assets
├── tools/               # Build-time tools
│   └── jce_pak.c        # Asset packer
├── conan/profiles/      # Conan profiles per platform
└── scripts/             # Build scripts
```

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

This mode builds the standalone game executable (`caged_kingdom.exe`).

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

Use `-DJCE_BUILD_VARIANT=dist` for distribution builds (no logging, SHA-256 integrity).

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

### Using the build script

```powershell
./scripts/build-editor.bat           # release build
./scripts/build-editor.bat --dist    # distribution build
```

Output: `build/desktop/windows-x64/release/jce_editor.exe`

## C. Build Java + JNI Mode

This mode builds `jce.dll` and runs the app from Java (`com.jce.Main`).

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

Configure with `-DJCE_ENABLE_CPPCHECK=OFF` for Visual Studio builds, or switch to Ninja generator for compile database workflows.
