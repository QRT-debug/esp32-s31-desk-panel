"""ESP32-S31 语音识别中转（PC 侧）

板子把 16kHz 单声道 WAV POST 到 http://<this-pc>:8080/api/asr，本服务转 ASR 后返回纯文本。

两种后端二选一（环境变量切换）：

  1) OpenAI 兼容 API（推荐先试，最简单）
     set ASR_BACKEND=openai
     set OPENAI_API_KEY=sk-xxxx
     set OPENAI_BASE_URL=https://api.openai.com/v1        （可换国内兼容网关）
     python tools/asr_server.py

  2) 本地 whisper.cpp（免费离线，需要先下载模型）
     set ASR_BACKEND=whisper_cpp
     set WHISPER_EXE=C:\\whisper\\whisper-cli.exe
     set WHISPER_MODEL=C:\\whisper\\ggml-base.bin
     python tools/asr_server.py

不装任何第三方库：标准库 http.server + urllib（openai 后端用 requests 风格手写）。
"""
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(os.environ.get("ASR_PORT", "8080"))
BACKEND = os.environ.get("ASR_BACKEND", "openai").lower()


def asr_openai(wav_path: str, lang: str = "zh") -> str:
    key = os.environ.get("OPENAI_API_KEY", "")
    base = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/")
    model = os.environ.get("OPENAI_ASR_MODEL", "whisper-1")
    if not key:
        raise RuntimeError("OPENAI_API_KEY 未设置")

    boundary = "----esp32s31boundary"
    with open(wav_path, "rb") as f:
        audio = f.read()

    body = b""
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="model"\r\n\r\n'
    body += model.encode() + b"\r\n"
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="language"\r\n\r\n'
    body += lang.encode() + b"\r\n"
    body += f"--{boundary}\r\n".encode()
    body += b'Content-Disposition: form-data; name="file"; filename="a.wav"\r\n'
    body += b"Content-Type: audio/wav\r\n\r\n"
    body += audio + b"\r\n"
    body += f"--{boundary}--\r\n".encode()

    req = urllib.request.Request(
        f"{base}/audio/transcriptions",
        data=body,
        headers={
            "Authorization": f"Bearer {key}",
            "Content-Type": f"multipart/form-data; boundary={boundary}",
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode()).get("text", "").strip()


def asr_whisper_cpp(wav_path: str, lang: str = "zh") -> str:
    exe = os.environ.get("WHISPER_EXE", "")
    model = os.environ.get("WHISPER_MODEL", "")
    if not exe or not model:
        raise RuntimeError("WHISPER_EXE / WHISPER_MODEL 未设置")
    out = subprocess.run(
        [exe, "-m", model, "-f", wav_path, "-l", lang, "-nt", "-np"],
        capture_output=True, text=True, timeout=120,
    )
    if out.returncode != 0:
        raise RuntimeError(out.stderr[-300:])
    return " ".join(out.stdout.split())


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[asr]", fmt % args, flush=True)

    def do_POST(self):
        if self.path != "/api/asr":
            self.send_error(404)
            return
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n > 0 else b""
        if len(raw) < 100:
            self.send_error(400, "too short")
            return

        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            f.write(raw)
            path = f.name

        try:
            text = asr_openai(path) if BACKEND == "openai" else asr_whisper_cpp(path)
            print(f"[asr] {len(raw)}B -> \"{text}\"", flush=True)
        except Exception as e:
            text = ""
            print(f"[asr] failed: {e}", flush=True)
            self.send_error(500, str(e))
            os.unlink(path)
            return

        os.unlink(path)
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    print(f"ASR 中转启动: http://0.0.0.0:{PORT}/api/asr  (backend={BACKEND})", flush=True)
    if BACKEND == "openai" and not os.environ.get("OPENAI_API_KEY"):
        print("警告：OPENAI_API_KEY 未设置，请求会失败", flush=True)
    if BACKEND == "whisper_cpp" and not os.environ.get("WHISPER_EXE"):
        print("警告：WHISPER_EXE 未设置，请求会失败", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
