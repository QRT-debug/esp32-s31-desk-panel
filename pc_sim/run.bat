@echo off
rem ---------------------------------------------------------------
rem  PC simulator build & run
rem  ASCII only: cmd parses .bat as GBK, non-ASCII comments break it
rem ---------------------------------------------------------------
setlocal

set "PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%"
cd /d "%~dp0"

echo === configure ===
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto fail

echo === build ===
cmake --build build
if errorlevel 1 goto fail

echo === run ===
build\pc_sim.exe
goto end

:fail
echo.
echo BUILD FAILED
pause
exit /b 1

:end
