@echo off
:: ===================================================================
:: cook-project.bat  Mirror source_assets -> cooked_assets for a JCE
::                   project before build (pure cmd.exe, no PowerShell).
::
:: Usage:
::   cook-project.bat <project_dir> [source_rel] [cooked_rel]
::
:: Both relative dirs are optional and default to the schema convention
:: (assets -> resources/_cooked).  Callers that have already parsed the
:: manifest (the editor, build-project.bat) pass them explicitly so this
:: script never has to read jce_project.json.  The editor additionally
:: cooks natively via jce_cook_run_all(); this script is the standalone /
:: fallback path and a cheap mtime-gated no-op when already cooked.
:: ===================================================================
setlocal EnableDelayedExpansion

set "PROJECT_DIR=%~1"
set "SRC_REL=%~2"
set "DST_REL=%~3"

if "%PROJECT_DIR%"=="" (
    echo [cook] usage: cook-project.bat ^<project_dir^> [source_rel] [cooked_rel]
    exit /b 1
)
if "%SRC_REL%"=="" set "SRC_REL=assets"
if "%DST_REL%"=="" set "DST_REL=resources/_cooked"

:: Normalise forward slashes for xcopy / path joins.
set "SRC_REL=%SRC_REL:/=\%"
set "DST_REL=%DST_REL:/=\%"
set "SRC_ABS=%PROJECT_DIR%\%SRC_REL%"
set "DST_ABS=%PROJECT_DIR%\%DST_REL%"

if not exist "%SRC_ABS%" (
    echo [cook] source_assets dir not present ^(%SRC_ABS%^) — skipping
    exit /b 0
)

echo [cook] %SRC_REL% -^> %DST_REL%
if not exist "%DST_ABS%" mkdir "%DST_ABS%"

:: /E recurse incl empty, /I assume dir, /Y overwrite, /D mtime-newer only,
:: /Q quiet.  Mirrors the previous cook-project.ps1 behaviour exactly.
xcopy "%SRC_ABS%" "%DST_ABS%" /E /I /Y /D /Q >nul
if errorlevel 1 (
    echo [cook] xcopy failed with exit %errorlevel%
    exit /b 2
)
exit /b 0
