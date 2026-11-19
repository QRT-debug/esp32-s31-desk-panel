#!/bin/sh
# 在 Git Bash 里激活 ESP-IDF 6.1（EIM 安装在 C:\Espressif\tools）
# 用法：  . ./idf_env.sh   然后  idf build
# 注意：IDF 的 export.sh 拒绝在 MSys/MinGW 下运行，这里绕过它手动拼环境。

unset MSYSTEM MINGW_PREFIX MINGW_CHOST 2>/dev/null

export IDF_PATH="D:/esp/.espressif/v6.1/esp-idf"
export IDF_TOOLS_PATH="C:/Espressif/tools"
export IDF_PYTHON_ENV_PATH="C:/Espressif/tools/python/v6.1/venv"
export ESP_ROM_ELF_DIR="C:/Espressif/tools/esp-rom-elfs/20241011/"
export OPENOCD_SCRIPTS="C:/Espressif/tools/openocd-esp32/v0.12.0-esp32-20260703/openocd-esp32/share/openocd/scripts"
export IDF_CCACHE_ENABLE=1

export PATH="/c/Espressif/tools/python/v6.1/venv/Scripts:/c/Espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin:/c/Espressif/tools/cmake/4.0.3/bin:/c/Espressif/tools/ninja/1.12.1:/c/Espressif/tools/ccache/4.12.1:/c/Espressif/tools/idf-exe/1.0.3:$PATH"

idf() { python "$IDF_PATH/tools/idf.py" "$@"; }
