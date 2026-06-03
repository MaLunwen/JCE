@echo off
REM ============================================================== REM
REM  scripts\package-sdk.bat                                          REM
REM                                                                   REM
REM  Build the JCE SDK for a Windows target arch.                     REM
REM  Produces a self-contained install tree at                        REM
REM      dist\sdk\win32-<arch>\           (release variant)           REM
REM      dist\sdk\win32-<arch>-dist\      (dist  variant; royalty-free)REM
REM  that end-user projects consume via `find_package(JCE REQUIRED)`  REM
REM  -- no Conan required at consume time.                            REM
REM                                                                   REM
REM  release variant:                                                 REM
REM    Patented codecs (AAC/H.264/H.265 via fdk-aac/OpenH264/libhevc) REM
REM    are baked into jce_engine_core.lib.  Use for in-house / non-   REM
REM    redistributable builds.                                        REM
REM                                                                   REM
REM  dist variant:                                                    REM
REM    JCE_ENABLE_PATENTED_CODECS forced OFF.  No patent-encumbered   REM
REM    decoders linked in.  Safe for royalty-free redistribution.     REM
REM    Built in a *separate* build tree to avoid CMakeCache conflicts.REM
REM                                                                   REM
REM  Usage:  scripts\package-sdk.bat                                  REM
REM             [--arch x86_64|i686|aarch64]   (aliases: x64|x86|arm64)REM
REM             [--variant release|dist|both]  (default: both)        REM
REM             [--release-only]               (legacy alias of       REM
REM                                              --variant release    REM
REM                                              + skip Debug config) REM
REM             [--skip-debug]                                        REM
REM ============================================================== REM
setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.." >nul
set "ROOT=%CD%"
popd >nul

set "ARCH=x86_64"
set "VARIANT=both"
set "SKIP_DEBUG=0"

:parse_args
if "%~1"=="" goto :done_args
if /I "%~1"=="--release-only" set "VARIANT=release" & set "SKIP_DEBUG=1" & shift & goto :parse_args
if /I "%~1"=="--skip-debug"   set "SKIP_DEBUG=1" & shift & goto :parse_args
if /I "%~1"=="--arch" (
	if "%~2"=="" ( echo [package-sdk] --arch requires a value & exit /b 2 )
	set "ARCH=%~2"
	shift & shift & goto :parse_args
)
if /I "%~1"=="--variant" (
	if "%~2"=="" ( echo [package-sdk] --variant requires a value & exit /b 2 )
	set "VARIANT=%~2"
	shift & shift & goto :parse_args
)
echo [package-sdk] unknown argument: %~1
exit /b 2
:done_args

REM ------ Normalise --arch ------
set "ARCH_LC=%ARCH%"
if /I "%ARCH_LC%"=="x64"     set "ARCH=x86_64"
if /I "%ARCH_LC%"=="amd64"   set "ARCH=x86_64"
if /I "%ARCH_LC%"=="x86_64"  set "ARCH=x86_64"
if /I "%ARCH_LC%"=="x86"     set "ARCH=i686"
if /I "%ARCH_LC%"=="i686"    set "ARCH=i686"
if /I "%ARCH_LC%"=="arm64"   set "ARCH=aarch64"
if /I "%ARCH_LC%"=="aarch64" set "ARCH=aarch64"

set "ENG_ARCH="
set "CONAN_PROFILE="
if /I "%ARCH%"=="x86_64"  ( set "ENG_ARCH=x64"   & set "CONAN_PROFILE=windows-x64"   )
if /I "%ARCH%"=="i686"    ( set "ENG_ARCH=x86"   & set "CONAN_PROFILE=windows-x86"   )
if /I "%ARCH%"=="aarch64" ( set "ENG_ARCH=arm64" & set "CONAN_PROFILE=windows-arm64" )
if "%ENG_ARCH%"=="" (
	echo [package-sdk] unsupported --arch '%ARCH%'
	echo                 expected: x86_64^|i686^|aarch64
	echo                 aliases:  x64^|x86^|arm64
	exit /b 2
)

REM ------ Build-machine profile (always x64 on SDK packaging host) ------
REM For cross-compile targets (i686, aarch64) this diverges from CONAN_PROFILE.
set "BUILD_PROFILE=windows-x64"

