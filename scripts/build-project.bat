@echo off
:: ===================================================================
:: build-project.bat  Universal JCE project build entry point.
::
:: Usage:
::   build-project.bat <project_dir> [options]
::
:: Options:
::   --sdk <dir>                JCE SDK root (overrides jce_project.json
::                              sdk field and the JCE_SDK_DIR env var).
::   --target <CMakeTarget>     Target to build  (default: project name).
::   --exe <name[.exe]>         Expected artifact filename (verification).
::   --variant <debug|release|dist>   Build variant (default: release).
::   --arch <x86_64|i686|aarch64|armv7>
::                              Target architecture (aliases x64/x86/arm64/arm
::                              accepted).  Defaults to host arch probed
::                              from %PROCESSOR_ARCHITECTURE%.
::   --target-platform <p>      win32|linux|darwin|android|ios|wasm
::                              (only win32 is implemented in this host
::                               for now; others print a helpful stub).
::   --clean                    Remove the project's build/ subdir first.
::
:: Resolution order for the SDK:
::   1. --sdk <dir>
::   2. <project_dir>\jce_project.json "sdk"
::   3. %JCE_SDK_DIR%
::   4. <editor_install_dir>\sdk
::   5. Engine-workspace fallback: if the project is inside a JCE source
::      tree (detected by ..\engine\include\jce\api.h), delegate to the
::      in-tree build-desktop.bat path.
::
:: Output:
::   <project_dir>\build\<platform>-<arch>-<variant>\<exe>
:: ===================================================================
setlocal enabledelayedexpansion

:: Cache the script directory NOW before any pushd/popd disturbs how
:: cmd resolves a relatively-invoked %~dp0.  Everything downstream
:: should use %SCRIPT_DIR% instead of %~dp0.
set "SCRIPT_DIR=%~dp0"

:: ------- Defaults --------------------------------------------------
set "PROJECT_DIR="
set "SDK_DIR="
set "TARGET="
set "EXE_NAME="
set "VARIANT=release"
set "PLATFORM=win32"
set "DO_CLEAN=0"
:: Probe host arch (overridable via --arch).  We accept short aliases
:: (x64/x86/arm64) and normalise to the GNU-triplet style names used
:: across the SDK install tree (x86_64/i686/aarch64).
set "ARCH=x86_64"
if /i "%PROCESSOR_ARCHITECTURE%"=="x86"   set "ARCH=i686"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=aarch64"
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set "ARCH=aarch64"

:: ------- Parse args ------------------------------------------------
if "%~1"=="" goto :usage
set "PROJECT_DIR=%~1"
shift

:parse_args
if "%~1"=="" goto :after_args
if /i "%~1"=="--sdk"             ( set "SDK_DIR=%~2"  & shift & shift & goto :parse_args )
if /i "%~1"=="--target"          ( set "TARGET=%~2"   & shift & shift & goto :parse_args )
if /i "%~1"=="--exe"             ( set "EXE_NAME=%~2" & shift & shift & goto :parse_args )
if /i "%~1"=="--variant"         ( set "VARIANT=%~2"  & shift & shift & goto :parse_args )
if /i "%~1"=="--target-platform" ( set "PLATFORM=%~2" & shift & shift & goto :parse_args )
if /i "%~1"=="--arch"            ( set "ARCH=%~2"     & shift & shift & goto :parse_args )
if /i "%~1"=="--clean"           ( set "DO_CLEAN=1"   & shift & goto :parse_args )
if /i "%~1"=="-h"                goto :usage
if /i "%~1"=="--help"            goto :usage
echo ERROR: unknown argument: %~1
goto :usage
:after_args

