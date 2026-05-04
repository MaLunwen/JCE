@echo off
:: ================================================================
:: package-jni-jar.bat -- Build JCE JNI native and package fat JAR
:: Usage: package-jni-jar.bat [java_home] [--clean] [--fat] [--dist]
::   java_home - Path to JDK (default: JAVA_HOME env or D:\Java21\openjdk-8)
::   --clean   - Force full rebuild (remove CMake + Conan caches)
::   --fat     - Skip native build, only re-package fat JAR from natives
::   --dist    - Distribution build (Tracy OFF, optimised)
::
:: Default: build host native + package ALL available natives into fat JAR.
:: To add other platforms, copy their .dll/.so/.dylib into:
::   build\jni\natives\<classifier>\
:: Classifiers: win32-x86_64, win32-aarch64,
::   linux-x86_64, linux-aarch64, darwin-x86_64, darwin-aarch64
::
:: Output: build\jni\<variant>\jce-jni.jar  (fat JAR with all available natives)
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

:: -- Parse arguments (flags can appear in any position) --
set "JAVA_DIR="
set "DO_CLEAN=0"
set "DO_FAT=0"
set "VARIANT=release"
for %%A in (%*) do (
    if /i "%%~A"=="--clean" (
        set "DO_CLEAN=1"
    ) else if /i "%%~A"=="--fat" (
        set "DO_FAT=1"
    ) else if /i "%%~A"=="--dist" (
        set "VARIANT=dist"
    ) else if not defined JAVA_DIR (
        set "JAVA_DIR=%%~A"
    )
)

:: Detect host classifier
set "CLASSIFIER=win32-x86_64"
set "LIB_NAME=jce.dll"

:: Resolve Java home
if "%JAVA_DIR%"=="" (
    if defined JAVA_HOME (
        if exist "%JAVA_HOME%\bin\javac.exe" set "JAVA_DIR=%JAVA_HOME%"
    )
)
if "%JAVA_DIR%"=="" (
    set "JAVA_DIR=D:\Java21\openjdk-8"
)
if not exist "%JAVA_DIR%\bin\javac.exe" (
    echo ERROR: javac.exe not found in %JAVA_DIR%\bin
    echo Set JAVA_HOME or pass java_home as first argument.
    goto :error
)
if not exist "%JAVA_DIR%\bin\jar.exe" (
    echo ERROR: jar.exe not found in %JAVA_DIR%\bin
    goto :error
)
echo Using Java: %JAVA_DIR%

:: Shared directories under build\jni
set "JNI_ROOT=build\jni"
set "NATIVES_DIR=%JNI_ROOT%\natives"
set "DIST_DIR=%JNI_ROOT%\%VARIANT%"

:: ================================================================
:: Fat JAR mode: assemble from pre-built natives
:: ================================================================
if "%DO_FAT%"=="1" goto :fat_jar

:: ================================================================
:: Normal mode: build for host platform + package
:: ================================================================
set "CONAN_DIR=%JNI_ROOT%\desktop-conan"
set "BUILD_DIR=%JNI_ROOT%\desktop"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "DLL_PATH=%BUILD_DIR%\%VARIANT%\%LIB_NAME%"
set "PROFILE=conan/profiles/windows-x64"

echo.
echo ================================================================
echo   JCE JNI Build (host: %CLASSIFIER%, variant: %VARIANT%)
echo ================================================================

:: -- Handle --clean flag --
if "%DO_CLEAN%"=="1" (
    echo === Full clean ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    echo   Done
    echo.
)

:: -- Step 1: Conan install (skip if toolchain exists) --
echo === Step 1: Conan install ===
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:h %PROFILE% -pr:b %PROFILE% --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 2: CMake configure --
echo === Step 2: CMake configure ===
cmake -S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_BUILD_JNI=ON -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=%VARIANT%
if errorlevel 1 goto :error

:: -- Step 3: Build (Ninja handles incremental) --
echo === Step 3: Build ===
cmake --build %BUILD_DIR%
if errorlevel 1 goto :error

if not exist "%DLL_PATH%" (
    echo ERROR: JNI native not found: %DLL_PATH%
    goto :error
)
echo Built: %DLL_PATH%

