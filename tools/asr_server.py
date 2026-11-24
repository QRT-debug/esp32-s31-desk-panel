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
            # ★ 强制中文：language="" 是自动检测，短词（"应用"只有 0.8s）常被判成英文，
            #   输出成 "应ing用"/"应ingyong" 这种中英混杂（实测踩到）。
            language="zh",
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


# ================================================================== AI 对话 + TTS
# 2026-11-24 新增：让设备能"说话→回话"。
#   POST /api/chat  {"text":"..."}                 -> {"reply":"...","emotion":"..."}
#   POST /api/tts   {"text":"...","emotion":"..."}  -> 二进制 WAV
#   POST /api/reply {"text":"..."}                  -> 二进制 WAV（内部 chat+tts，一次到位）
#
# ★ 延迟构成：LLM 0.7~2s + edge-tts 0.5~1s + ffmpeg 0.2s ≈ 1.5~3s
# ★ 情感控制是"近似"的：edge-tts 不支持 style/emotion，只能用 rate/pitch 微调。
#   真情感要等 CosyVoice2 / GPT-SoVITS 那一步。

LLM_URL   = os.environ.get("LLM_URL", "http://127.0.0.1:1234/v1/chat/completions")
LLM_MODEL = os.environ.get("LLM_MODEL", "")          # 空 = 问 /v1/models 拿第一个
TTS_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")

# 人设：爱弥斯（Aemeath），星炬学院学生，自称"救世主"
# 素材来自参考音频的台词：活泼自信、偶尔中二、重感情
# ★ 7B 小模型的指令跟随不稳（实测会把情绪词写两遍、留空括号），
#   所以格式要求写得极死，并且 parse 后再做一次清洗。
PERSONA = os.environ.get("PERSONA", """你是爱弥斯（Aemeath），星炬学院的学生，自称"救世主"。
性格：活泼自信，说话带点调侃和中二，但内心重感情、在意身边的人。

输出格式（严格遵守）：
[情绪]台词

- 情绪只能是这五个词之一：开心 / 忧伤 / 埋怨 / 温柔 / 中立
- 方括号后面**直接接台词**，不要空格、不要重复情绪词、不要再加任何括号
- 台词口语化，1~2 句，40 字以内
- 不写动作、表情、旁白、心理描写

示例：
用户：今天好累啊
你：[温柔]那就歇会儿吧，我陪你坐一会。""")

# 情绪 → (语速, 音高)。edge-tts 只认这种近似控制，不是真情感模型。
EMOTION_MAP = {
    "开心": ("+14%", "+30Hz"),
    "忧伤": ("-10%", "-25Hz"),
    "埋怨": ("+6%", "+10Hz"),
    "温柔": ("-6%", "-10Hz"),
    "中立": ("+0%", "+0Hz"),
}

# ---------------------------------------------------------------- TTS 后端选择
# TTS_BACKEND=edge    → edge-tts（快、通用女声、情感靠语速近似）
# TTS_BACKEND=sovits  → GPT-SoVITS（爱弥斯音色、情感由参考音频决定、慢 1~2s）
TTS_BACKEND = os.environ.get("TTS_BACKEND", "edge").lower()
SOVITS_URL = os.environ.get("SOVITS_URL", "http://127.0.0.1:9880/tts")
SOVITS_ROOT = os.environ.get(
    "SOVITS_ROOT",
    r"D:\GPT-SoVITS-v2pro-nvidia50\GPT-SoVITS-v2pro-20250604-nvidia50")