:: ------- Normalise & validate arch ---------------------------------
:: Accept short aliases for convenience.
if /i "%ARCH%"=="x64"   set "ARCH=x86_64"
if /i "%ARCH%"=="amd64" set "ARCH=x86_64"
if /i "%ARCH%"=="x86"   set "ARCH=i686"
if /i "%ARCH%"=="arm64" set "ARCH=aarch64"
if /i "%ARCH%"=="arm"   set "ARCH=armv7"
if /i "%ARCH%"=="armv7a" set "ARCH=armv7"
if /i "%ARCH%"=="armhf" set "ARCH=armv7"
if /i "%ARCH%"=="x86_64"  goto :arch_ok
if /i "%ARCH%"=="i686"    goto :arch_ok
if /i "%ARCH%"=="aarch64" goto :arch_ok
if /i "%ARCH%"=="armv7"   goto :arch_ok
echo [build-project] ERROR: unsupported --arch '%ARCH%'
echo                 (expected: x86_64^|i686^|aarch64^|armv7
echo                  aliases:  x64^|x86^|arm64^|arm)
exit /b 2
:arch_ok

:: ------- Normalise platform ----------------------------------------
if /i "%PLATFORM%"=="win"     set "PLATFORM=win32"
if /i "%PLATFORM%"=="windows" set "PLATFORM=win32"
if /i "%PLATFORM%"=="mac"     set "PLATFORM=darwin"
if /i "%PLATFORM%"=="macos"   set "PLATFORM=darwin"
if /i "%PLATFORM%"=="osx"     set "PLATFORM=darwin"

:: ------- Platform-arch sanity --------------------------------------
:: Windows host can't natively target 32-bit ARM (armv7) — Microsoft
:: dropped the toolchain.  Catch this early instead of misrouting to
:: the x64 engine tree.
if /i "%PLATFORM%"=="win32" if /i "%ARCH%"=="armv7" (
    echo [build-project] ERROR: --arch armv7 is not supported on Windows.
    echo                 Microsoft removed the 32-bit ARM Windows toolchain.
    echo                 Use --arch aarch64 ^(64-bit ARM^), or target Android
    echo                 / Linux with --target-platform.
    exit /b 2
)

:: ------- Validate project dir + manifest ---------------------------
if not exist "%PROJECT_DIR%" (
    echo [build-project] ERROR: project directory not found: %PROJECT_DIR%
    exit /b 2
)
pushd "%PROJECT_DIR%" >nul || ( echo [build-project] ERROR: cannot cd to %PROJECT_DIR% & exit /b 2 )
set "PROJECT_DIR=%CD%"
popd >nul

set "MANIFEST=%PROJECT_DIR%\jce_project.json"
if not exist "%MANIFEST%" (
    echo [build-project] ERROR: no jce_project.json in %PROJECT_DIR%
    echo                 Create one with the editor:  File ^> New Project
    exit /b 2
)

:: ------- Derive defaults from manifest -----------------------------
:: Light-touch JSON scrape (we don't ship a JSON CLI yet).
if "%TARGET%"=="" call :json_str "%MANIFEST%" target  TARGET
if "%TARGET%"=="" call :json_str "%MANIFEST%" name    TARGET
if "%EXE_NAME%"=="" call :json_str "%MANIFEST%" exe   EXE_NAME
if "%EXE_NAME%"=="" set "EXE_NAME=%TARGET%.exe"
:: Ensure .exe extension (case-insensitive) without doubling it.
set "_lower=!EXE_NAME!"
:: Crude lower-case for last 4 chars: rely on if /i compare instead.
set "_tail=!EXE_NAME:~-4!"
if /i not "!_tail!"==".exe" set "EXE_NAME=!EXE_NAME!.exe"

:: ------- Cook source -> cooked assets ------------------------------
:: Mirror source_assets -> cooked_assets so the in-source PAK pack step
:: sees the editor-authored content.  Defaults come from schema v2.
:: Delegated to PowerShell to avoid cmd.exe JSON-parsing pain.
set "_COOKED=resources/_cooked"
call :json_str "%MANIFEST%" cooked_assets _COOKED
set "COOK_PS1=%SCRIPT_DIR%cook-project.ps1"
if exist "%COOK_PS1%" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%COOK_PS1%" -ProjectDir "%PROJECT_DIR%" -Manifest "%MANIFEST%"
    if errorlevel 1 (
        echo [build-project] WARN: cook step failed — continuing with stale cooked tree
    )
) else (
    echo [build-project] WARN: cook helper missing: %COOK_PS1%
)

