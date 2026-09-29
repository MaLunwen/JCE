@echo off
setlocal
set "JCE_GAME_PROJECT_DIR=examples/caged_kingdom"
set "JCE_GAME_TARGET=CagedKingdom"
set "JCE_GAME_ICON=resources\CK_icon.png"
set "JCE_ANDROID_APP_ICON=@drawable/ck_m256"
call "%~dp0..\..\..\scripts\package-jni-jar.bat" %*
exit /b %errorlevel%
