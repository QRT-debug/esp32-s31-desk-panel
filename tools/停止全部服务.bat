@echo off
chcp 65001 >nul
title S31 Services
cd /d "%~dp0.."

rem Keep this file pure ASCII: CMD parses .bat as GBK, non-ASCII here breaks parsing.
set "PY=python"
where python >nul 2>nul
if errorlevel 1 set "PY=C:\Users\Admin\AppData\Local\Programs\Python\Python313\python.exe"

echo ============================================================
echo   S31 panel  -  stop ALL services
echo ------------------------------------------------------------
echo   monitor 8080 / asr 8081 / sovits 9880
echo ============================================================
echo.

"%PY%" tools\services.py stop

echo.
echo   Done. To start again:  python tools\services.py start
echo.
pause