:: ------- Platform dispatch -----------------------------------------
if /i not "%PLATFORM%"=="win32" (
    echo [build-project] Target platform '%PLATFORM%' is not yet wired up
    echo                 in build-project.bat on this host.  Planned in a
    echo                 follow-up phase ^(toolchain detector + per-platform
    echo                 dispatch^).  For now, only --target-platform
    echo                 win^|win32 is implemented.
    exit /b 3
)

:: ------- Resolve SDK -----------------------------------------------
if "%SDK_DIR%"=="" call :json_str "%MANIFEST%" sdk SDK_DIR
if "%SDK_DIR%"=="" if not "%JCE_SDK_DIR%"=="" set "SDK_DIR=%JCE_SDK_DIR%"
if "%SDK_DIR%"=="" (
    :: Look for an editor-bundled SDK next to this script.
    if exist "%SCRIPT_DIR%..\sdk\lib\cmake\JCE\JCEConfig.cmake" set "SDK_DIR=%SCRIPT_DIR%..\sdk"
)

:: Normalise SDK_DIR to an absolute path (relative paths break cmake's
:: find_package since the working dir at configure-time is the build
:: tree, not the caller's CWD).
if not "%SDK_DIR%"=="" (
    for %%I in ("%SDK_DIR%") do set "SDK_DIR=%%~fI"
)

set "USE_SDK=0"
set "JCE_CMAKE_DIR="
if not "%SDK_DIR%"=="" (
    if exist "%SDK_DIR%\lib\cmake\JCE\JCEConfig.cmake" (
        set "USE_SDK=1"
        set "JCE_CMAKE_DIR=%SDK_DIR%\lib\cmake\JCE"
    ) else if exist "%SDK_DIR%\cmake\JCEConfig.cmake" (
        set "USE_SDK=1"
        set "JCE_CMAKE_DIR=%SDK_DIR%\cmake"
    )
)

:: ------- Engine-workspace fallback ---------------------------------
:: If no SDK but the project sits inside a JCE source tree, delegate
:: to the existing in-tree pipeline (build-desktop.bat).
set "ENGINE_ROOT="
if "%USE_SDK%"=="1" goto :after_engine_search
set "_p=%PROJECT_DIR%"
:find_engine_root
if exist "%_p%\engine\include\jce\api.h" (
    set "ENGINE_ROOT=%_p%"
    echo [build-project] No SDK located; falling back to engine-workspace
    echo                 mode at: %_p%
    goto :after_engine_search
)
for %%I in ("%_p%\..") do set "_parent=%%~fI"
if /i "%_parent%"=="%_p%" goto :after_engine_search
set "_p=%_parent%"
goto :find_engine_root
:after_engine_search

if "%USE_SDK%"=="0" if "%ENGINE_ROOT%"=="" (
    echo [build-project] ERROR: cannot locate a JCE SDK.
    echo                  - pass --sdk ^<dir^>
    echo                  - set the JCE_SDK_DIR environment variable
    echo                  - or set "sdk" in jce_project.json
    echo                 SDK packaging will arrive in Phase 3; for now
    echo                 place the project inside the JCE source tree to
    echo                 use engine-workspace mode.
    exit /b 4
)

:: ===================================================================
:: ENGINE-WORKSPACE FALLBACK PATH
:: ===================================================================
:: When the user's project is inside a JCE source tree but the SDK has
:: not been packaged yet, auto-install the SDK from the engine's build
:: tree (assumes the engine itself was already configured) and then
:: re-enter the SDK path with that fresh SDK_DIR.  This makes
::    File > New Project   →   ▶ Build
:: just work inside a dev checkout without forcing the user to run the
:: package-sdk script manually.
if "%USE_SDK%"=="0" (
    set "_AUTO_SDK=%ENGINE_ROOT%\dist\sdk\%PLATFORM%-%ARCH%"
    :: Engine build tree uses its own short arch names; translate.
    set "_ENG_ARCH=x64"
    if /i "%ARCH%"=="i686"    set "_ENG_ARCH=x86"
    if /i "%ARCH%"=="aarch64" set "_ENG_ARCH=arm64"
    set "_ENGINE_BUILD=%ENGINE_ROOT%\build\desktop\windows-!_ENG_ARCH!"
    if not exist "!_ENGINE_BUILD!\CMakeCache.txt" (
        echo [build-project] ERROR: engine workspace at %ENGINE_ROOT%
        echo                 has no build tree at !_ENGINE_BUILD!.  Run
        echo                 scripts\build-desktop.bat once to configure
        echo                 the engine, then retry.
        exit /b 9
    )
    rem Always re-run cmake --install in engine-workspace mode so the
    rem bundled SDK (headers, CMake exports, fat libs, host tools) stays
    rem in sync with whatever the engine tree currently builds. cmake's
    rem install step is itself incremental (content-identical files are
    rem skipped) so the steady-state cost is just stat() per file.
    rem Without this, fresh engine changes (new public types, new public
    rem API entries, etc.) silently fail to reach generated user projects
    rem until someone manually nukes dist\sdk.
    rem NOTE: :: comments inside an if (...) block break cmd.exe parsing
    rem ("X was unexpected at this time"), so we must use rem here.
    echo [build-project] Refreshing JCE SDK from engine build tree
    echo                 to: !_AUTO_SDK!
    cmake --install "!_ENGINE_BUILD!" --prefix "!_AUTO_SDK!" >nul || (
        echo [build-project] ERROR: SDK install failed.
        exit /b 10
    )
    if exist "!_AUTO_SDK!\lib\cmake\JCE\JCEConfig.cmake" (
        set "SDK_DIR=!_AUTO_SDK!"
        set "USE_SDK=1"
        set "JCE_CMAKE_DIR=!_AUTO_SDK!\lib\cmake\JCE"
        echo [build-project] Using auto-installed SDK at: !_AUTO_SDK!
    ) else (
        echo [build-project] ERROR: SDK install ran but JCEConfig.cmake
        echo                 still missing under !_AUTO_SDK!.
        exit /b 11
    )
)

:: ===================================================================
:: SDK PATH  (Phase 3 will produce JCEConfig.cmake under <sdk>/cmake)
:: ===================================================================
set "BUILD_DIR=%PROJECT_DIR%\build\%PLATFORM%-%ARCH%-%VARIANT%"
if "%DO_CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

set "CMAKE_BUILD_TYPE=Release"
if /i "%VARIANT%"=="debug" set "CMAKE_BUILD_TYPE=Debug"
if /i "%VARIANT%"=="dist"  set "CMAKE_BUILD_TYPE=Release"

echo [build-project] Configuring %TARGET% (%PLATFORM%/%ARCH%/%VARIANT%) with SDK at %SDK_DIR%

:: The Windows SDK ships MSVC-built static libs.  Force cmake to pick
:: cl.exe regardless of PATH order, and activate vcvars if needed so
:: cl + link + lib are reachable.  Override with JCE_SKIP_VCVARS=1.
:: vcvars target arch is derived from --arch.  When host is x64 and
:: target is i686/aarch64 we use the cross builder.
set "VCVARS_ARG=x64"
if /i "%ARCH%"=="i686"    set "VCVARS_ARG=x86"
if /i "%ARCH%"=="aarch64" set "VCVARS_ARG=arm64"
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" (
    if /i "%ARCH%"=="i686"    set "VCVARS_ARG=x64_x86"
    if /i "%ARCH%"=="aarch64" set "VCVARS_ARG=x64_arm64"
)
if not defined JCE_SKIP_VCVARS (
    where cl >nul 2>&1
    if errorlevel 1 (
        call :find_vcvars
        if defined VCVARS_BAT (
            echo [build-project] Activating MSVC env: !VCVARS_BAT! ^(!VCVARS_ARG!^)
            call "!VCVARS_BAT!" !VCVARS_ARG! >nul
        ) else (
            echo [build-project] WARN: cl.exe not on PATH and no vcvarsall.bat
            echo                  found.  The Windows SDK requires MSVC; install
            echo                  Visual Studio 2019/2022 ^(or Build Tools^) or
            echo                  open an x64 Native Tools prompt and re-run.
            exit /b 8
        )
    )
)