# 情绪 → (参考音频, 该音频的台词)。GPT-SoVITS 靠参考音频同时决定音色和情绪，
# ★ prompt_text 必须与参考音频内容逐字一致，否则合成会跑偏甚至失败。
SOVITS_REFS = {
    "开心": ("ref/happy.wav",   "感受星炬学院，在那些麻烦事之外的青春气息,这十几年来，我随手搭来的发歌网站"),
    "忧伤": ("ref/sad.wav",     "全员覆没，所以后来，我再没有在学院见过他们，我还以为只是错过了"),
    "埋怨": ("ref/neutral.wav", "想要拯救的越多，付出的代价就越大，但有些事，必须去做"),
    "温柔": ("ref/sad.wav",     "全员覆没，所以后来，我再没有在学院见过他们，我还以为只是错过了"),
    "中立": ("ref/neutral.wav", "想要拯救的越多，付出的代价就越大，但有些事，必须去做"),
}


def _to_44k_mono(audio_bytes, suffix=".wav"):
    """统一转成设备要的格式：44100Hz / 16bit / 单声道。
    ★ edge-tts 出 24kHz、GPT-SoVITS 出 32kHz，而设备 I2S 固定按 44.1kHz 播，
      不重采样就会变调（音调/速度都不对）。"""
    import subprocess
    import tempfile

    import imageio_ffmpeg

    src = tempfile.mktemp(suffix=suffix)
    dst = tempfile.mktemp(suffix=".wav")
    try:
        with open(src, "wb") as f:
            f.write(audio_bytes)
        subprocess.run(
            [imageio_ffmpeg.get_ffmpeg_exe(), "-y", "-i", src,
             "-ar", "44100", "-ac", "1", "-acodec", "pcm_s16le", dst],
            capture_output=True, timeout=90, check=True)
        with open(dst, "rb") as f:
            return f.read()
    finally:
        for p in (src, dst):
            try:
                os.unlink(p)
            except OSError:
                pass


