@echo off
:: ================================================================
:: lib/jce_build_common.bat -- shared Windows build subroutines.
::
:: Dispatched by the first argument. Result variables and exit codes are
:: written into the CALLER's environment, so this file intentionally does
:: NOT `setlocal` (that would discard SHADERC_EXE before the caller saw it).
:: Keep internal temporaries prefixed with _JBC_ to avoid clobbering.
::
:: Subcommands:
::   clean   <build_dir> <conan_dir>
::       Remove both directories if they exist.
::   conan   <profile_host> <profile_build> <conan_dir> <toolchain> [extra_args]
::       Run `conan install` unless <toolchain> already exists, then verify
::       the toolchain is present. Optional extra_args (e.g. "-s build_type=Debug")
::       are appended verbatim. exit /b 1 on failure.
::   shaderc
::       Locate bgfx's shaderc.exe in the Conan cache (Debug build-tree copy
::       preferred over the packaged \p\bin copy) and set SHADERC_EXE.
::   verify  <file> [label]
::       exit /b 1 (with an ERROR line) if <file> is missing.
::
:: Usage from a build script:
::   call "%~dp0lib\jce_build_common.bat" conan %PROFILE% %PROFILE% "%CONAN_DIR%" "%TOOLCHAIN%" || goto :error
::   call "%~dp0lib\jce_build_common.bat" shaderc
::   call "%~dp0lib\jce_build_common.bat" verify "%BUILD_DIR%\%VARIANT%\%EXE%" "%EXE%" || goto :error
:: ================================================================

set "_JBC_CMD=%~1"
if /i "%_JBC_CMD%"=="clean"   goto :jbc_clean
if /i "%_JBC_CMD%"=="conan"   goto :jbc_conan
if /i "%_JBC_CMD%"=="shaderc" goto :jbc_shaderc
if /i "%_JBC_CMD%"=="verify"  goto :jbc_verify
echo [jce_build_common] unknown subcommand: %_JBC_CMD%
exit /b 2

:jbc_clean
echo === Cleaning build directories ===
if exist "%~2" rmdir /s /q "%~2"
if exist "%~3" rmdir /s /q "%~3"
echo   Done
exit /b 0

:jbc_conan
:: %2 host profile, %3 build profile, %4 output folder, %5 toolchain path
:: %6 extra conan args (optional, e.g. "-s build_type=Debug" for SDK Debug builds)
if exist "%~5" (
    echo   Toolchain exists, skipping. Use --clean to force.
) else (
    conan install . -pr:h %~2 -pr:b %~3 --output-folder=%~4 --build=missing %~6
    if errorlevel 1 exit /b 1
)
if not exist "%~5" (
    echo ERROR: Conan toolchain not found: %~5
    exit /b 1
)
exit /b 0

:jbc_shaderc
:: Locate a host shaderc.exe and set SHADERC_EXE.  %2 == "require" makes a miss
:: fatal instead of a warning.
::
:: Deliberately NOT via build\host-conan's generator files.  Those record the
:: package folder of the conan install that WROTE them, and Conan garbage-
:: collects package folders.  Measured 2026-08-27: build-android.bat and
:: build-web.bat each carried their own copy of that approach; both parsed
:: bgfx-release-x86_64-data.cmake correctly and both landed on
:: .conan2\p\b\bgfxce3103953e9e9\p, a directory that no longer existed, while
:: the live cache held six shaderc.exe copies.  A miss then compiled zero
:: shaders, packed an empty pak, and failed 500 lines later with "generated
:: archive failed BOM integrity audit" -- a message that names nothing relevant.
set "SHADERC_EXE="
set "_JBC_ROOT=%~dp0..\.."

:: 1. An explicit override wins only when it executes on this host.
if defined JCE_SHADERC_EXECUTABLE if exist "%JCE_SHADERC_EXECUTABLE%" call :jbc_try_shaderc "%JCE_SHADERC_EXECUTABLE%"

:: 2. The LIVE Conan cache: Debug build tree, then Release build tree, then \p\bin.
if not defined SHADERC_EXE for /f "delims=" %%S in ('dir /s /b "%USERPROFILE%\.conan2\p\b\shaderc.exe" 2^>nul ^| findstr /i "bgfx" ^| findstr /i /c:"\b\build\Debug"') do if not defined SHADERC_EXE call :jbc_try_shaderc "%%S"
if not defined SHADERC_EXE for /f "delims=" %%S in ('dir /s /b "%USERPROFILE%\.conan2\p\b\shaderc.exe" 2^>nul ^| findstr /i "bgfx" ^| findstr /i /c:"\b\build\Release"') do if not defined SHADERC_EXE call :jbc_try_shaderc "%%S"
if not defined SHADERC_EXE for /f "delims=" %%S in ('dir /s /b "%USERPROFILE%\.conan2\p\b\shaderc.exe" 2^>nul ^| findstr /i "bgfx" ^| findstr /i /c:"\p\bin"') do if not defined SHADERC_EXE call :jbc_try_shaderc "%%S"

:: 3. An installed SDK ships one in bin\.
if not defined SHADERC_EXE for /f "delims=" %%S in ('dir /s /b "%_JBC_ROOT%\dist\sdk\*\bin\shaderc.exe" 2^>nul') do if not defined SHADERC_EXE call :jbc_try_shaderc "%%S"

:: 4. A host-tools build, if one was made.
if not defined SHADERC_EXE if exist "%_JBC_ROOT%\build\host\tools\shaderc.exe" call :jbc_try_shaderc "%_JBC_ROOT%\build\host\tools\shaderc.exe"

if defined SHADERC_EXE (
    echo   Using shaderc: %SHADERC_EXE%
    exit /b 0
)
if /i "%~2"=="require" (
    echo ERROR: host shaderc.exe not found, and this target needs it.
    echo        Without it zero shaders are compiled and the build fails much
    echo        later with an unrelated-looking pak integrity error.
    echo        Searched: JCE_SHADERC_EXECUTABLE, the Conan cache under
    echo        %USERPROFILE%\.conan2\p\b, %_JBC_ROOT%\dist\sdk\*\bin and
    echo        %_JBC_ROOT%\build\host\tools.
    echo        Fix: build the SDK once ^(python scripts\jce.py sdk^) or set
    echo        JCE_SHADERC_EXECUTABLE to an existing shaderc.exe.
    exit /b 1
)
echo   WARNING: shaderc.exe not found; shaders will not be compiled.
exit /b 0

:jbc_try_shaderc
"%~1" --version >nul 2>&1
if not errorlevel 1 set "SHADERC_EXE=%~1"
exit /b 0

:jbc_verify
:: %2 file to check, %3 optional human-readable label
if exist "%~2" exit /b 0
if "%~3"=="" (
    echo ERROR: expected file not found: %~2
) else (
    echo ERROR: %~3 not found: %~2
)
exit /b 1