set "CONAN_DIR=%ROOT%\build\desktop\windows-%ENG_ARCH%-conan"
set "CONAN_TOOLCHAIN_REL=%CONAN_DIR%\build\Release\generators\conan_toolchain.cmake"
set "CONAN_TOOLCHAIN_DBG=%CONAN_DIR%\build\Debug\generators\conan_toolchain.cmake"

REM ------ Normalise --variant ------
set "DO_RELEASE=0"
set "DO_DIST=0"
if /I "%VARIANT%"=="release" set "DO_RELEASE=1"
if /I "%VARIANT%"=="dist"    set "DO_DIST=1"
if /I "%VARIANT%"=="both"    ( set "DO_RELEASE=1" & set "DO_DIST=1" )
if "%DO_RELEASE%%DO_DIST%"=="00" (
	echo [package-sdk] unsupported --variant '%VARIANT%'
	echo                 expected: release^|dist^|both
	exit /b 2
)

echo [package-sdk] root:    %ROOT%
echo [package-sdk] arch:    %ARCH%  ^(engine-tree: %ENG_ARCH%^)
echo [package-sdk] variant: %VARIANT%
echo [package-sdk] debug:   %SKIP_DEBUG%  ^(0=include, 1=skip^)
echo.

if "%DO_RELEASE%"=="1" call :pack_variant release      "" || goto :error
if "%DO_DIST%"=="1"    call :pack_variant dist -dist   || goto :error

echo.
echo [package-sdk] [SUCCESS]
if "%DO_RELEASE%"=="1" echo [package-sdk]   release SDK: %ROOT%\dist\sdk\win32-%ARCH%
if "%DO_DIST%"=="1"    echo [package-sdk]   dist    SDK: %ROOT%\dist\sdk\win32-%ARCH%-dist
endlocal
exit /b 0

REM ============================================================== REM
REM  :pack_variant <variant-name> <install-suffix>                   REM
REM    variant-name    "release" | "dist"                            REM
REM    install-suffix  ""        | "-dist"                           REM
REM ============================================================== REM
:pack_variant
set "V_NAME=%~1"
set "V_SUFFIX=%~2"

set "BUILD_REL=%ROOT%\build\desktop\windows-%ENG_ARCH%%V_SUFFIX%"
set "BUILD_DBG=%ROOT%\build\desktop\windows-%ENG_ARCH%%V_SUFFIX%-debug"
set "INSTALL_DIR=%ROOT%\dist\sdk\win32-%ARCH%%V_SUFFIX%"

set "PATENTED_FLAG="
if /I "%V_NAME%"=="dist" set "PATENTED_FLAG=-DJCE_ENABLE_PATENTED_CODECS=OFF"

echo.
echo ===============================================================
echo [package-sdk] [%V_NAME%] BUILD_REL  = %BUILD_REL%
echo [package-sdk] [%V_NAME%] INSTALL    = %INSTALL_DIR%
echo [package-sdk] [%V_NAME%] PATENTED   = %PATENTED_FLAG%  (empty = default ON)
echo ===============================================================

REM ---- Release ----
if not exist "%BUILD_REL%\CMakeCache.txt" (
	if /I "%V_NAME%"=="release" (
		echo [package-sdk] Release build dir missing: %BUILD_REL%
		echo [package-sdk] Run the matching desktop build first
		echo                 ^(build-desktop.bat / build-windows-x86.bat /
		echo                  build-windows-arm64.bat^).
		exit /b 1
	)
	REM dist variant: ensure conan toolchain present, then fresh configure.
	echo [package-sdk] [%V_NAME%] ensuring Conan toolchain (profile %CONAN_PROFILE%, Release)
	call "%SCRIPT_DIR%lib\jce_build_common.bat" conan "conan/profiles/%CONAN_PROFILE%" "conan/profiles/%BUILD_PROFILE%" "%CONAN_DIR%" "%CONAN_TOOLCHAIN_REL%" || exit /b 1
	echo [package-sdk] [%V_NAME%] configuring fresh Release build tree
	cmake -S "%ROOT%" -B "%BUILD_REL%" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CONAN_TOOLCHAIN_REL%" -DCMAKE_BUILD_TYPE=Release -DJCE_BUILD_VARIANT=%V_NAME% %PATENTED_FLAG% -DJCE_ENABLE_SDK_INSTALL=ON -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"
	if errorlevel 1 exit /b 1
) else (
	echo [package-sdk] [%V_NAME%] reconfiguring existing Release tree
	cmake "%BUILD_REL%" %PATENTED_FLAG% -DJCE_ENABLE_SDK_INSTALL=ON -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"
	if errorlevel 1 exit /b 1
)

