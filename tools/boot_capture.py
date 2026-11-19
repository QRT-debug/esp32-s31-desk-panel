# -*- coding: utf-8 -*-
# 复位 ESP32-S31 并抓取启动日志，确认 PSRAM/内存初始化信息
import serial, time

s = serial.Serial('COM5', 115200, timeout=0.5)
s.reset_input_buffer()

# esptool classic reset 序列: EN 拉低再释放 (RTS 经三极管控制 EN)
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False
time.sleep(0.05)

buf = b''
t0 = time.time()
while time.time() - t0 < 10:
    buf += s.read(4096)
s.close()

text = buf.decode('utf-8', 'replace')
with open('boot_log.txt', 'w', encoding='utf-8') as f:
    f.write(text)
# 只打印内存相关行
for line in text.splitlines():
    low = line.lower()
    if any(k in low for k in ['psram', 'sram', 'heap', 'flash', 'spiram', 'dram', 'boot:',
                              'mem', 'cpu', 'pro cpu', 'app cpu', 'wifi', 'mode:']):
        print(line)
