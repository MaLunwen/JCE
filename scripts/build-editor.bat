@echo off
:: ================================================================
:: build-editor.bat -- Build standalone jce_editor.exe for Windows x64
:: Usage: build-editor.bat [--clean] [--dist]
:: Output: build\desktop\windows-x64\release\jce_editor.exe
::         build\desktop\windows-x64\dist\jce_editor.exe  (with --dist)
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-conan"
set "BUILD_DIR=build\desktop\windows-x64"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "PROFILE=conan/profiles/windows-x64"
set "VARIANT=release"
set "COMMON=%~dp0lib\jce_build_common.bat"

:: -- Parse arguments --
:parse_args
if /i "%~1"=="--clean" (
    call "%COMMON%" clean "%BUILD_DIR%" "%CONAN_DIR%"
    shift
    goto :parse_args
)
if /i "%~1"=="--dist" (
    set "VARIANT=dist"
    shift
    goto :parse_args
)

:: -- Step 1: Conan install (skip if toolchain exists) --
echo === Step 1: Conan install (windows-x64) ===
call "%COMMON%" conan "%PROFILE%" "%PROFILE%" "%CONAN_DIR%" "%TOOLCHAIN%" || goto :error

:: -- Step 2: CMake configure --
echo === Step 2: CMake configure (%VARIANT%) ===
cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_BUILD_VARIANT=%VARIANT%
if errorlevel 1 goto :error

:: -- Step 3: Build editor target --
echo === Step 3: Build JCE_Editor ===
cmake --build %BUILD_DIR% --target JCE_Editor
if errorlevel 1 goto :error

call "%COMMON%" verify "%BUILD_DIR%\%VARIANT%\jce_editor.exe" "jce_editor.exe" || goto :error

echo.
echo [SUCCESS] Editor build complete (%VARIANT%): %BUILD_DIR%\%VARIANT%\jce_editor.exe
popd
call "%~dp0lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
