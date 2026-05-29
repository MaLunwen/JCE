@echo off
:: ================================================================
:: build-windows-x86.bat -- Cross-compile JCE for Windows 32-bit
:: Usage: build-windows-x86.bat [--clean] [--dist]
:: Output: build\desktop\windows-x86\release\caged_kingdom.exe
::         build\desktop\windows-x86\dist\caged_kingdom.exe  (with --dist)
::
:: Prerequisites:
::   Visual Studio with "MSVC v143 - VS 2022 C++ x86/x64 build tools"
::   (Default install on x64 host already ships the x86 cross-builder.)
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\desktop\windows-x86-conan"
set "BUILD_DIR=build\desktop\windows-x86"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "HOST_PAK=build\host\tools\jce_pak.exe"
set "VARIANT=release"

echo ================================================================
echo   JCE Windows x86 (32-bit) Cross-Build
echo ================================================================
echo.

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

:: -- Step 1: Activate MSVC amd64_x86 cross-compiler --
echo === Step 1: Activate MSVC amd64_x86 cross-compiler ===
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere not found. Install Visual Studio.
    goto :error
)

set "VS_PATH="
for /f "delims=" %%I in ('"%VSWHERE%" -all -property installationPath') do (
    if not defined VS_PATH (
        for /d %%M in ("%%I\VC\Tools\MSVC\*") do (
            if exist "%%M\bin\Hostx64\x86\cl.exe" (
                set "VS_PATH=%%I"
            )
        )
    )
)

if not defined VS_PATH (
    echo ERROR: No Visual Studio installation has x86 build tools.
    echo   Install Individual Component "MSVC v143 - VS 2022 C++ x86/x64 build tools".
    goto :error
)
echo   Found x86 tools in: !VS_PATH!

set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat"
echo   Activating amd64_x86 cross-compiler...
call "%VCVARS%" amd64_x86
if errorlevel 1 (
    echo ERROR: Failed to activate x86 cross-compiler.
    goto :error
)
echo   OK

:: -- Step 2: Ensure host jce_pak exists --
echo === Step 2: Resolve host jce_pak ===
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
echo === Step 4: Conan install (windows-x86) ===

set "CONAN_HOOKS_DIR=%USERPROFILE%\.conan2\extensions\hooks"
if not exist "%CONAN_HOOKS_DIR%" mkdir "%CONAN_HOOKS_DIR%"
for %%F in (conan\hooks\hook_*.py) do copy /Y "%%F" "%CONAN_HOOKS_DIR%\" >nul
echo   Synced hooks -> %CONAN_HOOKS_DIR%

if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:b conan/profiles/windows-x64 -pr:h conan/profiles/windows-x86 --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 5: CMake configure --
echo === Step 5: CMake configure (windows-x86) ===
for %%F in ("%TOOLCHAIN%") do set "TOOLCHAIN=%%~fF"
set "CMAKE_ARGS=-S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_PAK_EXECUTABLE=%HOST_PAK% -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=%VARIANT%"
if defined HOST_SHADERC set "CMAKE_ARGS=%CMAKE_ARGS% -DJCE_SHADERC_EXECUTABLE=%HOST_SHADERC%"
cmake %CMAKE_ARGS%
if errorlevel 1 goto :error

:: -- Step 6: Build --
echo === Step 6: Build ===
cmake --build %BUILD_DIR%
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\%VARIANT%\caged_kingdom.exe" (
    echo ERROR: caged_kingdom.exe not found after build
    goto :error
)

echo.
echo ================================================================
echo   [SUCCESS] Windows x86 build complete (%VARIANT%):
echo     %BUILD_DIR%\%VARIANT%\caged_kingdom.exe
echo ================================================================
popd
call "%~dp0lib\jce_finish.bat" success
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
