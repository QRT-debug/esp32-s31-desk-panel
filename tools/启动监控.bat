@echo off
title PC Monitor Server
cd /d "%~dp0"

echo ============================================================
echo   PC Monitor Server  -  for ESP32-S31 display
echo ------------------------------------------------------------
echo   1. Keep this window OPEN while using the device.
echo   2. Press Ctrl+C to stop.
echo   3. First run may pop up a Windows Firewall dialog - Allow it.
echo ============================================================
echo.

python pc_monitor_server.py

echo.
echo ------------------------------------------------------------
echo   Server stopped. Press any key to close.
echo ------------------------------------------------------------
pause >nul
