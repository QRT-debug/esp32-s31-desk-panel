#!/usr/bin/env python3
"""解析 Wallpaper Engine 的 scene.pkg（PKGV0018）并列出/提取内部文件。

格式（从 scene.pkg 的实际字节推断，已用 hexdump 验证前 32 字节）：
    uint32  version_len
    char[]  version        "PKGV0018"
    uint32  file_count
    repeat file_count:
        uint32 name_len
        char[] name         UTF-8，例如 "scene.json"、"materials/xxx.json"
        uint32 offset       数据在包内的绝对偏移
        uint32 size         数据长度

用法：
    python tools/unpack_pkg.py <scene.pkg> [输出目录]
不给输出目录时只列清单，不落盘。
"""
import os
import struct
import sys


def parse(pkg_path):
    with open(pkg_path, "rb") as f:
        data = f.read()

    ver_len = struct.unpack_from("<I", data, 0)[0]
    version = data[4:4 + ver_len].decode("ascii", "replace")
    pos = 4 + ver_len

    count = struct.unpack_from("<I", data, pos)[0]
    pos += 4

    files = []
    for _ in range(count):
        nl = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        name = data[pos:pos + nl].decode("utf-8", "replace")
        pos += nl
        off = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        size = struct.unpack_from("<I", data, pos)[0]
        pos += 4
        files.append((name, off, size))
    return version, files, data


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    pkg = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else None

    version, files, data = parse(pkg)
    print(f"{version}: {len(files)} 个文件")
    print("-" * 70)

    # 按扩展名统计，先让调用者看清里面有什么格式
    ext_count = {}
    for name, _off, _size in files:
        ext = os.path.splitext(name)[1].lower() or "(无扩展名)"
        ext_count[ext] = ext_count.get(ext, 0) + 1
    print("扩展名统计:", ", ".join(f"{k}×{v}" for k, v in sorted(ext_count.items())))
    print("-" * 70)

    for name, off, size in files:
        print(f"  {size:>10,} B   {name}")

    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        for name, off, size in files:
            dst = os.path.join(out_dir, name.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(data[off:off + size])
        print("-" * 70)
        print(f"已提取到 {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
