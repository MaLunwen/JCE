@echo off
REM ============================================================== REM
REM  scripts\package-editor.bat                                       REM
REM                                                                   REM
REM  Assemble a redistributable JCE Editor bundle for a Windows       REM
REM  target arch.  The editor is a SELF-CONTAINED single executable:   REM
REM  its UI (i18n / fonts) is baked into an embedded PAK, and it now    REM
REM  builds / cooks / packages user projects NATIVELY (driving         REM
REM  cmake/ninja + MSVC directly via jce_process) — it no longer shells REM
REM  out to first-party scripts\*.bat helpers.  The bundle is therefore REM
REM  just the exe plus the SDK it links projects against:               REM
REM                                                                   REM
REM      dist\editor\win32-<arch>[-dist]\                             REM
REM      |-- jce_editor.exe          (+ jce_editor_sha256.txt, dist)  REM
REM      |-- sdk\                    (headers, libs, jce_pak, cmake)   REM
REM      |-- VERSION.txt                                              REM
REM      \-- README.txt                                              REM
REM                                                                   REM
REM  Layout rationale:                                                REM
REM    * jce_editor.exe sits at the bundle root.  The editor resolves  REM
REM      the SDK from project.sdk_path / JCE_SDK_DIR; for the bundled  REM
REM      case point a project at <root>\sdk.                           REM
REM                                                                   REM
REM  Still required on the user machine (NOT bundled): MSVC build     REM
REM  tools, CMake >= 3.20, Ninja.  See README.txt emitted below.      REM
REM                                                                   REM
REM  Usage:  scripts\package-editor.bat                               REM
REM             [--arch x86_64|i686|aarch64]   (aliases: x64|x86|arm64)REM
REM             [--variant release|dist]       (default: release)     REM
REM             [--skip-build]   reuse existing editor exe / SDK tree  REM
REM             [--out <dir>]    override output dir                   REM
REM                                                                   REM
REM  Exit codes: 0 ok | 2 bad usage | 3 build failed | 4 stage failed REM
REM ============================================================== REM
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.." >nul
set "ROOT=%CD%"
popd >nul

set "ARCH=x86_64"
set "VARIANT=release"
set "SKIP_BUILD=0"
set "OUT_DIR="

:parse_args
if "%~1"=="" goto :done_args
if /I "%~1"=="--skip-build" set "SKIP_BUILD=1" & shift & goto :parse_args
if /I "%~1"=="--arch" (
	if "%~2"=="" ( echo [package-editor] --arch requires a value & exit /b 2 )
	set "ARCH=%~2"
	shift & shift & goto :parse_args
)
if /I "%~1"=="--variant" (
	if "%~2"=="" ( echo [package-editor] --variant requires a value & exit /b 2 )
	set "VARIANT=%~2"
	shift & shift & goto :parse_args
)
if /I "%~1"=="--out" (
	if "%~2"=="" ( echo [package-editor] --out requires a value & exit /b 2 )
	set "OUT_DIR=%~2"
	shift & shift & goto :parse_args
)
echo [package-editor] unknown argument: %~1
exit /b 2
:done_args

REM ------ Normalise --arch (mirror package-sdk.bat) ------
set "ARCH_LC=%ARCH%"
if /I "%ARCH_LC%"=="x64"     set "ARCH=x86_64"
if /I "%ARCH_LC%"=="amd64"   set "ARCH=x86_64"
if /I "%ARCH_LC%"=="x86_64"  set "ARCH=x86_64"
if /I "%ARCH_LC%"=="x86"     set "ARCH=i686"
if /I "%ARCH_LC%"=="i686"    set "ARCH=i686"
if /I "%ARCH_LC%"=="arm64"   set "ARCH=aarch64"
if /I "%ARCH_LC%"=="aarch64" set "ARCH=aarch64"

set "ENG_ARCH="
if /I "%ARCH%"=="x86_64"  set "ENG_ARCH=x64"
if /I "%ARCH%"=="i686"    set "ENG_ARCH=x86"
if /I "%ARCH%"=="aarch64" set "ENG_ARCH=arm64"
if "%ENG_ARCH%"=="" (
	echo [package-editor] unsupported --arch '%ARCH%'
	echo                   expected: x86_64^|i686^|aarch64  ^(aliases x64^|x86^|arm64^)
	exit /b 2
)

REM ------ Normalise --variant ------
set "V_SUFFIX="
if /I "%VARIANT%"=="release" set "V_SUFFIX="
if /I "%VARIANT%"=="dist"    set "V_SUFFIX=-dist"
if /I not "%VARIANT%"=="release" if /I not "%VARIANT%"=="dist" (
	echo [package-editor] unsupported --variant '%VARIANT%'
	echo                   expected: release^|dist
	exit /b 2
)

set "EDITOR_BUILD_DIR=%ROOT%\build\desktop\windows-%ENG_ARCH%"
set "EDITOR_EXE=%EDITOR_BUILD_DIR%\%VARIANT%\jce_editor.exe"
set "SDK_DIR=%ROOT%\dist\sdk\win32-%ARCH%%V_SUFFIX%"
if "%OUT_DIR%"=="" set "OUT_DIR=%ROOT%\dist\editor\win32-%ARCH%%V_SUFFIX%"

echo [package-editor] root:    %ROOT%
echo [package-editor] arch:    %ARCH%  ^(engine-tree: %ENG_ARCH%^)
echo [package-editor] variant: %VARIANT%
echo [package-editor] out:     %OUT_DIR%
echo.

