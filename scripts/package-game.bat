@echo off
:: ===================================================================
:: package-game.bat  End-to-end "ship a game" pipeline.
::
:: Builds a JCE project via build-project.bat, then stages the
:: executable into a clean dist directory ready for distribution.
::
:: Usage:
::   package-game.bat <project_dir> [options]
::
:: Options:
::   --sdk <dir>                JCE SDK root.
::   --variant <release|dist>   Build variant (default: release).
::   --arch <x86_64|i686|...>   Target architecture (default: host).
::   --target <CMakeTarget>     CMake target to build.
::   --exe <name[.exe]>         Built artifact filename.
::   --name <project_name>      Used in output dir name.  Default = dir name.
::   --version <x.y.z>          Used in output dir name.  Default = 0.0.0.
::   --out <dir>                Output dir (default:
::                              dist\games\<name>-<version>-<plat>-<arch>).
::   --clean                    Wipe build/ before configuring.
::
:: Output layout:
::   <out>\<exe>
::   <out>\VERSION.txt
::
:: Exit codes:
::   0 ok | 1 bad usage | 2 build failed | 3 artefact / copy failed
:: ===================================================================
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"

set "PROJECT_DIR="
set "SDK_DIR="
set "VARIANT=release"
set "ARCH="
set "TARGET="
set "EXE="
set "P_NAME="
set "P_VERSION=0.0.0"
set "OUT_DIR="
set "DO_CLEAN="

:parse
if "%~1"=="" goto :after_parse
if /I "%~1"=="--sdk"      ( set "SDK_DIR=%~2"   & shift & shift & goto :parse )
if /I "%~1"=="--variant"  ( set "VARIANT=%~2"   & shift & shift & goto :parse )
if /I "%~1"=="--arch"     ( set "ARCH=%~2"      & shift & shift & goto :parse )
if /I "%~1"=="--target"   ( set "TARGET=%~2"    & shift & shift & goto :parse )
if /I "%~1"=="--exe"      ( set "EXE=%~2"       & shift & shift & goto :parse )
if /I "%~1"=="--name"     ( set "P_NAME=%~2"    & shift & shift & goto :parse )
if /I "%~1"=="--version"  ( set "P_VERSION=%~2" & shift & shift & goto :parse )
if /I "%~1"=="--out"      ( set "OUT_DIR=%~2"   & shift & shift & goto :parse )
if /I "%~1"=="--clean"    ( set "DO_CLEAN=1"    & shift & goto :parse )
if "%PROJECT_DIR%"=="" (
    set "PROJECT_DIR=%~1"
    shift
    goto :parse
)
echo [package-game] unknown argument: %~1
exit /b 1

:after_parse
if "%PROJECT_DIR%"=="" (
    echo [package-game] missing project_dir
    echo Usage: package-game.bat ^<project_dir^> [options]
    exit /b 1
)

pushd "%PROJECT_DIR%" >nul 2>&1
if errorlevel 1 (
    echo [package-game] project dir not found: %PROJECT_DIR%
    exit /b 1
)
set "PROJECT_DIR=%CD%"
popd

if "%P_NAME%"=="" (
    for %%I in ("%PROJECT_DIR%") do set "P_NAME=%%~nxI"
)

:: ---- Scrape manifest for name / version / target / exe -------------
:: Best-effort; CLI flags always win over manifest values.
set "MANIFEST=%PROJECT_DIR%\jce_project.json"
if exist "%MANIFEST%" (
    call :json_str "%MANIFEST%" name     _MF_NAME
    call :json_str "%MANIFEST%" version  _MF_VERSION
    call :json_str "%MANIFEST%" target   _MF_TARGET
    call :json_str "%MANIFEST%" exe      _MF_EXE
    if not "!_MF_NAME!"=="" if "%P_NAME%"=="" set "P_NAME=!_MF_NAME!"
    if not "!_MF_VERSION!"=="" if "%P_VERSION%"=="0.0.0" set "P_VERSION=!_MF_VERSION!"
    if not "!_MF_TARGET!"=="" if "%TARGET%"=="" set "TARGET=!_MF_TARGET!"
    if not "!_MF_EXE!"=="" if "%EXE%"=="" set "EXE=!_MF_EXE!"
)

:: ---- Arch / platform defaults --------------------------------------
if "%ARCH%"=="" (
    if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "ARCH=x86_64"
    if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=aarch64"
)
if "%ARCH%"=="" set "ARCH=x86_64"
set "PLATFORM=win32"

:: ---- Cook step -----------------------------------------------------
:: Mirror source_assets -> cooked_assets so the in-tree CMake build sees
:: the latest editor-authored content.  Reads paths from
:: jce_project.json (schema v2 fields) and falls back to the v1
:: convention "assets" -> "resources/_cooked".
set "_CK_SRC=assets"
set "_CK_DST=resources/_cooked"
call :json_str "%MANIFEST%" source_assets _CK_SRC
call :json_str "%MANIFEST%" cooked_assets _CK_DST
set "COOK_BAT=%SCRIPT_DIR%cook-project.bat"
if exist "%COOK_BAT%" (
    call "%COOK_BAT%" "%PROJECT_DIR%" "%_CK_SRC%" "%_CK_DST%"
    if errorlevel 1 (
        echo [package-game] cook step failed
        exit /b 2
    )
) else (
    echo [package-game] cook helper missing: %COOK_BAT% — skipping cook
)