:: ------- Resolve bundles list ---------------------------------------
:: Extract `bundles` (JSON array of strings) and pass semicolon-joined
:: paths to CMake via -DJCE_PROJECT_BUNDLES; the template's POST_BUILD
:: step then copies each into `<exe_dir>/bundles/`.
set "BUNDLES="
for /f "usebackq delims=" %%B in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $j = Get-Content -Raw -LiteralPath '%MANIFEST%' | ConvertFrom-Json; if ($j.bundles) { ($j.bundles -join ';') } } catch {}"`) do (
    set "BUNDLES=%%B"
)

cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -G Ninja ^
      -DCMAKE_BUILD_TYPE=%CMAKE_BUILD_TYPE% ^
      -DCMAKE_C_COMPILER=cl ^
      -DJCE_PROJECT_COOKED_ASSETS="%_COOKED%" ^
      -DJCE_PROJECT_BUNDLES="%BUNDLES%" ^
      -DJCE_DIR="%JCE_CMAKE_DIR%" || exit /b 5

echo [build-project] Building %TARGET%
cmake --build "%BUILD_DIR%" --target "%TARGET%" || exit /b 6

set "EXE_PATH=%BUILD_DIR%\%EXE_NAME%"
if not exist "%EXE_PATH%" (
    :: Some generators place under <BUILD>\Release\.
    if exist "%BUILD_DIR%\%CMAKE_BUILD_TYPE%\%EXE_NAME%" set "EXE_PATH=%BUILD_DIR%\%CMAKE_BUILD_TYPE%\%EXE_NAME%"
)
if not exist "%EXE_PATH%" (
    echo [build-project] ERROR: build succeeded but artifact missing: %EXE_NAME%
    echo                 Searched: %BUILD_DIR%
    exit /b 7
)