:: -- Step 4: Auto-stage native --
echo === Step 4: Auto-stage native ===
set "STAGE_NATIVE_DIR=%NATIVES_DIR%\%CLASSIFIER%"
if not exist "%STAGE_NATIVE_DIR%" mkdir "%STAGE_NATIVE_DIR%"
copy /y "%DLL_PATH%" "%STAGE_NATIVE_DIR%\%LIB_NAME%" >nul
if errorlevel 1 goto :error
echo   Staged: %STAGE_NATIVE_DIR%\%LIB_NAME%

:: -- Fall through to fat JAR packaging --

:: ================================================================
:: Fat JAR: collect all natives from build\jni\natives into one JAR
:: ================================================================
:fat_jar
echo.
echo ================================================================
echo   JCE Fat JAR Assembly
echo ================================================================

:: Ensure all platform directories exist
for %%C in (win32-x86_64 win32-aarch64 linux-x86_64 linux-aarch64 darwin-x86_64 darwin-aarch64) do (
    if not exist "%NATIVES_DIR%\%%C" mkdir "%NATIVES_DIR%\%%C"
)

:: Count platforms that have actual files
set "NATIVE_COUNT=0"
for /d %%D in ("%NATIVES_DIR%\*") do (
    set "_HAS=0"
    for %%F in ("%%D\*") do set "_HAS=1"
    if !_HAS!==1 (
        set /a NATIVE_COUNT+=1
        echo   Found: %%~nxD
    )
)
if %NATIVE_COUNT%==0 (
    echo ERROR: No native libraries found in %NATIVES_DIR%
    echo   Build first or copy natives into the platform directories.
    goto :error
)
echo   Platforms with natives: %NATIVE_COUNT%
echo.

:: Prepare staging
set "STAGING=%JNI_ROOT%\staging"
set "CLASSES_DIR=%STAGING%\classes"

if exist "%STAGING%" rmdir /s /q "%STAGING%"
mkdir "%CLASSES_DIR%" 2>nul
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"

:: Compile Java sources
echo === Compile Java sources ===
"%JAVA_DIR%\bin\javac.exe" -d "%CLASSES_DIR%" engine/java/com/jce/JceRuntime.java engine/java/com/jce/Main.java
if errorlevel 1 goto :error

:: Copy natives (only dirs with files)
echo === Copy natives ===
for /d %%D in ("%NATIVES_DIR%\*") do (
    set "_HAS=0"
    for %%F in ("%%D\*") do set "_HAS=1"
    if !_HAS!==1 (
        set "CLS=%%~nxD"
        mkdir "%CLASSES_DIR%\natives\!CLS!" 2>nul
        copy /y "%%D\*" "%CLASSES_DIR%\natives\!CLS!\" >nul
        echo   Packed: natives/!CLS!/
    )
)

:: Build universal PAK (all shader variants: dx11 + mtl + spv + glsl + ...)
echo === Build universal PAK ===
set "GAME_PAK="
call :try_universal_pak
echo.

:: Copy shared game_assets.pak — prefer universal, fall back to platform-specific
echo === Copy shared PAK ===
if not defined GAME_PAK (
    :: Universal PAK not available; search for any pre-built PAK
    for %%P in (
        "%REPO_ROOT%\build\jni\desktop\release\game_assets.pak"
        "%REPO_ROOT%\build\jni\desktop\game_assets.pak"
        "%REPO_ROOT%\build\desktop\windows-x64\release\game_assets.pak"
        "%REPO_ROOT%\build\desktop\windows-x64\game_assets.pak"
        "%REPO_ROOT%\build\jni\macos-x64\release\game_assets.pak"
        "%REPO_ROOT%\build\jni\macos-arm64\release\game_assets.pak"
        "%REPO_ROOT%\build\jni\linux-x64\release\game_assets.pak"
        "%REPO_ROOT%\build\jni\linux-arm64\release\game_assets.pak"
        "%REPO_ROOT%\build\desktop\macos-x64\release\game_assets.pak"
        "%REPO_ROOT%\build\desktop\linux-x64\release\game_assets.pak"
    ) do (
        if not defined GAME_PAK if exist %%P set "GAME_PAK=%%~P"
    )
)
if defined GAME_PAK (
    copy /y "%GAME_PAK%" "%CLASSES_DIR%\game_assets.pak" >nul
    echo   Packed: game_assets.pak [from %GAME_PAK%]
) else (
    echo   WARNING: game_assets.pak not found, JAR will not include assets.
    echo   Build the game first to generate game_assets.pak.
)

