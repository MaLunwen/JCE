@echo off
:: ================================================================
:: build-desktop.bat -- Build a standalone .exe for Windows x64
:: Usage: build-desktop.bat [--clean] [--dist] [--target <CMakeTarget>] [--exe <name.exe>]
:: Defaults: --target CagedKingdom --exe caged_kingdom.exe
:: Output: build\desktop\windows-x64\<release|dist>\<exe>
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-conan"
set "BUILD_DIR=build\desktop\windows-x64"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "VARIANT=release"
set "TARGET=CagedKingdom"
set "EXE_NAME=caged_kingdom.exe"

:: -- Parse arguments --
:parse_args
if /i "%~1"=="--clean" (
    echo === Cleaning build directory ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    echo   Done
    shift
    goto :parse_args
)
if /i "%~1"=="--dist" (
    set "VARIANT=dist"
    shift
    goto :parse_args
)
if /i "%~1"=="--target" (
    set "TARGET=%~2"
    shift
    shift
    goto :parse_args
)
if /i "%~1"=="--exe" (
    set "EXE_NAME=%~2"
    shift
    shift
    goto :parse_args
)
if not "%~1"=="" (
    echo ERROR: unknown argument: %~1
    goto :error
)

:: -- Step 1: Conan install (skip if toolchain exists) --
echo === Step 1: Conan install (windows-x64) ===
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:h conan/profiles/windows-x64 -pr:b conan/profiles/windows-x64 --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 2: CMake configure (always, picks up new/removed sources) --
echo === Step 2: CMake configure (%VARIANT%) ===
cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_ENABLE_CPPCHECK=ON -DJCE_BUILD_VARIANT=%VARIANT%
if errorlevel 1 goto :error

:: -- Step 3: Build (Ninja handles incremental) --
echo === Step 3: Build target %TARGET% ===
cmake --build %BUILD_DIR% --target %TARGET%
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\%VARIANT%\%EXE_NAME%" (
    echo ERROR: %EXE_NAME% not found at %BUILD_DIR%\%VARIANT%\%EXE_NAME%
    goto :error
)

set "CPPCHECK_XML=%BUILD_DIR%\reports\cppcheck-report.xml"
if exist "%CPPCHECK_XML%" (
    echo [INFO] cppcheck report: %CPPCHECK_XML%
) else (
    echo [WARN] cppcheck report not found. Ensure cppcheck is installed and discoverable by CMake.
)

echo.
echo [SUCCESS] Desktop build complete (%VARIANT%): %BUILD_DIR%\%VARIANT%\%EXE_NAME%
popd 2>nul
call "%~dp0lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd 2>nul
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
