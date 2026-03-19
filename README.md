# JCE(JAVA CAT ENGINE) Build Guide (Windows)

This document provides working build flows for:

1. Native C/C++ executable (standalone app)
2. Java + JNI mode (Java drives the native engine)

All commands below are tested in PowerShell from the repository root.

## Prerequisites

1. Visual Studio 2022 (Desktop C++ workload)
2. CMake 3.20+
3. Conan 2.x
4. Java 8+ JDK (current setup uses `D:/Java21/openjdk-8`)

Optional:

1. Ninja (recommended for faster builds)
2. cppcheck (if you want static analysis during build)

## Repo Root

```powershell
Set-Location D:/Code/C_CPP/JCE
```

## A. Build Native Standalone Executable (C/C++)

This mode builds the normal native app entrypoint (`main.c` + SDL callbacks).

### A1) Install dependencies and generate toolchain

```powershell
conan install . --output-folder=build/Native --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja
```

### A2) Configure

```powershell
cmake -S . -B build/Native \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE=build/Native/build/Release/generators/conan_toolchain.cmake
```

### A3) Build

```powershell
cmake --build build/Native --config Release
```

### A4) Run

```powershell
./build/Native/src/JCE.exe
```

## B. Build Java + JNI Mode

This mode builds `jce.dll` and runs the app from Java (`com.jce.Main`).

### B1) Install dependencies and generate toolchain

```powershell
conan install . --output-folder=build/Jni --build=missing -o "&:jce_jni=True" -c tools.cmake.cmaketoolchain:generator=Ninja
```

Note: use `&:jce_jni=True` to scope the option to the root package and avoid Conan ambiguity warnings.

### B2) Configure JNI build (Visual Studio generator)

```powershell
cmake -S . -B build/JniVS \
	-G "Visual Studio 17 2022" -A x64 \
	-DCMAKE_TOOLCHAIN_FILE=build/Jni/build/Release/generators/conan_toolchain.cmake \
	-DJCE_BUILD_JNI=ON \
	-DJCE_ENABLE_CPPCHECK=OFF
```

`JCE_ENABLE_CPPCHECK=OFF` is recommended with Visual Studio generator because `compile_commands.json` is not produced there by default.

### B3) Build JNI library target

```powershell
cmake --build build/JniVS --config Release --target JCE
```

Expected output:

1. `build/JniVS/src/Release/jce.dll`
2. `build/JniVS/src/Release/jce.lib`

### B4) Compile Java classes

```powershell
New-Item -ItemType Directory -Force -Path build/JniVS/java-out | Out-Null

& "D:/Java21/openjdk-8/bin/javac.exe" \
	-d build/JniVS/java-out \
	src/main/java/com/jce/JceRuntime.java \
	src/main/java/com/jce/Main.java
```

### B5) Run Java app with JNI

```powershell
& "D:/Java21/openjdk-8/bin/java.exe" \
	"-Djava.library.path=build/JniVS/src/Release" \
	-cp build/JniVS/java-out \
	com.jce.Main
```

Smoke test (run N frames and exit):

```powershell
& "D:/Java21/openjdk-8/bin/java.exe" \
	"-Djava.library.path=build/JniVS/src/Release" \
	-cp build/JniVS/java-out \
	com.jce.Main 5
```

### B6) Package runnable JAR with embedded `jce.dll`

Use the script below after B3 completes:

```powershell
./scripts/package-jni-jar.ps1 -BuildDir build/JniVS -Config Release -JavaHome D:/Java21/openjdk-8
```

Double-click friendly options:

1. `package-jni-jar.bat` in repo root
2. `scripts/package-jni-jar.bat`

Both BAT launchers run PowerShell with `ExecutionPolicy Bypass`.

1. Success: auto-close after 5 seconds.
2. Failure: keep window open and prompt for key press.

If `build/JniVS` does not exist, the script will automatically run:

```powershell
conan install . --output-folder=build/Jni --build=missing -o "&:jce_jni=True" -c tools.cmake.cmaketoolchain:generator=Ninja
cmake -S . -B build/JniVS -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=build/Jni/build/Release/generators/conan_toolchain.cmake -DJCE_BUILD_JNI=ON -DJCE_ENABLE_CPPCHECK=OFF
```

If `build/JniVS/src/Release/jce.dll` is missing, the script then automatically tries:

```powershell
cmake --build build/JniVS --config Release --target JCE
```

before packaging.

Output:

1. `build/JniVS/dist/jce-jni-win64.jar`
2. JAR includes native library at `natives/win32-x86_64/jce.dll`

Run packaged JAR (no `-Djava.library.path` needed):

```powershell
& "D:/Java21/openjdk-8/bin/java.exe" -jar build/JniVS/dist/jce-jni-win64.jar 5
```

Config file resolution for packaged JAR:

1. JNI runtime resolves config path relative to JAR location.
2. Place config at `build/JniVS/dist/.config/jce.ini`.

## VS Code Launch

You can run either mode from VS Code:

1. Native C/C++: launch configuration `JCE (MSVC)`
2. Java + JNI: launch configuration `JceRuntimeMain (JNI DLL)`

## Common Issues

### `jni.h` not found in editor

Ensure `.vscode/c_cpp_properties.json` includes:

1. `D:/Java21/openjdk-8/include`
2. `D:/Java21/openjdk-8/include/win32`

Then run `C/C++: Rescan Workspace` or reload window.

### `Duplicate preset: conan-release`

Do not rely on merged presets when there are duplicate names. Use explicit `cmake -S ... -B ...` configure commands shown above.

### `cppcheck` target fails in VS generator

Configure with `-DJCE_ENABLE_CPPCHECK=OFF` for Visual Studio builds, or switch to Ninja generator for compile database workflows.

