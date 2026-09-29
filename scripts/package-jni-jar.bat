@echo off
call "%~dp0..\tools\build\package-jni-jar.bat" %*
exit /b %errorlevel%
