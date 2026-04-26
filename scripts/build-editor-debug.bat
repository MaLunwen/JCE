@echo off
:: ================================================================
:: build-editor-debug.bat -- Build jce_editor.exe with FULL debug info
::
::   * /MDd debug CRT (CrtDbgFlag works)
::   * /Zi /Ob0 /Od (no inlining, full source mapping)
::   * /RTC1 (run-time stack & uninit checks)
::   * /DEBUG:FULL PDB linked
::   * Conan deps rebuilt in Debug (matching CRT)
::
::   Output: build\desktop\windows-x64-debug\debug\jce_editor.exe
::           build\desktop\windows-x64-debug\debug\jce_editor.pdb
::
::   Open in VS 2026:
::     1. File -> Open -> Folder -> D:\Code\C_CPP\JCE
::     2. Pick configuration: "windows-x64-debug" from the preset dropdown
::     3. Set startup item: jce_editor.exe
::     4. Working dir: build\desktop\windows-x64-debug\release
::     5. F5 to debug
::
:: Usage: build-editor-debug.bat [--clean]
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-debug-conan"
set "BUILD_DIR=build\desktop\windows-x64-debug"
set "TOOLCHAIN=%CONAN_DIR%\build\Debug\generators\conan_toolchain.cmake"
set "PROFILE=conan/profiles/windows-x64-debug"

:: -- Parse arguments --
:parse_args
if /i "%~1"=="--clean" (
    echo === Cleaning debug build directory ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    echo   Done
    shift
    goto :parse_args
)

:: -- Step 1: Conan install (Debug profile, builds dependencies in Debug) --
echo === Step 1: Conan install (windows-x64-debug) ===
echo   This will (re)build third-party deps in Debug mode the first time.
echo   It can take 10-20 minutes initially; subsequent runs are instant.
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force rebuild.
) else (
    conan install . -pr:h %PROFILE% -pr:b %PROFILE% --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 2: CMake configure (direct, bypasses CMakePresets to avoid
::    "Duplicate conan-release" error from legacy CMakeUserPresets.json) --
echo === Step 2: CMake configure (Debug) ===

:: Locate shaderc from Debug Conan build (bgfx tools=True). The cmake helper
:: hardcodes bgfx_PACKAGE_FOLDER_RELEASE which is empty in Debug builds, so we
:: pass the path explicitly. shaderc is a host tool; build_type doesn't matter.
set "SHADERC_EXE="
for /f "delims=" %%S in ('powershell -NoProfile -Command "$paths=@('%USERPROFILE%\.conan2\p\b'); Get-ChildItem -Path $paths -Recurse -Filter shaderc.exe -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'bgfx.+\\(b\\build\\Debug|p\\bin)\\' } | Sort-Object { if ($_.FullName -match '\\b\\build\\Debug\\') { 0 } else { 1 } } | Select-Object -First 1 -ExpandProperty FullName"') do (
    set "SHADERC_EXE=%%S"
)
if defined SHADERC_EXE (
    echo   Using shaderc: !SHADERC_EXE!
) else (
    echo WARNING: shaderc.exe not found; CMake configure will fail.
)

cmake -S . -B %BUILD_DIR% -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% ^
    -DCMAKE_BUILD_TYPE=Debug ^
    -DJCE_BUILD_VARIANT=debug ^
    -DJCE_ENABLE_CPPCHECK=OFF ^
    -DJCE_SHADERC_EXECUTABLE="!SHADERC_EXE!" ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    "-DCMAKE_C_FLAGS_DEBUG=/MDd /Zi /Ob0 /Od /RTC1 /DDEBUG /D_DEBUG /FS" ^
    "-DCMAKE_CXX_FLAGS_DEBUG=/MDd /Zi /Ob0 /Od /RTC1 /DDEBUG /D_DEBUG /FS" ^
    "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=/DEBUG:FULL /INCREMENTAL:NO" ^
    "-DCMAKE_SHARED_LINKER_FLAGS_DEBUG=/DEBUG:FULL /INCREMENTAL:NO"
if errorlevel 1 goto :error

:: -- Step 3: Build editor target --
echo === Step 3: Build JCE_Editor (Debug) ===
cmake --build %BUILD_DIR% --target JCE_Editor
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\debug\jce_editor.exe" (
    echo ERROR: jce_editor.exe not found after build
    goto :error
)
if not exist "%BUILD_DIR%\debug\jce_editor.pdb" (
    echo WARNING: jce_editor.pdb missing -- VS debugger will only show addresses.
)

echo.
echo [SUCCESS] Debug editor build complete:
echo   EXE: %BUILD_DIR%\debug\jce_editor.exe
echo   PDB: %BUILD_DIR%\debug\jce_editor.pdb
echo.
echo Next steps:
echo   * Open D:\Code\C_CPP\JCE in VS 2026 (File ^> Open ^> Folder)
echo   * Pick the "windows-x64-debug" CMake preset
echo   * Right-click jce_editor target ^> Set as Startup Item
echo   * Set working directory to: %BUILD_DIR%\debug
echo   * Press Ctrl+Alt+F2 to open Diagnostic Tools (Heap Profiling)
echo   * F5 to debug; take 3 memory snapshots over 60s to find leaks
popd
call "%~dp0lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Debug build failed.
popd
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
