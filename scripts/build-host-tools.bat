@echo off
:: ================================================================
:: build-host-tools.bat -- Build host jce_pak.exe (prerequisite for cross-compilation)
:: Usage: build-host-tools.bat [--clean]
:: Output: build\host\src\tools\jce_pak.exe
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

set "CONAN_DIR=build\host-conan"
set "BUILD_DIR=build\host"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"

:: -- Handle --clean flag --
if /i "%~1"=="--clean" (
    echo === Cleaning build directory ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    echo   Done
)

:: -- Step 1: Conan install (skip if toolchain exists) --
echo === Step 1: Conan install (windows-x64 host) ===
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

:: -- Step 2: CMake configure --
echo === Step 2: CMake configure ===
cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_ENABLE_CPPCHECK=OFF
if errorlevel 1 goto :error

:: -- Step 3: Build jce_pak --
echo === Step 3: Build jce_pak ===
cmake --build %BUILD_DIR% --target jce_pak
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\src\tools\jce_pak.exe" (
    echo ERROR: jce_pak.exe not found after build
    goto :error
)

echo.
echo [SUCCESS] Host jce_pak ready: %BUILD_DIR%\src\tools\jce_pak.exe
popd
timeout /t 5 /nobreak >nul
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
pause
exit /b 1
