#!/usr/bin/env python3
"""
统一服务管理器 —— 一次管住面板需要的所有 PC 侧服务。

为什么要它：以前重启电脑要分别双击好几个 bat，窗口散在各处，
不知道哪个还活着、哪个挂了。现在一张表看全。

★ 核心设计（决定了"某个服务挂了要不要全重启"）：
  - 每个服务是**独立子进程**，一个挂了不影响另一个；
  - 启动是**幂等**的：先探端口，已在监听就跳过 ⇒ 双击两次不会起两个实例；
  - **可以单独重启**：`restart asr` 只动 asr，不碰 monitor
    （monitor 正在被设备拉数据时，没必要陪着重启）；
  - 状态以"端口是否真的在监听"为准，不以"PID 文件存在"为准
    （PID 文件会残留，端口不会骗人）。

用法：
    python tools/services.py status          看状态（默认）
    python tools/services.py start           启动全部（已跑着的跳过）
    python tools/services.py stop            停止全部
    python tools/services.py restart asr     只重启语音服务
    python tools/services.py log asr         打印该服务的日志尾部
"""

import os
import socket
import subprocess
import sys
import time
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(ROOT, "logs")
PID_FILE = os.path.join(ROOT, "Temp", "services.pid")

# ---------------------------------------------------------------- 服务清单
# cwd 很重要：asr_server 要在工程根目录找 models/，monitor 在 tools/ 里跑
SERVICES = {
    "monitor": {
        "desc": "PC 主机监控 + B站热搜中转",
        "script": os.path.join(TOOLS, "pc_monitor_server.py"),
        "cwd": TOOLS,
        "port": 8080,
        "env": {},
    },
    "asr": {
        "desc": "语音识别（SenseVoice-Small）",
        "script": os.path.join(TOOLS, "asr_server.py"),
        "cwd": ROOT,
        "port": 8081,
        "env": {
            "ASR_BACKEND": "sensevoice",
            "SV_DIR": os.path.join("models", "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17"),
        },
    },
}

PYTHON = sys.executable          # 用当前解释器，避免路径写死
C_OK, C_BAD, C_DIM, C_END = "\033[32m", "\033[31m", "\033[90m", "\033[0m"


