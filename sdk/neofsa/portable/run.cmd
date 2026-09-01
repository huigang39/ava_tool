@echo off
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
"%~dp0runtime\python\python.exe" -B "%~dp0launcher.py" %*
set "demo_exit=%errorlevel%"
if "%~1"=="" pause
exit /b %demo_exit%