:: Package fat JAR
echo === Package fat JAR ===
set "FAT_JAR=%DIST_DIR%\jce-jni.jar"
if exist "%FAT_JAR%" del /q "%FAT_JAR%"

"%JAVA_DIR%\bin\jar.exe" cfe "%FAT_JAR%" com.jce.Main -C "%CLASSES_DIR%" .
if errorlevel 1 goto :error

echo.
echo [SUCCESS] %FAT_JAR% (%VARIANT%)
echo   Platforms: %NATIVE_COUNT%
echo   Run: %JAVA_DIR%\bin\java.exe -jar %FAT_JAR% 5
popd
call "%~dp0lib\jce_finish.bat" success
exit /b 0

::
:: ================================================================
:: Subroutine: build a universal PAK containing all shader variants
:: (dx11 + mtl + spv + glsl + ...) so the fat JAR works on all
:: desktop platforms without a platform-specific rebuild.
::
:: Sets GAME_PAK to the built PAK path on success.
:: Leaves GAME_PAK unset on failure (caller falls back to pre-built).
:: ================================================================
:try_universal_pak
set "_PAK_EXE="
if exist "%REPO_ROOT%\build\jni\desktop\tools\jce_pak.exe" set "_PAK_EXE=%REPO_ROOT%\build\jni\desktop\tools\jce_pak.exe"
if not defined _PAK_EXE if exist "%REPO_ROOT%\build\host\tools\jce_pak.exe" set "_PAK_EXE=%REPO_ROOT%\build\host\tools\jce_pak.exe"
if not defined _PAK_EXE (
    echo   jce_pak.exe not found - using pre-built PAK.
    echo   Hint: run without --fat first to build jce_pak.exe.
    goto :eof
)

:: Fetch Metal shaders from macOS (optional — failure is non-fatal)
echo   Fetching Metal shaders from macOS...
python "%REPO_ROOT%\scripts\sync_macos.py" --fetch-shaders >nul 2>&1
if not errorlevel 1 (
    echo   Metal shaders fetched.
) else (
    echo   WARNING: Metal shader fetch skipped (Mac offline or paramiko not installed).
    echo   Copy *_mtl.bin shaders to engine\resources\assets\shaders\ for macOS Metal support.
)

:: Prepare icon staging directory (jce_pak needs the icon as a resource)
set "_ICON_STAGE=%REPO_ROOT%\build\jni\desktop\_raw_game_icon"
if not exist "%_ICON_STAGE%" mkdir "%_ICON_STAGE%"
copy /y "%REPO_ROOT%\caged_kingdom\resources\CK_icon.png" "%_ICON_STAGE%\CK_icon.png" >nul 2>&1

:: Output directory for the universal PAK
set "_UNIV_DIR=%REPO_ROOT%\build\jni\universal"
if not exist "%_UNIV_DIR%" mkdir "%_UNIV_DIR%"

:: Build the PAK — no --exclude-suffix flags so every shader variant
:: present on disk (dx11, mtl, spv, glsl, ...) is included.
echo   Building universal PAK (all shader variants)...
"%_PAK_EXE%" ^
    --resource-dir "%REPO_ROOT%\engine\resources\assets" ^
    --resource-dir "%REPO_ROOT%\engine\ui" ^
    --resource-dir "%REPO_ROOT%\caged_kingdom\resources\assets" ^
    --resource-dir "%REPO_ROOT%\caged_kingdom\ui" ^
    --resource-dir "%_ICON_STAGE%" ^
    --exclude-segment raw_assets ^
    --level 19 ^
    --pak-file      "%_UNIV_DIR%\game_assets.pak" ^
    --header-file   "%_UNIV_DIR%\game_embedded_assets.h" ^
    --manifest-file "%_UNIV_DIR%\_game_assets_manifest.cmake" ^
    --obj-format    none
if not errorlevel 1 (
    echo   Universal PAK built: %_UNIV_DIR%\game_assets.pak
    set "GAME_PAK=%_UNIV_DIR%\game_assets.pak"
) else (
    echo   WARNING: jce_pak exited with error.
    if exist "%_UNIV_DIR%\game_assets.pak" (
        echo   Reusing previously built universal PAK.
        set "GAME_PAK=%_UNIV_DIR%\game_assets.pak"
    ) else (
        echo   No universal PAK available - will use platform-specific PAK.
    )
)
goto :eof

:error
echo.
echo [FAILED] Build failed.
popd
call "%~dp0lib\jce_finish.bat" fail
exit /b 1
