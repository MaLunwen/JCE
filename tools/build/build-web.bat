@echo off
:: ================================================================
:: build-web.bat -- Cross-compile JCE for WebAssembly (Emscripten)
:: Usage: build-web.bat [emsdk_path] [--clean] [--dist]
:: Output: build\web\wasm\release\<project>.js, <project>.wasm, <project>.html
::         build\web\wasm\dist\<project>.js  (with --dist)
:: <project> is the explicit JCE_GAME_PROJECT_DIR consumer selection.
:: ================================================================
setlocal enabledelayedexpansion

:: Which in-tree project this script packages.  The engine is general;
:: whatever sits in this slot is not, so it is a variable rather than a
:: spelled-out name.  Same slot as CMake's JCE_GAME_PROJECT_DIR.

set "REPO_ROOT=%~dp0..\.."
pushd "%REPO_ROOT%" || goto :error
if not defined JCE_GAME_PROJECT_DIR (
    echo ERROR: Set JCE_GAME_PROJECT_DIR to the consumer project path.
    popd
    exit /b 2
)

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
:: Resolve emsdk WITHOUT naming one machine's disk.  In order: the argument
:: above, $EMSDK (what emsdk_env exports), then the emcc wrapper already on
:: PATH walked back to its checkout (emsdk_env puts
:: <emsdk>\upstream\emscripten on PATH, so emcc.bat names the root), then a
:: checkout beside this repository.  scripts\jce.py find_emsdk() resolves the
:: same list in the same order.
if "%EMSDK_PATH%"=="" if defined EMSDK set "EMSDK_PATH=%EMSDK%"
if "%EMSDK_PATH%"=="" (
    for %%I in (emcc.bat) do if not "%%~$PATH:I"=="" set "EMSDK_PATH=%%~dp$PATH:I..\.."
)
if "%EMSDK_PATH%"=="" if exist "%REPO_ROOT%\..\emsdk\upstream\emscripten" set "EMSDK_PATH=%REPO_ROOT%\..\emsdk"
if "%EMSDK_PATH%"=="" if exist "%REPO_ROOT%\..\cross_platform\emsdk\upstream\emscripten" set "EMSDK_PATH=%REPO_ROOT%\..\cross_platform\emsdk"
if "%EMSDK_PATH%"=="" (
    echo ERROR: emsdk not found.
    echo   Activate it ^(emsdk_env^), set EMSDK, pass the path as an argument,
    echo   or put a checkout beside this repo as ..\emsdk.
    goto :error
)

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
    REM Call jce.py directly (not the build-host-tools shim) so this orchestrator
    REM keeps its own single beep + ":q" footer instead of nesting a second one.
    python "%~dp0jce.py" host-tools
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
:: Shared with build-android.bat -- see the note there for why reading
:: build\host-conan's generator file was wrong.  "require" because the
:: engine-shaders pak is the same CMake target on every platform, and on
:: Android a missing shaderc was measured to pack zero files and fail with
:: a BOM integrity error hundreds of lines later.
call "%~dp0jce_build_common.bat" shaderc require || goto :error
set "HOST_SHADERC=%SHADERC_EXE%"

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
:: Report what was ACTUALLY produced, by globbing.  These three lines named a
:: project until 2026-08-27 -- and named the WRONG one: this build emits
:: science_lab.*, so the "success" message pointed at files that had never
:: been there.  A glob cannot be wrong about its own output directory.
for %%F in ("%BUILD_DIR%\%VARIANT%\*.html") do echo   %%~F
for %%F in ("%BUILD_DIR%\%VARIANT%\*.js")   do echo   %%~F
for %%F in ("%BUILD_DIR%\%VARIANT%\*.wasm") do echo   %%~F
echo.
echo   To run: python scripts\serve-web.py
echo   Then open http://localhost:8080
popd
call "%~dp0..\..\scripts\lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
call "%~dp0..\..\scripts\lib\jce_finish.bat" fail
exit /b 1
