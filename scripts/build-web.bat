@echo off
REM JCE_BUILD_CONSISTENCY_XP_V3: public entry routes through the unified driver.
python "%~dp0jce.py" web %*
exit /b %errorlevel%
