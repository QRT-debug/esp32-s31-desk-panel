"""持续监听串口，带时间戳存文件。用法: python serial_watch.py [秒数]"""
import sys, time
import serial

PORT = "COM5"
BAUD = 115200
dur = int(sys.argv[1]) if len(sys.argv) > 1 else 150

s = serial.Serial(PORT, BAUD, timeout=0.3)
start = time.time()
n = 0
with open("mic_session.log", "w", encoding="utf-8", errors="replace") as f:
    f.write(f"# listening {PORT} @ {BAUD} for {dur}s\n")
    while time.time() - start < dur:
        try:
            line = s.readline()
        except Exception as e:
            f.write(f"# serial error: {e}\n"); break
        if line:
            t = time.time() - start
            txt = line.decode("utf-8", "replace").rstrip()
            f.write(f"[{t:6.1f}] {txt}\n")
            f.flush()
            n += 1
s.close()
print(f"captured {n} lines in {dur}s")
