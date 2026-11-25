#!/usr/bin/env python3
"""零扰动串口日志监听。

★★★ 为什么必须零扰动：
    auto-download 电路靠 DTR/RTS 翻转来进下载模式。pyserial 的 open() 默认会把
    这两条线拉高 ⇒ **打开串口就等于按了一次复位**。
    （2026-11-23 踩过一次并修好；2026-11-25 我绕开这个脚本手写 PowerShell，
     又把用户正要拍的"画面错位"现场复位掉了。）

    正确做法：`dtr=False` / `rts=False` 必须放在 open() **之前**；
    open 之后再设已经晚了一步，脉冲已经发出。

用法：
    python tools/watch_log.py              # 监听 30 秒，全部输出
    python tools/watch_log.py 60 voice     # 监听 60 秒，只显示含 "voice" 的行
    python tools/watch_log.py 60 voice asr # 多个过滤词（任一命中即显示）

输出每行前面带相对时间戳（秒），方便算两个事件之间隔了多久。
"""
import sys
import time

import serial

PORT = "COM5"
BAUD = 115200


def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
    keys = sys.argv[2:] or [""]

    p = serial.Serial()
    p.port = PORT
    p.baudrate = BAUD
    p.timeout = 0.3
    # ★★★ 这两行必须在 open() 之前，否则会被复位
    p.dtr = False
    p.rts = False
    p.open()

    print(f"# 监听 {PORT} {dur:.0f}s，过滤: {keys}", flush=True)
    t0 = time.time()
    try:
        while time.time() - t0 < dur:
            raw = p.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace").rstrip()
            if line and any(k in line for k in keys):
                print(f"[{time.time() - t0:6.2f}] {line}", flush=True)
    finally:
        p.close()


if __name__ == "__main__":
    main()
