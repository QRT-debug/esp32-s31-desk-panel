@echo off
chcp 65001 >nul
title S31 Services - RESTART
cd /d "%~dp0.."

rem Keep this file pure ASCII: CMD parses .bat as GBK, Chinese breaks parsing.
set "PY=python"
where python >nul 2>nul
if errorlevel 1 set "PY=C:\Users\Admin\AppData\Local\Programs\Python\Python313\python.exe"

echo ============================================================
echo   RESTART all services  (reload modified code)
echo ------------------------------------------------------------
echo   Use this after editing persona / asr_server / services.py.
echo   The plain start script only STARTS services that are not
echo   running yet, so it will NOT pick up code changes.
echo ============================================================
echo.

"%PY%" tools\services.py stop
timeout /t 2 >nul
"%PY%" tools\services.py start

echo.
pause
