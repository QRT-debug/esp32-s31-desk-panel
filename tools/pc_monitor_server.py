#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC 状态采集服务 —— 给 ESP32-S31 显示屏提供 CPU / GPU / 内存 / 磁盘 / 网络数据

用法：
    python pc_monitor_server.py              # 默认端口 8080
    python pc_monitor_server.py 9000         # 指定端口

接口：
    http://<本机IP>:8080/api/stats           ESP32 拉这个（JSON）
    http://<本机IP>:8080/                    浏览器打开，人肉看效果

设计：
    - 后台线程每 1 秒采集一次，结果放缓存；HTTP 请求只读缓存，不会卡
    - GPU 用 pynvml（NVIDIA）。没有独显/驱动时自动降级，gpu 字段返回 null
    - CPU 温度在 Windows 上 psutil 读不到（需要管理员 + 主板支持），这里不采
"""
import json
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import psutil

# ★★★ 清掉代理环境变量 —— 必须在任何网络请求之前执行（2026-11-23 踩过）
#   urllib 会读 HTTP_PROXY / HTTPS_PROXY。如果启动本服务的会话里带着一个**已失效**的代理
#   （IDE、终端、代理软件退出后的残留都很常见），所有出网请求都会被送到那个死端口，
#   报 WinError 10061「目标计算机积极拒绝，无法连接」。
#   设备侧的表现就是：BILI HOT 反复断开、榜单/天气拿不到数据，而 PC 浏览器一切正常
#   —— 因为浏览器用的是系统代理设置，不读这些环境变量。
#   ★ 实测：只加 ProxyHandler({}) 不够，必须连 os.environ 一起清干净。
#   本服务只访问公网 B 站 + 本机数据，不需要代理。
import os as _os

for _k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy",
           "ALL_PROXY", "all_proxy"):
    _os.environ.pop(_k, None)

# ------------------------------------------------------------------ GPU
try:
    import pynvml

    pynvml.nvmlInit()
    _GPU_OK = True
except Exception as exc:              # 没有 NVIDIA 卡或没装驱动
    _GPU_OK = False
    pynvml = None
    print(f"[warn] NVIDIA GPU 不可用，gpu 字段将为 null（{exc}）")

# ------------------------------------------------------------------ 状态
_state_lock = threading.Lock()
_state = {}
_net_prev = None


def _read_gpu():
    if not _GPU_OK:
        return None
    try:
        h = pynvml.nvmlDeviceGetHandleByIndex(0)
        util = pynvml.nvmlDeviceGetUtilizationRates(h)
        mem = pynvml.nvmlDeviceGetMemoryInfo(h)
        temp = pynvml.nvmlDeviceGetTemperature(h, pynvml.NVML_TEMPERATURE_GPU)
        try:
            power = pynvml.nvmlDeviceGetPowerUsage(h) / 1000.0
        except Exception:
            power = None

        name = pynvml.nvmlDeviceGetName(h)
        if isinstance(name, bytes):
            name = name.decode("utf-8", "replace")

        return {
            "name": name,
            "usage": int(util.gpu),
            "mem_usage": int(util.memory),
            "mem_used": round(mem.used / 2**30, 2),
            "mem_total": round(mem.total / 2**30, 2),
            "temp": int(temp),
            "power": round(power, 1) if power is not None else None,
        }
    except Exception:
        return None


def _read_net():
    """返回 (上行 KB/s, 下行 KB/s)"""
    global _net_prev
    now = time.time()
    io = psutil.net_io_counters()
    up = down = 0.0
    if _net_prev is not None:
        dt = max(now - _net_prev[0], 1e-3)
        up = (io.bytes_sent - _net_prev[1]) / dt / 1024.0
        down = (io.bytes_recv - _net_prev[2]) / dt / 1024.0
    _net_prev = (now, io.bytes_sent, io.bytes_recv)
    return round(max(up, 0), 1), round(max(down, 0), 1)


def collect():
    cpu_pct = psutil.cpu_percent(interval=None)
    try:
        freq = psutil.cpu_freq()
        cpu_mhz = int(freq.current) if freq else 0
    except Exception:
        cpu_mhz = 0

    vm = psutil.virtual_memory()

    disks = []
    for part in psutil.disk_partitions(all=False):
        # 跳过光驱/不可读的分区
        if "cdrom" in part.opts or part.fstype == "":
            continue
        try:
            u = psutil.disk_usage(part.mountpoint)
        except Exception:
            continue
        disks.append({
            "name": part.mountpoint.rstrip("\\") or part.mountpoint,
            "percent": round(u.percent, 1),
            "free_gb": round(u.free / 2**30, 1),
            "total_gb": round(u.total / 2**30, 1),
        })
    disks.sort(key=lambda d: d["name"])

    up, down = _read_net()

    return {
        "host": socket.gethostname(),
        "uptime": int(time.time() - psutil.boot_time()),
        "cpu": {
            "usage": round(cpu_pct, 1),
            "mhz": cpu_mhz,
            "cores": psutil.cpu_count(logical=True) or 0,
        },
        "mem": {
            "percent": round(vm.percent, 1),
            "used": round(vm.used / 2**30, 2),
            "total": round(vm.total / 2**30, 2),
        },
        "gpu": _read_gpu(),
        "disks": disks,
        "net": {"up_kbps": up, "down_kbps": down},
    }


def collector_loop():
    psutil.cpu_percent(interval=None)      # 第一次调用用于建立基线
    while True:
        try:
            data = collect()
            with _state_lock:
                _state.clear()
                _state.update(data)
        except Exception as exc:
            print(f"[warn] 采集失败: {exc}")
        time.sleep(1.0)


# ------------------------------------------------------------------ HTTP
def local_ips():
    ips = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except Exception:
        pass
    # UDP connect 技巧：拿到默认出口网卡的地址（不真的发包）
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ips.add(s.getsockname()[0])
        s.close()
    except Exception:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127."))


def to_kv(d):
    """把采集结果转成 key=value 行文本，给 MCU 用（IDF 6.x 已不带 cJSON）"""
    lines = [
        f"host={d['host']}",
        f"uptime={d['uptime']}",
        f"cpu={d['cpu']['usage']}",
        f"mhz={d['cpu']['mhz']}",
        f"cores={d['cpu']['cores']}",
        f"mem={d['mem']['percent']}",
        f"mem_used={d['mem']['used']}",
        f"mem_total={d['mem']['total']}",
        f"net_up={d['net']['up_kbps']}",
        f"net_down={d['net']['down_kbps']}",
    ]
    g = d.get("gpu")
    if g:
        lines += [
            "gpu_ok=1",
            f"gpu_name={g['name']}",
            f"gpu={g['usage']}",
            f"gpu_mem_pct={g['mem_usage']}",
            f"gpu_mem_used={g['mem_used']}",
            f"gpu_mem_total={g['mem_total']}",
            f"gpu_temp={g['temp']}",
            f"gpu_power={g['power'] if g['power'] is not None else 0}",
        ]
    else:
        lines.append("gpu_ok=0")

    disks = d.get("disks", [])[:4]
    lines.append(f"disk_count={len(disks)}")
    for i, dk in enumerate(disks):
        lines += [
            f"disk{i}_name={dk['name']}",
            f"disk{i}_pct={dk['percent']}",
            f"disk{i}_free={dk['free_gb']}",
            f"disk{i}_total={dk['total_gb']}",
        ]
    return "\n".join(lines) + "\n"


# ---------------- B 站热搜（中转：设备连不上 B 站 CDN，由 PC 代抓） ----------------
_bili_cache = {"t": 0.0, "body": ""}


def fetch_bili() -> str:
    """抓 B 站热搜，返回纯文本：每行 `热搜词|热度`。缓存 5 分钟。"""
    import time
    import json
    import urllib.request

    now = time.time()
    if _bili_cache["body"] and (now - _bili_cache["t"]) < 300:
        return _bili_cache["body"]

    # ★ 用 web-interface/search/square：data.trending.list[] 带 keyword/show_name/heat_score
    #   （app.bilibili.com 的 v2 接口 list 里没有热度字段，只有 position）
    url = "https://api.bilibili.com/x/web-interface/search/square?limit=10"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    # ★★ 显式禁用代理：urllib 默认会读 HTTP_PROXY/HTTPS_PROXY 环境变量和系统代理设置。
    #    本服务只访问公网 B 站和本机数据，不需要代理；而启动会话里若带着失效的代理变量
    #    （IDE、终端、代理软件残留都很常见），请求就会被送到一个没人监听的本地端口，
    #    报 WinError 10061「目标计算机积极拒绝」，设备侧表现就是 BILI HOT 老是断开。
    #    ProxyHandler({}) = 不走任何代理，一劳永逸。
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req, timeout=10) as r:
        data = json.load(r)

    lst = (((data.get("data") or {}).get("trending") or {}).get("list")) or []
    lines = []
    for it in lst[:10]:
        w = (it.get("show_name") or it.get("keyword") or "").replace("\n", " ").replace("|", "/")
        h = int(it.get("heat_score") or 0)
        if w:
            lines.append("%s|%d" % (w, h))
    body = "\n".join(lines) + "\n"
    _bili_cache["t"] = now
    _bili_cache["body"] = body
    return body


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # 只打关键请求，避免 ESP32 每秒一次刷屏
        path = args[0] if args else ""
        if "/api/stats" not in str(path):
            sys.stderr.write("[http] %s\n" % (fmt % args))

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        raw = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.startswith("/api/bili.txt"):
            try:
                body = fetch_bili()
                self._send(200, body, "text/plain; charset=utf-8")
            except Exception as e:                      # noqa: BLE001
                self._send(502, "error: %s\n" % e, "text/plain; charset=utf-8")
        elif self.path.startswith("/api/stats.txt"):
            with _state_lock:
                body = to_kv(_state)
            self._send(200, body, "text/plain; charset=utf-8")
        elif self.path.startswith("/api/stats"):
            with _state_lock:
                body = json.dumps(_state, ensure_ascii=False)
            self._send(200, body)
        elif self.path in ("/", "/index.html"):
            with _state_lock:
                body = json.dumps(_state, ensure_ascii=False, indent=2)
            html = (
                "<!doctype html><meta charset='utf-8'>"
                "<title>PC Monitor</title>"
                "<style>body{background:#111;color:#ddd;font:14px/1.6 Consolas,monospace;"
                "padding:24px}h1{font-size:16px;color:#4cf}</style>"
                "<h1>PC Monitor — /api/stats</h1><pre>" + body + "</pre>"
            )
            self._send(200, html, "text/html; charset=utf-8")
        else:
            self._send(404, '{"error":"not found"}')


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    threading.Thread(target=collector_loop, daemon=True).start()
    time.sleep(1.2)                        # 等第一次采集完成

    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print("=" * 56)
    print("  PC Monitor 已启动")
    print("=" * 56)
    for ip in local_ips():
        print(f"  ESP32 填写地址:  http://{ip}:{port}/api/stats.txt")
    print(f"  浏览器 JSON:     http://127.0.0.1:{port}/")
    print(f"  浏览器 KV 文本:  http://127.0.0.1:{port}/api/stats.txt")
    print(f"  GPU: {'NVIDIA (pynvml)' if _GPU_OK else '未检测到，gpu=null'}")
    print("-" * 56)
    print("  Ctrl+C 停止")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止")
        srv.server_close()


if __name__ == "__main__":
    main()
