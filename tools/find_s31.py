"""找到局域网里的 ESP32-S31 设备并打印 IP。

用法：双击本文件，或 python find_s31.py
原理：并发扫描 192.168.1.2~254 的 80 端口，认页面标题 "SmartPanel"。
（不依赖 mDNS —— Windows 系统不解析 .local，浏览器普通导航也不查 mDNS。）
"""
import concurrent.futures
import socket
import sys
import urllib.request

SUBNET = "192.168.1."
PORTS = (80,)


def probe(ip: str):
    for port in PORTS:
        try:
            with socket.create_connection((ip, port), timeout=0.35):
                pass
        except OSError:
            continue
        try:
            with urllib.request.urlopen(f"http://{ip}/", timeout=1.5) as r:
                body = r.read(400)
                if b"SmartPanel" in body:
                    return ip
        except Exception:
            continue
    return None


def main():
    print(f"扫描 {SUBNET}2-254 ...")
    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as ex:
        for res in ex.map(probe, [f"{SUBNET}{i}" for i in range(2, 255)]):
            if res:
                found.append(res)
    if not found:
        print("没找到设备。检查：1) 设备是否上电联网 2) 电脑是否和它同一网段")
        return 1
    for ip in found:
        print(f"\n  设备地址: http://{ip}/\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
