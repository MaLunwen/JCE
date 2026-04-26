@echo off
:: ================================================================
:: lib/jce_finish.bat -- Common build-script footer.
::
::   Usage from a build script (must `popd` first if it pushed):
::     call "%~dp0lib\jce_finish.bat" success
::     exit /b 0
::
::     :error
::     call "%~dp0lib\jce_finish.bat" fail
::     exit /b 1
::
:: Behaviour:
::   * Plays the BIOS-style beep (1 short / 3 short via lib\jce_beep.bat)
::   * Runs the auto-close countdown previously inlined in every script:
::       - success: 3s, ":q" to exit early
::       - fail   : 15s, any key pauses, ":q" to exit
::   * Returns control to caller; caller chooses the final exit /b code.
::
:: All terminal output uses cmd.exe + powershell only — no third-party
:: tools required, no platform-specific code beyond what the .bat itself
:: already implies.
:: ================================================================
setlocal

set "MODE=%~1"
if /i "%MODE%"=="success" goto :do_success
if /i "%MODE%"=="fail"    goto :do_fail
echo [jce_finish] usage: jce_finish.bat ^<success^|fail^>
exit /b 0

:do_success
call "%~dp0jce_beep.bat" success
powershell -NoProfile -Command "$t=3;$e=0;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if($e -ge $t){Write-Host '';break};Write-Host \"`r[SUCCESS] Auto-closing in $($t-$e)s... (:q to quit) \" -NoNewline;Start-Sleep 1;$e++}"
exit /b 0

:do_fail
call "%~dp0jce_beep.bat" fail
powershell -NoProfile -Command "$t=15;$e=0;$p=$false;$c=$false;while($true){if([Console]::KeyAvailable){$k=[Console]::ReadKey($true);if(-not $p){$p=$true;Write-Host \"`r[FAILED] Paused - type :q to quit.                                        \" -NoNewline};if($k.KeyChar -eq ':'){$c=$true}elseif($c -and $k.Key -eq [ConsoleKey]::Q){Write-Host '';break}else{$c=$false}};if(-not $p){if($e -ge $t){Write-Host '';break};Write-Host \"`r[FAILED] Auto-closing in $($t-$e)s... (any key to pause, :q to quit) \" -NoNewline;Start-Sleep 1;$e++}else{Start-Sleep -Milliseconds 100}}"
exit /b 0
