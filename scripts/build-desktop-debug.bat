@echo off
:: ================================================================
:: build-desktop-debug.bat -- Build caged_kingdom.exe with FULL debug info
::
::   * /MDd debug CRT (CrtDbgFlag works)
::   * /Zi /Ob0 /Od (no inlining, full source mapping)
::   * /RTC1 (run-time stack & uninit checks)
::   * /DEBUG:FULL PDB linked
::   * Conan deps rebuilt in Debug (matching CRT)
::
::   Output: build\desktop\windows-x64-debug\debug\caged_kingdom.exe
::           build\desktop\windows-x64-debug\debug\caged_kingdom.pdb
::
:: Usage: build-desktop-debug.bat [--clean]
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-debug-conan"
set "BUILD_DIR=build\desktop\windows-x64-debug"
set "TOOLCHAIN=%CONAN_DIR%\build\Debug\generators\conan_toolchain.cmake"
set "PROFILE=conan/profiles/windows-x64-debug"
set "COMMON=%~dp0lib\jce_build_common.bat"

:: -- Parse arguments --
:parse_args
if /i "%~1"=="--clean" (
    call "%COMMON%" clean "%BUILD_DIR%" "%CONAN_DIR%"
    shift
    goto :parse_args
)

:: -- Step 1: Conan install (Debug profile, builds dependencies in Debug) --
echo === Step 1: Conan install (windows-x64-debug) ===
echo   This will (re)build third-party deps in Debug mode the first time.
echo   It can take 10-20 minutes initially; subsequent runs are instant.
call "%COMMON%" conan "%PROFILE%" "%PROFILE%" "%CONAN_DIR%" "%TOOLCHAIN%" || goto :error

:: -- Step 2: CMake configure (direct, bypasses CMakePresets to avoid
::    "Duplicate conan-release" error from legacy CMakeUserPresets.json) --
echo === Step 2: CMake configure (Debug) ===

:: Locate shaderc from the Conan cache (bgfx tools=True). The cmake helper
:: hardcodes bgfx_PACKAGE_FOLDER_RELEASE which is empty in Debug builds, so we
:: pass the path explicitly. shaderc is a host tool; build_type doesn't matter.
call "%COMMON%" shaderc

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

:: -- Step 3: Build CagedKingdom target --
echo === Step 3: Build CagedKingdom (Debug) ===
cmake --build %BUILD_DIR% --target CagedKingdom
if errorlevel 1 goto :error

call "%COMMON%" verify "%BUILD_DIR%\debug\caged_kingdom.exe" "caged_kingdom.exe" || goto :error
if not exist "%BUILD_DIR%\debug\caged_kingdom.pdb" (
    echo WARNING: caged_kingdom.pdb missing -- VS debugger will only show addresses.
)

echo.
echo [SUCCESS] Debug desktop build complete:
echo   EXE: %BUILD_DIR%\debug\caged_kingdom.exe
echo   PDB: %BUILD_DIR%\debug\caged_kingdom.pdb
echo.
echo Next steps:
echo   * Open D:\Code\C_CPP\JCE in VS 2026 (File ^> Open ^> Folder)
echo   * Pick the "windows-x64-debug" CMake preset
echo   * Right-click CagedKingdom target ^> Set as Startup Item
echo   * Set working directory to: %BUILD_DIR%\debug
echo   * F5 to debug
popd
call "%~dp0lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Debug build failed.
popd
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
