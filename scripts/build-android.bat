@echo off
call "%~dp0..\tools\build\build-android.bat" %*
exit /b %errorlevel%
