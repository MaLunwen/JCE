@echo off
:: ================================================================
:: build-android.bat -- Cross-compile JCE for Android and package APK
:: Usage: build-android.bat [ndk_path] [sdk_path] [arch] [--clean]
::   ndk_path  - Android NDK root (default: D:\Code\C_CPP\cross_platform\android-ndk-r27d)
::   sdk_path  - Android SDK root (default: ANDROID_HOME or D:\Code\C_CPP\cross_platform\android-sdk)
::   arch      - arm64 or arm (default: arm64)
::   --clean   - Force full rebuild (remove CMake + Conan caches)
:: Output: scripts\android\app\build\outputs\apk\debug\app-debug.apk
:: ================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || goto :error

:: -- Parse arguments (--clean can appear in any position) --
set "NDK_PATH="
set "SDK_PATH="
set "ARCH="
set "DO_CLEAN=0"
for %%A in (%*) do (
    if /i "%%~A"=="--clean" (
        set "DO_CLEAN=1"
    ) else if not defined NDK_PATH (
        set "NDK_PATH=%%~A"
    ) else if not defined SDK_PATH (
        set "SDK_PATH=%%~A"
    ) else if not defined ARCH (
        set "ARCH=%%~A"
    )
)
if "%NDK_PATH%"=="" set "NDK_PATH=D:\Code\C_CPP\cross_platform\android-ndk-r27d"
if "%SDK_PATH%"=="" set "SDK_PATH=%ANDROID_HOME%"
if "%SDK_PATH%"=="" set "SDK_PATH=%ANDROID_SDK_ROOT%"
if "%SDK_PATH%"=="" set "SDK_PATH=D:\Code\C_CPP\cross_platform\android-sdk"
if "%ARCH%"=="" set "ARCH=arm64"

:: Validate arch and set ABI
if /i "%ARCH%"=="arm64" (
    set "ABI=arm64-v8a"
    set "NDK_TRIPLE=aarch64-linux-android"
) else if /i "%ARCH%"=="arm" (
    set "ABI=armeabi-v7a"
    set "NDK_TRIPLE=arm-linux-androideabi"
) else (
    echo ERROR: Invalid arch "%ARCH%". Use arm64 or arm.
    goto :error
)

:: Java -- Android Gradle requires JDK 11+, force override
set "JAVA_HOME=D:\Java21\openjdk-21"

set "CONAN_DIR=build\mobile\android-%ARCH%-conan"
set "BUILD_DIR=build\mobile\android-%ARCH%"
set "TOOLCHAIN=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "HOST_PAK=build\host\tools\jce_pak.exe"
set "ABI_DIR=scripts\android\app\src\main\jniLibs\%ABI%"
set "APK_COPY_DIR=build\mobile\android-%ARCH%\release"

echo ================================================================
echo   JCE Android Build (%ARCH% / %ABI%)
echo ================================================================
echo   NDK:  %NDK_PATH%
echo   SDK:  %SDK_PATH%
echo   JAVA: %JAVA_HOME%
echo ================================================================
echo.

:: -- Handle --clean flag --
if "%DO_CLEAN%"=="1" (
    echo === Full clean ===
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
    if exist "%CONAN_DIR%" rmdir /s /q "%CONAN_DIR%"
    if exist "scripts\android\app\build" rmdir /s /q "scripts\android\app\build"
    if exist "scripts\android\app\src\main\jniLibs" rmdir /s /q "scripts\android\app\src\main\jniLibs"
    if exist "scripts\android\app\src\main\assets" rmdir /s /q "scripts\android\app\src\main\assets"
    echo   Done
    echo.
)

:: -- Step 0: Smart cleanup (only target ABI + stale Gradle cache) --
echo === Step 0: Smart cleanup ===
:: Remove only the target ABI directory (other ABIs stay if multi-arch)
if exist "%ABI_DIR%" (
    echo   Removing %ABI_DIR%
    rmdir /s /q "%ABI_DIR%"
)
:: Remove Gradle build cache (Gradle cannot detect .so content changes)
if exist "scripts\android\app\build" (
    echo   Removing Gradle build cache
    rmdir /s /q "scripts\android\app\build"
)
echo   OK
echo.

