@echo off
rem ============================================================
rem  ESP32-S31 面板 · PC 服务（一个脚本，两个端点）
rem    /api/stats.txt  -> PC 监视（CPU/内存/磁盘）  面板 APPS 页
rem    /api/bili.txt   -> B 站热搜中转              面板 BILI HOT 页
rem  面板要求 PC 开机并运行本服务；服务停了面板会显示上次数据。
rem  开机自启：Win+R -> shell:startup -> 把本文件快捷方式放进去
rem  注：用 python.exe 而非 pythonw（pythonw 无控制台会静默退出）
rem ============================================================
cd /d "%~dp0"
start "S31 PC Monitor" /min "C:/Users/Admin/AppData/Local/Programs/Python/Python313/python.exe" "pc_monitor_server.py"
exit
