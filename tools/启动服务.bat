@echo off
chcp 65001 >nul
title S31 Services
cd /d "%~dp0.."

rem Keep this file pure ASCII: CMD parses .bat as GBK, Chinese here breaks command parsing
set "PY=python"
where python >nul 2>nul
if errorlevel 1 set "PY=C:\Users\Admin\AppData\Local\Programs\Python\Python313\python.exe"

echo ============================================================
echo   ESP32-S31 panel  -  PC side services
echo ------------------------------------------------------------
echo   monitor  8080   PC stats + Bilibili hot search relay
echo   asr      8081   Voice recognition (SenseVoice)
echo ============================================================
echo.

"%PY%" tools\services.py start

echo.
echo ------------------------------------------------------------
echo   Services keep running after this window closes.
echo.
echo   Stop all :  "%PY%" tools\services.py stop
echo   Restart  :  "%PY%" tools\services.py restart asr
echo   Log      :  "%PY%" tools\services.py log asr
echo ------------------------------------------------------------
echo.
pause
