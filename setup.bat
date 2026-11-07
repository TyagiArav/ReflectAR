@echo off
rem One-time ReflectAR setup for Windows (see SETUP.md). Double-click it, or run it from a terminal.
rem Pass -NoBuild to skip compiling the Unreal module.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_windows.ps1" %*
echo.
pause