echo [package-sdk] [%V_NAME%] building Release SDK libraries
REM  The packaged editor now generates PAK/BOM/embed sources in-process,
REM  so the redistributable SDK no longer ships first-party host tools.
cmake --build "%BUILD_REL%" --target jce_sdk_fat_lib jce_msvc_stl_shims -j 8
if errorlevel 1 exit /b 1

echo [package-sdk] [%V_NAME%] installing Release
cmake --install "%BUILD_REL%"
if errorlevel 1 exit /b 1
call :prune_host_tools "%INSTALL_DIR%" || exit /b 1

REM ---- Debug (optional) ----
REM  Skip via a plain goto rather than wrapping ~25 lines (with nested
REM  if/else + literal parens in echoes) in one fragile parenthesised
REM  block: such blocks have historically mis-parsed and run the Debug
REM  config even when --skip-debug set SKIP_DEBUG=1.
if "%SKIP_DEBUG%"=="1" (
	echo [package-sdk] [%V_NAME%] skipping Debug SDK ^(--skip-debug^)
	goto :after_debug
)

if not exist "%BUILD_DBG%\CMakeCache.txt" (
	if /I "%V_NAME%"=="release" (
		echo [package-sdk] Debug build dir missing: %BUILD_DBG%
		echo [package-sdk] Run the matching debug build first, or pass --skip-debug.
		exit /b 1
	)
	echo [package-sdk] [%V_NAME%] ensuring Conan toolchain (profile %CONAN_PROFILE%, Debug)
	call "%SCRIPT_DIR%lib\jce_build_common.bat" conan "conan/profiles/%CONAN_PROFILE%" "conan/profiles/%BUILD_PROFILE%" "%CONAN_DIR%" "%CONAN_TOOLCHAIN_DBG%" "-s build_type=Debug" || exit /b 1
	echo [package-sdk] [%V_NAME%] configuring fresh Debug build tree
	cmake -S "%ROOT%" -B "%BUILD_DBG%" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%CONAN_TOOLCHAIN_DBG%" -DCMAKE_BUILD_TYPE=Debug -DJCE_BUILD_VARIANT=%V_NAME% %PATENTED_FLAG% -DJCE_ENABLE_SDK_INSTALL=ON -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"
	if errorlevel 1 exit /b 1
) else (
	echo [package-sdk] [%V_NAME%] reconfiguring existing Debug tree
	cmake "%BUILD_DBG%" %PATENTED_FLAG% -DJCE_ENABLE_SDK_INSTALL=ON -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"
	if errorlevel 1 exit /b 1
)

echo [package-sdk] [%V_NAME%] building Debug SDK libraries
cmake --build "%BUILD_DBG%" --target jce_sdk_fat_lib jce_msvc_stl_shims -j 8
if errorlevel 1 exit /b 1

echo [package-sdk] [%V_NAME%] installing Debug
cmake --install "%BUILD_DBG%"
if errorlevel 1 exit /b 1
call :prune_host_tools "%INSTALL_DIR%" || exit /b 1

:after_debug

REM ---- VERSION.txt ----
for /f "delims=" %%v in ('git -C "%ROOT%" rev-parse --short HEAD 2^>nul') do set "SHA=%%v"
if "%SHA%"=="" set "SHA=unknown"
> "%INSTALL_DIR%\VERSION.txt" echo JCE SDK build
>> "%INSTALL_DIR%\VERSION.txt" echo commit:  %SHA%
>> "%INSTALL_DIR%\VERSION.txt" echo host:    win32-%ARCH%
>> "%INSTALL_DIR%\VERSION.txt" echo variant: %V_NAME%

echo [package-sdk] [%V_NAME%] done -^> %INSTALL_DIR%
exit /b 0

:prune_host_tools
set "SDK_ROOT=%~1"
if "%SDK_ROOT%"=="" exit /b 1
if exist "%SDK_ROOT%\bin\jce_pak.exe" (
	del /q "%SDK_ROOT%\bin\jce_pak.exe" || exit /b 1
)
if exist "%SDK_ROOT%\bin\jce_bin2obj.exe" (
	del /q "%SDK_ROOT%\bin\jce_bin2obj.exe" || exit /b 1
)
exit /b 0

:error
echo.
echo [package-sdk] [FAILED]
endlocal
exit /b 1
