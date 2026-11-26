@echo off
chcp 65001 >nul
title S31 Services
cd /d "%~dp0.."

rem Keep this file pure ASCII: CMD parses .bat as GBK, non-ASCII here breaks parsing.
set "PY=python"
where python >nul 2>nul
if errorlevel 1 set "PY=C:\Users\Admin\AppData\Local\Programs\Python\Python313\python.exe"

echo ============================================================
echo   S31 panel  -  stop VOICE MODELS only
echo ------------------------------------------------------------
echo   asr     8081  SenseVoice   (VRAM)
echo   sovits  9880  GPT-SoVITS  (VRAM)
echo.
echo   monitor 8080 keeps running (PC stats + bilibili relay)
echo ============================================================
echo.

"%PY%" tools\services.py stop asr sovits

echo.
echo   Done. To bring voice back:  python tools\services.py start asr sovits
echo.
pause