REM ============================================================== REM
REM  Step 1: editor exe                                              REM
REM ============================================================== REM
if "%SKIP_BUILD%"=="1" (
	if not exist "%EDITOR_EXE%" (
		echo [package-editor] --skip-build set but editor exe missing: %EDITOR_EXE%
		exit /b 3
	)
	echo [package-editor] reusing editor exe: %EDITOR_EXE%
) else (
	if /I not "%ENG_ARCH%"=="x64" (
		echo [package-editor] build-editor.bat only targets x64; for %ENG_ARCH%
		echo                   build the editor first, then re-run with --skip-build.
		if not exist "%EDITOR_EXE%" exit /b 3
		echo [package-editor] found existing exe, continuing: %EDITOR_EXE%
	) else (
		echo === Step 1: build editor ^(%VARIANT%^) ===
		if /I "%VARIANT%"=="dist" (
			call "%SCRIPT_DIR%build-editor.bat" --dist || goto :build_fail
		) else (
			call "%SCRIPT_DIR%build-editor.bat" || goto :build_fail
		)
	)
)
if not exist "%EDITOR_EXE%" (
	echo [package-editor] editor exe not found after build: %EDITOR_EXE%
	exit /b 3
)

REM ============================================================== REM
REM  Step 2: SDK tree                                                REM
REM ============================================================== REM
set "SDK_CONFIG=%SDK_DIR%\lib\cmake\JCE\JCEConfig.cmake"
if "%SKIP_BUILD%"=="1" (
	if not exist "%SDK_CONFIG%" (
		echo [package-editor] --skip-build set but SDK missing: %SDK_CONFIG%
		echo                   run: scripts\package-sdk.bat --arch %ARCH% --variant %VARIANT%
		exit /b 3
	)
	echo [package-editor] reusing SDK: %SDK_DIR%
) else (
	echo === Step 2: ensure SDK ^(%VARIANT%^) ===
	call "%SCRIPT_DIR%package-sdk.bat" --arch %ARCH% --variant %VARIANT% --skip-debug || goto :build_fail
)
if not exist "%SDK_CONFIG%" (
	echo [package-editor] SDK config not found after packaging: %SDK_CONFIG%
	exit /b 3
)

REM ============================================================== REM
REM  Step 3: stage bundle                                            REM
REM ============================================================== REM
echo === Step 3: stage bundle ===
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%" 2>nul || goto :stage_fail

REM ---- editor exe (+ sidecars) ----
copy /Y "%EDITOR_EXE%" "%OUT_DIR%\" >nul || goto :stage_fail
if exist "%EDITOR_BUILD_DIR%\%VARIANT%\jce_editor_sha256.txt" (
	copy /Y "%EDITOR_BUILD_DIR%\%VARIANT%\jce_editor_sha256.txt" "%OUT_DIR%\" >nul
)
for %%F in ("%EDITOR_BUILD_DIR%\%VARIANT%\*.dll") do copy /Y "%%~F" "%OUT_DIR%\" >nul

REM ---- sdk\ ----
mkdir "%OUT_DIR%\sdk" 2>nul
xcopy /E /I /Y /Q "%SDK_DIR%" "%OUT_DIR%\sdk" >nul || goto :stage_fail

REM ---- VERSION.txt ----
set "GIT_SHA=unknown"
for /f "usebackq tokens=* delims=" %%S in (`git -C "%ROOT%" rev-parse --short HEAD 2^>nul`) do set "GIT_SHA=%%S"
set "SDK_SHA=unknown"
if exist "%SDK_DIR%\VERSION.txt" (
	for /f "usebackq tokens=2 delims=: " %%V in (`findstr /i /c:"commit:" "%SDK_DIR%\VERSION.txt"`) do set "SDK_SHA=%%V"
)
> "%OUT_DIR%\VERSION.txt" (
	echo product:    JCE Editor bundle
	echo platform:   win32
	echo arch:       %ARCH%
	echo variant:    %VARIANT%
	echo commit:     %GIT_SHA%
	echo sdk-commit: %SDK_SHA%
	echo built:      %DATE% %TIME%
)

REM ---- README.txt ----
> "%OUT_DIR%\README.txt" (
	echo JCE Editor bundle ^(win32-%ARCH%, %VARIANT%^)
	echo =================================================
	echo.
	echo Contents:
	echo   jce_editor.exe   The editor. Double-click to launch.
	echo   sdk\             Engine SDK ^(headers, libs, jce_pak, CMake config^).
	echo.
	echo Prerequisites on this machine ^(NOT bundled^):
	echo   * Microsoft Visual C++ Build Tools ^(MSVC^) - the editor compiles
	echo     your game project locally and links it against the SDK libs.
	echo   * CMake 3.20 or newer, on PATH.
	echo   * Ninja, on PATH.
	echo.
	echo The editor builds/cooks/packages projects NATIVELY - it drives
	echo cmake/ninja + MSVC itself, with no helper scripts. Point your
	echo project at this bundled sdk\ ^(set sdk_path in jce_project.json or
	echo the JCE_SDK_DIR environment variable^). Keep jce_editor.exe and
	echo sdk\ together.
	echo.
	echo Do not move jce_editor.exe out of this folder on its own.
)

echo.
echo [package-editor] [SUCCESS]
echo [package-editor]   bundle: %OUT_DIR%
echo [package-editor]   exe:    %OUT_DIR%\jce_editor.exe
endlocal
exit /b 0

:build_fail
echo.
echo [package-editor] [FAILED] build step failed.
exit /b 3

:stage_fail
echo.
echo [package-editor] [FAILED] staging failed.
exit /b 4