:: -- Step 1: Validate SDK --
echo === Step 1: Validate Android SDK ===
if not exist "%SDK_PATH%" (
    echo ERROR: Android SDK not found at: %SDK_PATH%
    echo.
    echo Setup instructions:
    echo   1. Download cmdline-tools from:
    echo      https://developer.android.com/studio#command-line-tools-only
    echo   2. Extract to: %SDK_PATH%\cmdline-tools\latest\
    echo   3. Run:
    echo      set ANDROID_HOME=%SDK_PATH%
    echo      %%ANDROID_HOME%%\cmdline-tools\latest\bin\sdkmanager "platforms;android-35" "build-tools;35.0.0" "platform-tools"
    goto :error
)
echo   OK

:: -- Step 2: Validate NDK --
echo === Step 2: Validate Android NDK ===
if not exist "%NDK_PATH%" (
    echo ERROR: Android NDK not found at: %NDK_PATH%
    goto :error
)
set "ANDROID_NDK_HOME=%NDK_PATH%"
set "NDK_UNIX=%NDK_PATH:\=/%"
echo   OK

:: -- Step 3: Ensure host jce_pak exists --
echo === Step 3: Resolve host jce_pak ===
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

:: -- Step 4: Locate host shaderc --
echo === Step 4: Locate host shaderc ===
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

:: -- Step 5: Conan install (skip if toolchain exists) --
echo === Step 5: Conan install (android-%ARCH%) ===
if exist "%TOOLCHAIN%" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:b conan/profiles/windows-x64 -pr:h "conan/profiles/android-%ARCH%" -c "user.sdl:android=True" -c "tools.android:ndk_path=%NDK_UNIX%" --output-folder=%CONAN_DIR% --build=missing
    if errorlevel 1 goto :error
)

if not exist "%TOOLCHAIN%" (
    echo ERROR: Conan toolchain not found: %TOOLCHAIN%
    goto :error
)

:: -- Step 6: CMake configure --
echo === Step 6: CMake configure (android-%ARCH%) ===
for %%F in ("%TOOLCHAIN%") do set "TOOLCHAIN=%%~fF"
set "CMAKE_ARGS=-S . -B %BUILD_DIR% -G Ninja -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% -DCMAKE_BUILD_TYPE=Release -DJCE_PAK_EXECUTABLE=%HOST_PAK% -DJCE_ENABLE_CPPCHECK=OFF -DJCE_BUILD_VARIANT=release"
if defined HOST_SHADERC set "CMAKE_ARGS=%CMAKE_ARGS% -DJCE_SHADERC_EXECUTABLE=%HOST_SHADERC%"
cmake %CMAKE_ARGS%
if errorlevel 1 goto :error

:: -- Step 7: Build native library (Ninja handles incremental) --
echo === Step 7: Build native library ===
cmake --build %BUILD_DIR%
if errorlevel 1 goto :error

set "NATIVE_LIB=%BUILD_DIR%\caged_kingdom\libJCE.so"
if not exist "%NATIVE_LIB%" (
    echo ERROR: libJCE.so not found after build
    dir /b /s "%BUILD_DIR%\caged_kingdom\*.so" 2>nul
    goto :error
)
echo   Built: %NATIVE_LIB%

:: -- Step 7b: Strip debug symbols --
echo === Step 7b: Strip debug symbols ===
set "STRIP=%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
if not exist "%STRIP%" (
    echo   WARNING: llvm-strip not found, .so will not be stripped
) else (
    "%STRIP%" --strip-unneeded "%NATIVE_LIB%"
    if errorlevel 1 goto :error
    echo   Stripped: %NATIVE_LIB%
)

