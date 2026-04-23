@echo off
:: ================================================================
:: build-web.bat -- Cross-compile JCE for WebAssembly (Emscripten)
:: Usage: build-web.bat [emsdk_path] [--clean] [--dist]
:: Output: build\web\wasm\release\caged_kingdom.js, caged_kingdom.wasm, caged_kingdom.html
::         build\web\wasm\dist\caged_kingdom.js  (with --dist)
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\web\wasm-conan"
set "BUILD_DIR=build\web\wasm"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "HOST_PAK=build\host\tools\jce_pak.exe"

:: -- Parse arguments --
set "EMSDK_PATH="
set "DO_CLEAN=0"
set "VARIANT=release"
for %%A in (%*) do (
    if /i "%%~A"=="--clean" (
        set "DO_CLEAN=1"
    ) else if /i "%%~A"=="--dist" (
        set "VARIANT=dist"
    ) else if not defined EMSDK_PATH (
        set "EMSDK_PATH=%%~A"
    )
)
if "%EMSDK_PATH%"=="" set "EMSDK_PATH=D:\Code\C_CPP\cross_platform\emsdk"

:: -- Handle --clean flag --
if "%DO_CLEAN%"=="1" (
    echo === Cleaning build directory ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    echo   Done
)

:: -- Step 1: Ensure host jce_pak exists --
echo === Step 1: Resolve host jce_pak ===
if not exist "%HOST_PAK%" (
    echo   Host jce_pak not found - building...
    call "%~dp0build-host-tools.bat"
    if errorlevel 1 goto :error
)
if not exist "%HOST_PAK%" (
    echo ERROR: jce_pak.exe still not found after host build
    goto :error
)
for %%F in ("%HOST_PAK%") do set "HOST_PAK=%%~fF"
echo   %HOST_PAK%

:: -- Step 2: Validate emsdk --
echo === Step 2: Validate emsdk ===
if not exist "%EMSDK_PATH%" (
    echo ERROR: emsdk not found at: %EMSDK_PATH%
    goto :error
)
set "EMSDK=%EMSDK_PATH%"
set "EMSCRIPTEN=%EMSDK_PATH%\upstream\emscripten"

set "EMSCRIPTEN_CMAKE=%EMSDK_PATH%\upstream\emscripten\cmake\Modules\Platform\Emscripten.cmake"
if not exist "%EMSCRIPTEN_CMAKE%" (
    echo ERROR: Emscripten.cmake not found: %EMSCRIPTEN_CMAKE%
    echo Run: %EMSDK_PATH%\emsdk install latest ^&^& %EMSDK_PATH%\emsdk activate latest
    goto :error
)

set "EMSCRIPTEN_CMAKE_UNIX=%EMSCRIPTEN_CMAKE:\=/%"
set "EMCC=%EMSDK_PATH%\upstream\emscripten\emcc.bat"
set "EMPP=%EMSDK_PATH%\upstream\emscripten\em++.bat"
set "EMCC_UNIX=%EMCC:\=/%"
set "EMPP_UNIX=%EMPP:\=/%"
echo   EMSDK = %EMSDK_PATH%

:: -- Step 3: Locate host shaderc --
echo === Step 3: Locate host shaderc ===
set "HOST_SHADERC="
set "_BGFX_DATA=build\host-conan\build\Release\generators\bgfx-release-x86_64-data.cmake"
if exist "%_BGFX_DATA%" (
    for /f "delims=" %%L in ('findstr /b /c:"set(bgfx_PACKAGE_FOLDER_RELEASE" "%_BGFX_DATA%"') do (
        set "_LINE=%%L"
    )
)
if defined _LINE (
    set "_BGFX_DIR=!_LINE:~33,-2!"
    if exist "!_BGFX_DIR!\bin\shaderc.exe" set "HOST_SHADERC=!_BGFX_DIR!\bin\shaderc.exe"
)
if defined HOST_SHADERC (
    echo   !HOST_SHADERC!
) else (
    echo   WARNING: host shaderc.exe not found - shaders will not be compiled.
)

:: -- Step 4: Conan install (skip if toolchain exists) --
echo === Step 4: Conan install (wasm) ===
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:b conan/profiles/windows-x64 -pr:h conan/profiles/wasm -c "tools.cmake.cmaketoolchain:user_toolchain=['%EMSCRIPTEN_CMAKE_UNIX%']" -c "tools.build:compiler_executables={'c': '%EMCC_UNIX%', 'cpp': '%EMPP_UNIX%'}" --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 5: CMake configure --
echo === Step 5: CMake configure (wasm) ===
for %%F in ("%TOOLCHAIN%") do set "TOOLCHAIN=%%~fF"
set "CMAKE_ARGS=-S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_PAK_EXECUTABLE=%HOST_PAK% -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=%VARIANT%"
if defined HOST_SHADERC set "CMAKE_ARGS=%CMAKE_ARGS% -DJCE_SHADERC_EXECUTABLE=%HOST_SHADERC%"
cmake %CMAKE_ARGS%
if errorlevel 1 goto :error

:: -- Step 6: Build (Ninja handles incremental) --
echo === Step 6: Build (wasm) ===
cmake --build %BUILD_DIR%
if errorlevel 1 goto :error

echo.
echo [SUCCESS] Web build complete (%VARIANT%):
echo   %BUILD_DIR%\%VARIANT%\caged_kingdom.html
echo   %BUILD_DIR%\%VARIANT%\caged_kingdom.js
echo   %BUILD_DIR%\%VARIANT%\caged_kingdom.wasm
echo.
echo   To run: python scripts\serve-web.py
echo   Then open http://localhost:8080
popd
powershell -NoProfile -Command "$t=5;$e=0;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if($e -ge $t){Write-Host '';break};Write-Host \"`r[SUCCESS] Auto-closing in $($t-$e)s... (:q to quit) \" -NoNewline;Start-Sleep 1;$e++}"
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
powershell -NoProfile -Command "$t=15;$e=0;$p=$false;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if(-not $p){$p=$true;Write-Host \"`r[FAILED] Paused - type :q to quit.                                        \" -NoNewline};if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if(-not $p){if($e -ge $t){Write-Host '';break};Write-Host \"`r[FAILED] Auto-closing in $($t-$e)s... (any key to pause, :q to quit) \" -NoNewline;Start-Sleep 1;$e++}else{Start-Sleep -Milliseconds 100}}"
exit /b 1