def synth_sovits(text, emotion="中立"):
    """调 GPT-SoVITS api_v2（返回 32kHz WAV），再重采样到 44.1k。"""
    import json
    import urllib.request

    ref, prompt = SOVITS_REFS.get(emotion, SOVITS_REFS["中立"])
    body = {
        "text": text,
        "text_lang": "zh",
        "ref_audio_path": os.path.join(SOVITS_ROOT, ref),
        "prompt_text": prompt,
        "prompt_lang": "zh",
        "media_type": "wav",
        "streaming_mode": False,
    }
    req = urllib.request.Request(
        SOVITS_URL, data=json.dumps(body, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json"})
    with _local_opener().open(req, timeout=180) as r:
        raw = r.read()
    return _to_44k_mono(raw, suffix=".wav")


def _local_opener():
    """拒绝一切代理：本服务只连本机 LM Studio 和微软 TTS，代理只会添乱。"""
    import urllib.request
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def fetch_chat(user_text):
    """调 LM Studio（OpenAI 兼容）→ (回复文本, 情绪)"""
    import json
    import re as _re
    import urllib.request

    model = LLM_MODEL
    if not model:
        try:
            with _local_opener().open("http://127.0.0.1:1234/v1/models", timeout=10) as r:
                ms = json.load(r).get("data") or []
            model = ms[0]["id"] if ms else "local-model"
        except Exception:
            model = "local-model"

    body = {
        "model": model,
        "messages": [{"role": "system", "content": PERSONA},
                     {"role": "user", "content": user_text}],
        "temperature": 0.8,
        "max_tokens": 120,
        "stream": False,
    }
    req = urllib.request.Request(
        LLM_URL, data=json.dumps(body).encode("utf-8"),
        headers={"Content-Type": "application/json"})
    with _local_opener().open(req, timeout=90) as r:
        d = json.load(r)
    content = (d["choices"][0]["message"]["content"] or "").strip()

    emo = "中立"
    m = _re.match(r"^\s*[\[【]([^\]】]{1,6})[\]】]\s*", content)
    if m:
        cand = m.group(1).strip()
        if cand in EMOTION_MAP:
            emo = cand
            content = content[m.end():]

    # ★ 后处理清洗：7B 模型实测会犯这些毛病，不清掉会被 TTS 念出来
    #   ① 正文开头又重复了一遍情绪词（"中立 那就偷个懒"）
    #   ② 残留空括号 / 多余的方括号标记（"[ ] 哇塞"）
    for _ in range(3):
        content = content.lstrip()
        content = _re.sub(r"^\s*[\[【][^\]】]{0,6}[\]】]\s*", "", content)   # 多余标签
        hit = False
        for w in EMOTION_MAP:
            if content.startswith(w):
                content = content[len(w):].lstrip(" ，,、:：。")
                hit = True
                break
        if not hit:
            break
    content = content.strip().strip("[]【】").strip()
    return content, emo


def synth_wav(text, emotion="中立"):
    """按 TTS_BACKEND 选引擎，统一返回 44100/16bit/mono 的 WAV 字节。"""
    if TTS_BACKEND == "sovits":
        return synth_sovits(text, emotion)

    import asyncio

    import edge_tts                      # 延迟导入：没装也不影响 /api/asr

    rate, pitch = EMOTION_MAP.get(emotion, EMOTION_MAP["中立"])
    tmp_mp3 = tempfile.mktemp(suffix=".mp3")

    async def _run():
        c = edge_tts.Communicate(text, TTS_VOICE, rate=rate, pitch=pitch)
        await c.save(tmp_mp3)

    try:
        asyncio.run(_run())
        with open(tmp_mp3, "rb") as f:
            mp3 = f.read()
        return _to_44k_mono(mp3, suffix=".mp3")   # edge 出 24kHz，必须重采样
    finally:
        try:
            os.unlink(tmp_mp3)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print("[asr]", fmt % args, flush=True)

    def _json_body(self):
        import json
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n > 0 else b"{}"
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except Exception:
            return {}

    def _send_bytes(self, data, ctype, extra=None):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        # ---------- 新增：AI 对话 ----------
        if self.path == "/api/chat":
            d = self._json_body()
            text = (d.get("text") or "").strip()
            if not text:
                self.send_error(400, "empty text")
                return
            try:
                reply, emo = fetch_chat(text)
            except Exception as e:
                print(f"[chat] failed: {e}", flush=True)
                self.send_error(503, str(e))
                return
            print(f"[chat] \"{text}\" -> [{emo}] {reply}", flush=True)
            import json as _j
            body = _j.dumps({"reply": reply, "emotion": emo},
                            ensure_ascii=False).encode("utf-8")
            self._send_bytes(body, "application/json; charset=utf-8")
            return

        # ---------- 新增：TTS / 一次到位 ----------
        if self.path in ("/api/tts", "/api/reply"):
            d = self._json_body()
            text = (d.get("text") or "").strip()
            emo = (d.get("emotion") or "中立").strip()
            if not text:
                self.send_error(400, "empty text")
                return
            reply = text
            if self.path == "/api/reply":          # 先对话，再合成，一次请求拿到音频
                try:
                    reply, emo = fetch_chat(text)
                except Exception as e:
                    print(f"[reply] chat failed: {e}", flush=True)
                    self.send_error(503, f"chat failed: {e}")
                    return
            try:
                wav = synth_wav(reply, emo)
            except Exception as e:
                print(f"[tts] failed: {e}", flush=True)
                self.send_error(500, str(e))
                return
            import urllib.parse as _up
            print(f"[tts] [{emo}] {reply[:30]} -> {len(wav)}B", flush=True)
            self._send_bytes(wav, "audio/wav",
                             {"X-Reply-Text": _up.quote(reply),
                              "X-Emotion": _up.quote(emo)})
            return

        # ---------- 原有：ASR ----------
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
    print(f"  + /api/chat   POST {{text}}          -> {{reply, emotion}}   (LLM: {LLM_URL})", flush=True)
    print(f"  + /api/tts    POST {{text, emotion}} -> WAV                  "
          f"(backend={TTS_BACKEND}"
          + (f", voice={TTS_VOICE}" if TTS_BACKEND == "edge" else f", url={SOVITS_URL}")
          + ")", flush=True)
    print(f"  + /api/reply  POST {{text}}          -> WAV（对话+合成一次到位）", flush=True)
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