:: -- Step 8: Copy native libs to jniLibs --
echo === Step 8: Copy native libs to jniLibs\%ABI% ===
if not exist "%ABI_DIR%" mkdir "%ABI_DIR%"
copy /y "%NATIVE_LIB%" "%ABI_DIR%\libJCE.so" >nul
if errorlevel 1 goto :error
echo   Copied libJCE.so

:: Copy game_assets.pak into APK assets/ (loaded at runtime via SDL)
set "PAK_SRC=%BUILD_DIR%\game_assets.pak"
set "ASSETS_DIR=scripts\android\app\src\main\assets"
if not exist "%PAK_SRC%" (
    echo ERROR: game_assets.pak not found at: %PAK_SRC%
    goto :error
)
if not exist "%ASSETS_DIR%" mkdir "%ASSETS_DIR%"
copy /y "%PAK_SRC%" "%ASSETS_DIR%\game_assets.pak" >nul
if errorlevel 1 goto :error
echo   Copied game_assets.pak to APK assets

:: Copy libc++_shared.so from NDK
set "LIBCPP=%NDK_PATH%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\%NDK_TRIPLE%\libc++_shared.so"
if not exist "%LIBCPP%" (
    echo ERROR: libc++_shared.so not found at: %LIBCPP%
    goto :error
)
copy /y "%LIBCPP%" "%ABI_DIR%\libc++_shared.so" >nul
if errorlevel 1 goto :error
echo   Copied libc++_shared.so

:: -- Step 9: Write local.properties --
echo === Step 9: Write scripts/android/local.properties ===
set "SDK_UNIX=%SDK_PATH:\=/%"
echo sdk.dir=%SDK_UNIX%> scripts\android\local.properties
echo   sdk.dir=%SDK_UNIX%

:: -- Step 10: Gradle assembleDebug --
echo === Step 10: Gradle assembleDebug ===
set "ANDROID_HOME=%SDK_PATH%"
pushd scripts\android
call .\gradlew.bat assembleDebug
if errorlevel 1 (
    popd
    goto :error
)
popd

set "APK=scripts\android\app\build\outputs\apk\debug\app-debug.apk"
if not exist "%APK%" (
    echo ERROR: APK not found at: %APK%
    goto :error
)

:: -- Step 11: Copy APK to build output folder --
echo === Step 11: Copy APK to %APK_COPY_DIR% ===
if not exist "%APK_COPY_DIR%" mkdir "%APK_COPY_DIR%"
copy /y "%APK%" "%APK_COPY_DIR%\JCE.apk" >nul
if errorlevel 1 goto :error
echo   Copied: %APK_COPY_DIR%\JCE.apk

echo.
echo ================================================================
echo   [SUCCESS] APK built: %APK%
echo   [SUCCESS] APK copied: %APK_COPY_DIR%\JCE.apk
echo ================================================================

:: -- Step 12: Auto-install on WSA if connected --
set "ADB=%SDK_PATH%\platform-tools\adb.exe"
if not exist "%ADB%" (
    echo   [SKIP] adb not found at %ADB%, skipping auto-install.
    goto :done
)

echo.
echo === Step 12: Check for WSA / connected device ===
:: Try WSA first (localhost:58526)
"%ADB%" connect 127.0.0.1:58526 >nul 2>&1
"%ADB%" devices | findstr /r "device$" >nul 2>&1
if errorlevel 1 (
    echo   [SKIP] No Android device or WSA detected, skipping install.
    goto :done
)

echo   Device detected, installing APK...
"%ADB%" install -r "%APK_COPY_DIR%\JCE.apk"
if errorlevel 1 (
    echo   [WARN] Install failed.
    goto :done
)
echo   Launching com.jce/.JCEActivity ...
"%ADB%" shell am start -n com.jce/com.jce.JCEActivity
echo   [OK] App installed and launched.

:done
echo.
popd
timeout /t 5 /nobreak >nul
exit /b 0

:error
echo.
echo [FAILED] Build failed.
popd
pause
exit /b 1
