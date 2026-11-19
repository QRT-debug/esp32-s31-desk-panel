"""截屏工具（纯标准库，靠 ctypes 调 GDI32，不依赖 Pillow）

用法：python tools\\screenshot.py [输出路径] [--window 标题片段]
  不带 --window：截整个屏幕
  带 --window  ：只截标题含该片段的窗口（找不到则退回全屏）

输出 BMP，可直接用看图工具/模型读。
"""
import ctypes
import struct
import sys
from ctypes import wintypes

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# 让进程感知 DPI，否则高缩放屏上截出来是模糊放大的
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass


def find_window(keyword):
    """按标题片段找窗口，返回 (x, y, w, h) 或 None"""
    result = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def enum_proc(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n == 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        if keyword.lower() in title.lower():
            rect = wintypes.RECT()
            user32.GetClientRect(hwnd, ctypes.byref(rect))
            pt = wintypes.POINT(0, 0)
            user32.ClientToScreen(hwnd, ctypes.byref(pt))
            w = rect.right - rect.left
            h = rect.bottom - rect.top
            if w > 0 and h > 0:
                result.append((pt.x, pt.y, w, h, title, hwnd))
        return True

    user32.EnumWindows(enum_proc, 0)
    return result[0] if result else None


def grab(x, y, w, h, out):
    hdc = user32.GetDC(0)
    memdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(memdc, bmp)

    SRCCOPY = 0x00CC0020
    gdi32.BitBlt(memdc, 0, 0, w, h, hdc, x, y, SRCCOPY)

    buf = ctypes.create_string_buffer(w * h * 4)
    header = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, w * h * 4, 0, 0, 0, 0)
    gdi32.GetDIBits(memdc, bmp, 0, h, buf, header, 0)

    with open(out, "wb") as f:
        f.write(b"BM")
        f.write(struct.pack("<IHHI", 14 + 40 + w * h * 4, 0, 0, 14 + 40))
        f.write(header)
        f.write(buf.raw)

    gdi32.SelectObject(memdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(memdc)
    user32.ReleaseDC(0, hdc)


def main():
    out = "screenshot.bmp"
    keyword = None
    args = sys.argv[1:]
    if args and not args[0].startswith("--"):
        out = args[0]
        args = args[1:]
    if "--window" in args:
        i = args.index("--window")
        if i + 1 < len(args):
            keyword = args[i + 1]

    if keyword:
        win = find_window(keyword)
        if win:
            x, y, w, h, title, hwnd = win
            print(f"window: [{title}] {w}x{h} at ({x},{y})")

            # ★ 必须先提到最前：SDL 窗口若被别的窗口挡住，BitBlt 只能截到遮挡物
            import time
            SW_RESTORE = 9
            HWND_TOPMOST = -1
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            user32.ShowWindow(hwnd, SW_RESTORE)
            user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            user32.SetForegroundWindow(hwnd)
            time.sleep(0.4)

            grab(x, y, w, h, out)

            HWND_NOTOPMOST = -2
            user32.SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)
            print("saved", out)
            return 0
        print(f"window '{keyword}' not found, fallback to full screen")

    w = user32.GetSystemMetrics(0)
    h = user32.GetSystemMetrics(1)
    print(f"full screen {w}x{h}")
    grab(0, 0, w, h, out)
    print("saved", out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
