@echo off
call "%~dp0..\tools\build\build-web.bat" %*
exit /b %errorlevel%