:: ---- Run the build -------------------------------------------------
set "BUILDER=%SCRIPT_DIR%build-project.bat"
if not exist "%BUILDER%" (
    echo [package-game] missing builder: %BUILDER%
    exit /b 1
)

set "BUILD_ARGS=--variant %VARIANT% --arch %ARCH%"
if not "%TARGET%"=="" set "BUILD_ARGS=%BUILD_ARGS% --target %TARGET%"
if not "%EXE%"==""    set "BUILD_ARGS=%BUILD_ARGS% --exe %EXE%"
if not "%SDK_DIR%"=="" set "BUILD_ARGS=%BUILD_ARGS% --sdk %SDK_DIR%"
if defined DO_CLEAN   set "BUILD_ARGS=%BUILD_ARGS% --clean"

echo [package-game] running: "%BUILDER%" "%PROJECT_DIR%" %BUILD_ARGS%
call "%BUILDER%" "%PROJECT_DIR%" %BUILD_ARGS%
if errorlevel 1 (
    echo [package-game] build failed
    exit /b 2
)

:: ---- Locate built exe ---------------------------------------------
set "BUILD_DIR=%PROJECT_DIR%\build\%PLATFORM%-%ARCH%-%VARIANT%"
if "%EXE%"=="" set "EXE=%P_NAME%.exe"
:: Auto-append .exe if user passed bare name (matches editor convention).
if /I not "%EXE:~-4%"==".exe" set "EXE=%EXE%.exe"
set "BUILT_EXE=%BUILD_DIR%\%EXE%"
if not exist "%BUILT_EXE%" (
    echo [package-game] exe not found: %BUILT_EXE%
    exit /b 3
)

:: ---- Stage output --------------------------------------------------
if "%OUT_DIR%"=="" (
    set "OUT_DIR=%PROJECT_DIR%\dist\games\%P_NAME%-%P_VERSION%-%PLATFORM%-%ARCH%"
)
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%" 2>nul

copy /Y "%BUILT_EXE%" "%OUT_DIR%\" >nul
if errorlevel 1 (
    echo [package-game] copy of exe failed
    exit /b 3
)

:: Bundle any sibling DLLs emitted next to the exe.
for %%F in ("%BUILD_DIR%\*.dll") do (
    copy /Y "%%~F" "%OUT_DIR%\" >nul
)

:: ---- Stage cooked assets ------------------------------------------
:: The runtime expects resources/_cooked/ alongside the exe (PhysFS
:: mount root).  Without this, the packaged game ships with no assets
:: and silently fails to load any scene.  Path is read from the
:: manifest's cooked_assets field (schema v2); v1 default is
:: resources/_cooked.
set "_COOKED_REL=resources/_cooked"
call :json_str "%MANIFEST%" cooked_assets _CA
if not "!_CA!"=="" set "_COOKED_REL=!_CA!"
set "_COOKED_SRC=%PROJECT_DIR%\!_COOKED_REL:/=\!"
if exist "%_COOKED_SRC%" (
    set "_COOKED_DST=%OUT_DIR%\!_COOKED_REL:/=\!"
    if not exist "!_COOKED_DST!" mkdir "!_COOKED_DST!" 2>nul
    xcopy /E /I /Y /Q "%_COOKED_SRC%" "!_COOKED_DST!" >nul
    if errorlevel 1 (
        echo [package-game] WARN: failed to stage cooked assets from %_COOKED_SRC%
    )
) else (
    echo [package-game] WARN: cooked assets dir missing: %_COOKED_SRC%
    echo                 ^(cook step may have failed; packaged game will have no assets^)
)

:: ---- VERSION.txt ---------------------------------------------------
set "GIT_SHA=unknown"
for /f "usebackq tokens=* delims=" %%S in (`git -C "%PROJECT_DIR%" rev-parse --short HEAD 2^>nul`) do set "GIT_SHA=%%S"
> "%OUT_DIR%\VERSION.txt" (
    echo name:     %P_NAME%
    echo version:  %P_VERSION%
    echo platform: %PLATFORM%
    echo arch:     %ARCH%
    echo variant:  %VARIANT%
    echo commit:   %GIT_SHA%
    echo built:    %DATE% %TIME%
)

echo.
echo [package-game] OK
echo   out: %OUT_DIR%
echo   exe: %EXE%
echo.
exit /b 0

:: ===================================================================
:: Helpers
:: ===================================================================

:: json_str <manifest> <key> <out_var>
:: Best-effort scrape of "<key>": "<value>" from a small JSON file.
:: Only handles flat string fields, which is all the schema needs at
:: the top level today.  Mirrors build-project.bat's helper.
:json_str
set "%~3="
for /f "usebackq tokens=2 delims=:" %%A in (`findstr /i /c:"\"%~2\"" "%~1" 2^>nul`) do (
    set "_raw=%%A"
    set "_raw=!_raw: =!"
    set "_raw=!_raw:,=!"
    set "_raw=!_raw:"=!"
    if not "!_raw!"=="" set "%~3=!_raw!"
)
exit /b 0