echo [build-project] OK -^> %EXE_PATH%
exit /b 0

:: ===================================================================
:: Helpers
:: ===================================================================

:: json_str<manifest> <key> <out_var>
:: Best-effort scrape of "<key>": "<value>" from a small JSON file.
:: Not a real parser — only handles flat string fields, which is all
:: our schema has at the top level today.
:json_str
rem Preserve the caller-provided default in %~3.  Only overwrite it if
rem the field is actually present in the manifest.
for /f "usebackq tokens=2 delims=:" %%A in (`findstr /i /c:"\"%~2\"" "%~1" 2^>nul`) do (
    set "_raw=%%A"
    rem trim whitespace (space + tab), CR, commas, quotes
    set "_raw=!_raw: =!"
    for /f "tokens=* delims=	" %%T in ("!_raw!") do set "_raw=%%T"
    set "_raw=!_raw:	=!"
    set "_raw=!_raw:,=!"
    set "_raw=!_raw:"=!"
    if not "!_raw!"=="" set "%~3=!_raw!"
)
exit /b 0

:: find_vcvars  →  sets VCVARS_BAT to a usable vcvarsall.bat (or empty).
:: Probes vswhere first, then a small list of well-known install paths.
:find_vcvars
set "VCVARS_BAT="
set "_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%_VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%_VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        if exist "%%I\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS_BAT=%%I\VC\Auxiliary\Build\vcvarsall.bat"
    )
)
if defined VCVARS_BAT exit /b 0
for %%E in (2022 2019) do for %%F in (Enterprise Professional Community BuildTools) do (
    if exist "%ProgramFiles%\Microsoft Visual Studio\%%E\%%F\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS_BAT=%ProgramFiles%\Microsoft Visual Studio\%%E\%%F\VC\Auxiliary\Build\vcvarsall.bat"
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\%%F\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS_BAT=%ProgramFiles(x86)%\Microsoft Visual Studio\%%E\%%F\VC\Auxiliary\Build\vcvarsall.bat"
)
exit /b 0
goto :eof

:usage
echo build-project.bat  Build a JCE project.
echo.
echo   build-project.bat ^<project_dir^> [--sdk DIR] [--target NAME]
echo       [--exe NAME] [--variant debug^|release^|dist]
echo       [--arch x86_64^|i686^|aarch64^|armv7]  ^(aliases: x64^|x86^|arm64^|arm^)
echo       [--target-platform win32^|linux^|darwin^|android^|ios^|wasm] [--clean]
exit /b 1
