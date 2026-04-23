@echo off
:: ================================================================
:: build-desktop.bat -- Build standalone caged_kingdom.exe for Windows x64
:: Usage: build-desktop.bat [--clean] [--dist]
:: Output: build\desktop\windows-x64\release\caged_kingdom.exe
::         build\desktop\windows-x64\dist\caged_kingdom.exe  (with --dist)
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-conan"
set "BUILD_DIR=build\desktop\windows-x64"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "VARIANT=release"

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
echo === Step 3: Build ===
cmake --build %BUILD_DIR%
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\%VARIANT%\caged_kingdom.exe" (
    echo ERROR: caged_kingdom.exe not found after build
    goto :error
)

set "CPPCHECK_XML=%BUILD_DIR%\reports\cppcheck-report.xml"
if exist "%CPPCHECK_XML%" (
    echo [INFO] cppcheck report: %CPPCHECK_XML%
) else (
    echo [WARN] cppcheck report not found. Ensure cppcheck is installed and discoverable by CMake.
)

echo.
echo [SUCCESS] Desktop build complete (%VARIANT%): %BUILD_DIR%\%VARIANT%\caged_kingdom.exe
popd
powershell -NoProfile -Command "$t=5;$e=0;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if($e -ge $t){Write-Host '';break};Write-Host \"`r[SUCCESS] Auto-closing in $($t-$e)s... (:q to quit) \" -NoNewline;Start-Sleep 1;$e++}"
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
powershell -NoProfile -Command "$t=15;$e=0;$p=$false;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if(-not $p){$p=$true;Write-Host \"`r[FAILED] Paused - type :q to quit.                                        \" -NoNewline};if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if(-not $p){if($e -ge $t){Write-Host '';break};Write-Host \"`r[FAILED] Auto-closing in $($t-$e)s... (any key to pause, :q to quit) \" -NoNewline;Start-Sleep 1;$e++}else{Start-Sleep -Milliseconds 100}}"
exit /b 1
