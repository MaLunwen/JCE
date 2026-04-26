@echo off
:: ================================================================
:: _beep.bat -- BIOS-style audio cue for build scripts.
::   Usage:  call "%~dp0_beep.bat" success
::           call "%~dp0_beep.bat" fail
:: success -> 1 short high tone   (BIOS one-beep "POST OK")
:: fail    -> 3 quick low tones   (BIOS error code "system error")
:: Silently no-ops if powershell is not on PATH so scripts never
:: fail just because audio isn't available.
:: ================================================================
where powershell >nul 2>&1
if errorlevel 1 exit /b 0

if /i "%~1"=="success" (
    powershell -NoProfile -Command "[console]::beep(880,150)" >nul 2>&1
    exit /b 0
)
if /i "%~1"=="fail" (
    powershell -NoProfile -Command "[console]::beep(440,120);Start-Sleep -m 70;[console]::beep(440,120);Start-Sleep -m 70;[console]::beep(440,120)" >nul 2>&1
    exit /b 0
)
exit /b 0
