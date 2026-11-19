$env:IDF_PATH = "D:\esp\.espressif\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "C:\Espressif\tools\python\v6.1\venv"
$env:ESP_ROM_ELF_DIR = "C:\Espressif\tools\esp-rom-elfs\20241011\"
$env:IDF_CCACHE_ENABLE = "1"
$env:ESP_IDF_VERSION = "6.1.0"
$env:PATH = "C:\Espressif\tools\python\v6.1\venv\Scripts;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1;C:\Espressif\tools\idf-exe\1.0.3;" + $env:PATH

function idf { python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" @args }

# 烧录波特率 1500000。
# ★ 实测（2026-11-16，1.45 MB 固件，纯烧录阶段）：
#    NO_STUB=y + 921600 ...... 30 s 以上（关掉 stub 是最大瓶颈）
#    stub + 921600 ........... 16.1 s
#    stub + 1500000 .......... 12.6 s   ← 当前
#    stub + 2000000 .......... 11.3 s   （收益只剩 10%，偶发误码风险更高，不采用）
# 注意：烧录过程中不要按板上 RESET，
# 那会切断 USB 串口，esptool 报 "No more data to read from the serial port"。
$env:ESPBAUD = "1500000"
function flash { python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" -p COM5 -b 1500000 flash @args }
# 只烧 app（跳过 bootloader/分区表，省约 1 秒）；改了分区表或 sdkconfig 时不要用这个
function appflash { python "D:\esp\.espressif\v6.1\esp-idf\tools\idf.py" -p COM5 -b 1500000 app-flash @args }
