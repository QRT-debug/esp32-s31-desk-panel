@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

set PY=C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe
set SCRIPT=%~dp0mp3_to_wav_drop.py

if "%~1"=="" (
    echo.
    echo   MP3 to WAV converter  ^(ESP32-S31 board^)
    echo   ----------------------------------------
    echo   Drag one or more .mp3 files onto this .bat
    echo.
    echo   Output: same folder, same name, .wav
    echo           mono 16bit 44100 Hz  ^(plays on the board^)
    echo.
    echo   Note: only 44.1 kHz sources are accepted.
    echo         Other rates must be resampled first ^(see message^).
    echo.
    pause
    exit /b 0
)

"%PY%" "%SCRIPT%" %*
echo.
pause
