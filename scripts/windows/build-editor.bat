@echo off
REM Thin shim -> jce.py (cross-platform build driver). See scripts/jce.py.
REM On exit: beep + ":q to quit" auto-close footer via lib\jce_finish.bat.
python "%~dp0..\jce.py" editor %*
set "_RC=%errorlevel%"
if "%_RC%"=="0" (call "%~dp0..\lib\jce_finish.bat" success) else (call "%~dp0..\lib\jce_finish.bat" fail)
exit /b %_RC%
