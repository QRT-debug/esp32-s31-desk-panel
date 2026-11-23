"""ESP32-S31 语音识别中转（PC 侧）

板子把 16kHz 单声道 WAV POST 到 http://<this-pc>:8080/api/asr，本服务转 ASR 后返回纯文本。

两种后端二选一（环境变量切换）：

  1) OpenAI 兼容 API（推荐先试，最简单）
     set ASR_BACKEND=openai
     set OPENAI_API_KEY=sk-xxxx
     set OPENAI_BASE_URL=https://api.openai.com/v1        （可换国内兼容网关）
     python tools/asr_server.py

  2) 本地 faster-whisper（推荐，pip 装完即用，首次自动下模型）
     set ASR_BACKEND=local
     set WHISPER_SIZE=base            （tiny/base/small/medium，越大越准越慢）
     python tools/asr_server.py

  3) 本地 SenseVoice-Small（★ 中文首选：CER 约为 whisper 一半，CPU 极快）
     set ASR_BACKEND=sensevoice
     python tools/asr_server.py
     （模型目录默认 models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17，
       可用 SV_DIR 覆盖；用 sherpa-onnx 推理，无需 PyTorch）

  4) 本地 whisper.cpp（免费离线，需要先下载模型）
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

PORT = int(os.environ.get("ASR_PORT", "8081"))
BACKEND = os.environ.get("ASR_BACKEND", "sensevoice").lower()   # 默认中文专用：CER 约为 whisper 一半，CPU 30x 实时


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


_fw_model = None


def asr_local(wav_path: str, lang: str = "zh") -> str:
    """本地 faster-whisper（首次运行自动下载模型，走 hf-mirror 镜像）。"""
    global _fw_model
    os.environ.setdefault("HF_ENDPOINT", "https://hf-mirror.com")
    if _fw_model is None:
        from faster_whisper import WhisperModel
        size = os.environ.get("WHISPER_SIZE", "base")
        print(f"[asr] loading faster-whisper '{size}' (first run downloads model)...", flush=True)
        _fw_model = WhisperModel(size, device="cpu", compute_type="int8")
        print("[asr] model ready", flush=True)
    # ★ 不用 PyAV 解码：板子发来的是标准 16k 单声道 16bit PCM WAV，
    #   用标准库 wave 读成 numpy 再喂模型（避开 av 库的版本兼容坑）。
    import wave
    import numpy as np
    with wave.open(wav_path, "rb") as w:
        frames = w.readframes(w.getnframes())
    audio = np.frombuffer(frames, dtype=np.int16).astype(np.float32) / 32768.0
    # 板子固定 16k；若不是（如本地测试用 22.05k），线性重采样
    if w.getframerate() != 16000:
        n = int(len(audio) * 16000 / w.getframerate())
        audio = np.interp(np.linspace(0, len(audio) - 1, n),
                          np.arange(len(audio)), audio).astype(np.float32)
    # ★ initial_prompt 引导简体中文输出（否则 base 模型常出繁体）
    segs, _ = _fw_model.transcribe(
        audio, language=lang, beam_size=1, vad_filter=True,
        initial_prompt="以下是普通话的设备控制指令，涉及屏保、背光、亮度、天气、时钟、音量、场景切换。",
    )
    return "".join(s.text for s in segs).strip()


_sv_rec = None

SV_DIR_DEFAULT = "models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17"


def asr_sensevoice(wav_path: str, lang: str = "zh") -> str:
    """本地 SenseVoice-Small（sherpa-onnx 推理，非自回归，中文 CER ~7.8%）。

    ★ 为什么不用 whisper：whisper 中文训练数据占比低，同等规模 CER 约两倍；
      SenseVoice 是中文专用、非自回归（一次前向出整句），CPU 上就有 17x 实时速度。
    """
    global _sv_rec
    if _sv_rec is None:
        import sherpa_onnx
        d = os.environ.get("SV_DIR", SV_DIR_DEFAULT)
        if not os.path.isdir(d):
            raise RuntimeError(f"模型目录不存在: {d}")
        # int8 量化版优先（体积小、速度更快，精度损失可忽略）
        model = os.path.join(d, "model.int8.onnx")
        if not os.path.exists(model):
            model = os.path.join(d, "model.onnx")
        print(f"[asr] loading SenseVoice: {os.path.basename(model)}", flush=True)
        _sv_rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=model,
            tokens=os.path.join(d, "tokens.txt"),
            use_itn=True,          # 数字/标点规范化
            num_threads=4,
            debug=False,
        )
        print("[asr] SenseVoice ready", flush=True)

    import wave
    import numpy as np
    with wave.open(wav_path, "rb") as w:
        sr = w.getframerate()
        a = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
    if sr != 16000:
        n = int(len(a) * 16000 / sr)
        a = np.interp(np.linspace(0, len(a) - 1, n), np.arange(len(a)), a).astype(np.float32)

    st = _sv_rec.create_stream()
    st.accept_waveform(16000, a)
    _sv_rec.decode_stream(st)
    return st.result.text.strip()


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
            if BACKEND == "openai":
                text = asr_openai(path)
            elif BACKEND == "sensevoice":
                text = asr_sensevoice(path)
            elif BACKEND == "local":
                text = asr_local(path)
            else:
                text = asr_whisper_cpp(path)
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
    if BACKEND == "sensevoice":
        print(f"模型目录: {os.environ.get('SV_DIR', SV_DIR_DEFAULT)}", flush=True)
    if BACKEND == "local":
        print(f"本地模型: {os.environ.get('WHISPER_SIZE', 'base')} "
              f"(HF_ENDPOINT={os.environ.get('HF_ENDPOINT', 'https://hf-mirror.com')})", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    sys.exit(main())
