@echo off
chcp 65001 >nul
title S31 服务管理器
cd /d "%~dp0.."

rem 优先用 PATH 里的 python；没有就用已知的 3.13 绝对路径
set "PY=python"
where python >nul 2>nul
if errorlevel 1 set "PY=C:\Users\Admin\AppData\Local\Programs\Python\Python313\python.exe"

echo ============================================================
echo   ESP32-S31 面板 · PC 侧服务
echo ------------------------------------------------------------
echo   monitor  8080  PC 主机监控 + B站热搜中转
echo   asr      8081  语音识别（SenseVoice）
echo ------------------------------------------------------------
echo   设置语音服务（需先启动它，设备才能识别）
echo ============================================================
echo.

"%PY%" tools\services.py start

echo.
echo ------------------------------------------------------------
echo   服务已在后台运行，关闭本窗口不影响它们。
echo   停止全部 : "%PY%" tools\services.py stop
echo   重启某个 : "%PY%" tools\services.py restart asr
echo   看日志   : "%PY%" tools\services.py log asr
echo ------------------------------------------------------------
echo.
pause