# ---------------------------------------------------------------- 基础工具
def port_open(port, host="127.0.0.1", timeout=0.3):
    """端口是否有人监听 —— 判断"服务活着"最可靠的方式"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        return s.connect_ex((host, port)) == 0
    finally:
        s.close()


def load_pids():
    out = {}
    if not os.path.exists(PID_FILE):
        return out
    try:
        with open(PID_FILE, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 2:
                    out[parts[0]] = int(parts[1])
    except Exception:
        pass
    return out


def save_pids(pids):
    os.makedirs(os.path.dirname(PID_FILE), exist_ok=True)
    with open(PID_FILE, "w", encoding="utf-8") as f:
        for name, pid in pids.items():
            f.write(f"{name} {pid}\n")


def pid_alive(pid):
    if not pid:
        return False
    try:
        # Windows 上 os.kill(pid, 0) 会抛异常表示不存在
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# ---------------------------------------------------------------- 核心动作
def start_one(name, quiet=False):
    svc = SERVICES[name]
    if port_open(svc["port"]):
        if not quiet:
            print(f"  {C_DIM}{name:8s} 已在运行（端口 {svc['port']} 有监听），跳过{C_END}")
        return True

    os.makedirs(LOG_DIR, exist_ok=True)
    log_path = os.path.join(LOG_DIR, f"{name}.log")
    env = os.environ.copy()
    env.update(svc["env"])
    env["PYTHONIOENCODING"] = "utf-8"

    # 追加写日志，保留历史（重启后能看上一次为什么挂）
    logf = open(log_path, "a", encoding="utf-8", errors="replace")
    logf.write(f"\n{'=' * 60}\n[{datetime.now():%Y-%m-%d %H:%M:%S}] start {name}\n")
    logf.flush()

    # ★ 必须 DETACHED_PROCESS：否则本脚本（或启动它的窗口）一退出，
    #   子进程会被一起收走 —— 实测过，服务日志里只有启动行、没有报错，
    #   进程却没了，就是这个原因。CREATE_NEW_PROCESS_GROUP 让它不接收父进程的 Ctrl+C。
    flags = 0
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    try:
        proc = subprocess.Popen(
            [PYTHON, svc["script"]],
            cwd=svc["cwd"],
            env=env,
            stdout=logf,
            stderr=subprocess.STDOUT,
            creationflags=flags,
        )
    except Exception as e:
        print(f"  {C_BAD}{name:8s} 启动失败：{e}{C_END}")
        return False

    # 等端口起来（asr 要加载模型，可能 5~20 秒）
    wait = 25 if name == "asr" else 8
    for _ in range(wait * 10):
        if port_open(svc["port"]):
            pids = load_pids()
            pids[name] = proc.pid
            save_pids(pids)
            print(f"  {C_OK}{name:8s} 已启动  PID {proc.pid}  端口 {svc['port']}{C_END}")
            logf.close()
            return True
        if proc.poll() is not None:       # 进程已退出
            print(f"  {C_BAD}{name:8s} 启动后立即退出，看日志：{log_path}{C_END}")
            for line in tail(log_path, 8):
                print(f"      {C_DIM}{line}{C_END}")
            logf.close()
            return False
        time.sleep(0.1)

    print(f"  {C_BAD}{name:8s} {wait}s 内端口未就绪（进程还在，可能模型加载慢）{C_END}")
    logf.close()
    return False


def stop_one(name, quiet=False):
    svc = SERVICES[name]
    pids = load_pids()
    pid = pids.get(name)
    killed = False

    if pid and pid_alive(pid):
        try:
            os.kill(pid, 15)              # Windows: TerminateProcess
            killed = True
        except OSError:
            pass
    # 兜底：PID 丢了但端口还占着 → 按端口找进程
    if not killed and port_open(svc["port"]):
        try:
            # ★ 不能写 text=True：netstat 输出是 GBK，用 UTF-8 解会抛 UnicodeDecodeError
            raw = subprocess.run(["netstat", "-ano"], capture_output=True, timeout=10).stdout
            for line in raw.decode("gbk", errors="replace").splitlines():
                if f":{svc['port']}" in line and "LISTENING" in line:
                    p = int(line.split()[-1])
                    os.kill(p, 15)
                    killed = True
                    break
        except Exception:
            pass

    if killed:
        for _ in range(50):
            if not port_open(svc["port"]):
                break
            time.sleep(0.1)
        if not quiet:
            print(f"  {C_OK}{name:8s} 已停止{C_END}")
    elif not quiet:
        print(f"  {C_DIM}{name:8s} 本来就没在运行{C_END}")

    pids.pop(name, None)
    save_pids(pids)
    return True


def tail(path, n=10):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except Exception:
        return []


def status():
    print(f"\n{'服务':<10}{'状态':<10}{'端口':<8}{'PID':<8}说明")
    print("-" * 72)
    alive = 0
    for name, svc in SERVICES.items():
        up = port_open(svc["port"])
        pid = load_pids().get(name)
        # PID 只在"端口活着且进程还在"时才显示：PID 文件会残留，不能当证据
        shown_pid = pid if (up and pid and pid_alive(pid)) else "-"
        if up:
            alive += 1
            state = f"{C_OK}运行中{C_END}"
        else:
            state = f"{C_BAD}已停止{C_END}"
        print(f"{name:<10}{state:<19}{svc['port']:<8}{str(shown_pid):<8}{svc['desc']}")
    print("-" * 72)
    print(f"  {alive}/{len(SERVICES)} 个服务在运行    日志目录：logs/")
    if alive < len(SERVICES):
        print(f"  启动：{C_DIM}python tools/services.py start{C_END}")
    print()


# ---------------------------------------------------------------- 命令分发
def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    target = sys.argv[2] if len(sys.argv) > 2 else None

    if cmd == "status":
        status()

    elif cmd == "start":
        print("\n启动服务：")
        names = [target] if target else list(SERVICES)
        for n in names:
            if n not in SERVICES:
                print(f"  未知服务：{n}（可选：{', '.join(SERVICES)}）")
                continue
            start_one(n)
        print()
        status()

    elif cmd == "stop":
        print("\n停止服务：")
        names = [target] if target and target != "all" else list(SERVICES)
        for n in names:
            if n in SERVICES:
                stop_one(n)
        print()

    elif cmd == "restart":
        names = [target] if target else list(SERVICES)
        print(f"\n重启：{', '.join(names)}")
        for n in names:
            if n not in SERVICES:
                print(f"  未知服务：{n}")
                continue
            stop_one(n)
            start_one(n)
        print()
        status()

    elif cmd == "log":
        n = target or "asr"
        if n not in SERVICES:
            print(f"未知服务：{n}")
            return
        path = os.path.join(LOG_DIR, f"{n}.log")
        print(f"\n--- {path}（最后 30 行）---")
        for line in tail(path, 30):
            print(line)
        print()

    else:
        print(__doc__)


if __name__ == "__main__":
    main()
