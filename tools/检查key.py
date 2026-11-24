#!/usr/bin/env python3
"""检查 tools/llm.json 里的 API key 是否可用。

用法：
    python tools/检查key.py

输出：
    ✅ key 可用（并显示模型回的一句话）
    ❌ 401 / 403 → key 无效或已吊销
    ❌ 其他      → 网络/地址/模型名问题
"""
import json
import os
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = os.path.join(HERE, "llm.json")


def main():
    if not os.path.exists(CFG):
        print(f"✗ 找不到 {CFG}")
        print("  先复制 llm.example.json 为 llm.json，再填 url/key/model")
        return 1

    with open(CFG, encoding="utf-8") as f:
        cfg = json.load(f)

    url = (cfg.get("url") or "").strip()
    key = (cfg.get("key") or "").strip()
    model = (cfg.get("model") or "").strip()

    print(f"配置文件：{CFG}")
    print(f"  url   : {url or '(空)'}")
    print(f"  model : {model or '(空)'}")
    print(f"  key   : {(key[:6] + '****' + key[-4:]) if len(key) > 10 else ('(空)' if not key else '(太短)')}")
    print()

    if not url or not key or not model:
        print("✗ 三项都要填（url / key / model）")
        return 1
    if "你的key" in key:
        print("✗ key 还是模板里的占位文字")
        return 1

    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": "只回复两个字：收到"}],
        "max_tokens": 20,
    }).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",
    })

    print("正在请求…")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            d = json.load(r)
        txt = (d.get("choices") or [{}])[0].get("message", {}).get("content", "")
        print(f"✅ key 可用，模型回了：{txt.strip()!r}")
        return 0
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        print(f"✗ HTTP {e.code}")
        print(f"  {detail}")
        if e.code in (401, 403):
            print("\n→ key 无效或已被吊销，去平台后台重新生成一把")
        elif e.code == 404:
            print("\n→ 404 通常是 url 少了 /v1 或 model 名字写错")
        elif e.code == 429:
            print("\n→ 限流或余额不足")
        return 1
    except Exception as e:
        print(f"✗ {type(e).__name__}: {e}")
        print("\n→ 网络/代理问题，或 url 不可达")
        return 1


if __name__ == "__main__":
    sys.exit(main())
