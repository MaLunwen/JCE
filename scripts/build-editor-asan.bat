@echo off
:: ================================================================
:: build-editor-asan.bat -- Build jce_editor.exe with AddressSanitizer
::
:: ASan catches:
::   * use-after-free
::   * heap/stack out-of-bounds
::   * double-free
::   * memory leaks (with ASAN_OPTIONS=detect_leaks=1)
::   * Best for diagnosing your 0xC0000005 ACCESS_VIOLATION crash.
::
:: Notes:
::   * Uses Release-build Conan deps (/MD), kept fast (~Release perf).
::   * VS 2026 has built-in ASan support. Just F5 -> if violation hits,
::     VS pops a window with allocation/free/access stacks.
::
::   Output: build\desktop\windows-x64-asan\asan\jce_editor.exe
::
:: Usage: build-editor-asan.bat [--clean]
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x64-conan"
set "BUILD_DIR=build\desktop\windows-x64-asan"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "PROFILE=conan/profiles/windows-x64"

:parse_args
if /i "%~1"=="--clean" (
    echo === Cleaning ASan build directory ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    echo   Done
    shift
    goto :parse_args
)

echo === Step 1: Conan install (reuses windows-x64 release deps) ===
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping.
) else (
    conan install . -pr:h %PROFILE% -pr:b %PROFILE% --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

echo === Step 2: CMake configure (Release + Zi + ASan) ===
cmake -S . -B %BUILD_DIR% -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DJCE_BUILD_VARIANT=asan ^
    -DJCE_ENABLE_CPPCHECK=OFF ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    "-DCMAKE_C_FLAGS_RELEASE=/MD /O1 /Ob1 /Zi /DNDEBUG /fsanitize=address /Oy- /FS /D_DISABLE_STRING_ANNOTATION=1 /D_DISABLE_VECTOR_ANNOTATION=1" ^
    "-DCMAKE_CXX_FLAGS_RELEASE=/MD /O1 /Ob1 /Zi /DNDEBUG /fsanitize=address /Oy- /FS /D_DISABLE_STRING_ANNOTATION=1 /D_DISABLE_VECTOR_ANNOTATION=1" ^
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG:FULL /INCREMENTAL:NO" ^
    "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=/DEBUG:FULL /INCREMENTAL:NO"
if errorlevel 1 goto :error

echo === Step 3: Build JCE_Editor with ASan ===
cmake --build %BUILD_DIR% --target JCE_Editor
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\asan\jce_editor.exe" (
    echo ERROR: jce_editor.exe not found after build
    goto :error
)

echo.
echo [SUCCESS] ASan editor build complete:
echo   EXE: %BUILD_DIR%\asan\jce_editor.exe
echo.
echo Run with leak detection:
echo   set ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:print_stats=1
echo   cd %BUILD_DIR%\asan
echo   jce_editor.exe
popd
powershell -NoProfile -Command "$t=5;$e=0;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if($e -ge $t){Write-Host '';break};Write-Host \"`r[SUCCESS] Auto-closing in $($t-$e)s... (:q to quit) \" -NoNewline;Start-Sleep 1;$e++}"
exit /b 0

:error
echo.
echo [FAILED] ASan build failed.
popd
powershell -NoProfile -Command "$t=15;$e=0;$p=$false;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if(-not $p){$p=$true;Write-Host \"`r[FAILED] Paused - type :q to quit.                                        \" -NoNewline};if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if(-not $p){if($e -ge $t){Write-Host '';break};Write-Host \"`r[FAILED] Auto-closing in $($t-$e)s... (any key to pause, :q to quit) \" -NoNewline;Start-Sleep 1;$e++}else{Start-Sleep -Milliseconds 100}}"
exit /b 1
