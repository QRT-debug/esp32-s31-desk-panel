@echo off
rem ============================================================
rem  ESP32-S31 面板 · 关闭 PC 服务
rem
rem  什么时候用：玩大型游戏 / 想让后台干净一点
rem  重新开启  ：start_pc_monitor.bat
rem
rem  注：本服务只通过 NVML 读取显卡数据，不建 CUDA 上下文、
rem      不分配显存；关掉主要是少一个常驻 python 进程。
rem ============================================================
echo.
echo   正在关闭 PC 监控服务...

powershell -NoProfile -ExecutionPolicy Bypass -Command "$p=@(Get-CimInstance Win32_Process -Filter \"name='python.exe'\" | Where-Object { $_.CommandLine -like '*pc_monitor_server.py*' }); if ($p.Count -eq 0) { Write-Host '   没有运行中的服务（可能已经关了）' } else { $p | ForEach-Object { Write-Host ('   killed PID ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force } }"

echo.
echo   完成。重新开启请运行 start_pc_monitor.bat
timeout /t 4 >nul
