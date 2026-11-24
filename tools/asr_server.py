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
import collections
import json
import os
import subprocess
import sys
import tempfile
import urllib.error
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
LLM_KEY   = os.environ.get("LLM_KEY", "")            # 云端 API 的 key（本地 LM Studio 留空）
LLM_MODEL = os.environ.get("LLM_MODEL", "")          # 空 = 问 /v1/models 拿第一个
TTS_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")

# 人设：爱弥斯（Aemeath），星炬学院学生，自称"救世主"
# ★ 正式的人设文本在 tools/persona.txt，改那个文件就行，不用碰代码；
#   改完重启 asr 生效。文件不存在时才用下面这段内置默认值。
# ★ 7B 小模型的指令跟随不稳（实测会把情绪词写两遍、留空括号），
#   所以格式要求写得极死，并且 parse 后再做一次清洗。
_DEFAULT_PERSONA = """你是爱弥斯（Aemeath），星炬学院的学生，自称"救世主"。
性格：活泼自信，说话带点调侃和中二，但内心重感情、在意身边的人。

输出格式（严格遵守）：
[情绪]台词

- 情绪只能是这五个词之一：开心 / 忧伤 / 埋怨 / 温柔 / 中立
- 方括号后面**直接接台词**，不要空格、不要重复情绪词、不要再加任何括号
- 台词口语化，1~2 句，40 字以内
- 不写动作、表情、旁白、心理描写

示例：
用户：今天好累啊
你：[温柔]那就歇会儿吧，我陪你坐一会。"""


def _load_persona():
    """优先读 tools/persona.txt（用户可直接编辑），读不到/为空则用内置默认。"""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "persona.txt")
    try:
        with open(p, encoding="utf-8") as f:
            t = f.read().strip()
        if t:
            return t
    except OSError:
        pass
    return _DEFAULT_PERSONA


PERSONA = os.environ.get("PERSONA") or _load_persona()

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
# ★★★ prompt_text 必须与参考音频**逐字一致**，否则模型对不齐 ⇒ 吞字、韵律乱跑、
#     重音位置错（2026-11-24 实测：neutral.wav 的文件名少了"这点我早就知道"一整句，
#     导致合成时句尾"了"被吞、"终于/好久"被顶重音）。
#     这些台词是用 SenseVoice 识别 + 人工修正错字得到的，以音频实际内容为准，不信文件名。
# 情绪 → (参考音频, 该音频的台词)。GPT-SoVITS 靠参考音频同时决定音色和情绪。
#
# ★★★ 2026-11-24 实测结论：素材里 4 条参考音频**只有 light.wav 合格**
#   neutral.wav → 句尾"了"被吞（prompt 修正后仍吞，是音频本身与模型对不齐）
#   happy.wav   → 音调拔高过度，失真到"不像她"（用户原话："有点恐怖"）
#   happy2.wav  → 同上（"对吧对吧？"本身就很跳）
#   light.wav   → 正常：音色准、韵律自然、不吞字 ✅
# 所以四种情绪**统一用 light.wav**，情绪改由 LLM 的文本内容承担（说什么比怎么说更重要）。
# 这也让四段语音音色统一，不会一会儿像同一个人一会儿像另一个。
#
# 想恢复真情感，需要更好的参考音频：从动画里截一段她**日常对话**（平和、无背景音、
# 3~8 秒），替换 ref/ 下对应文件并更新 prompt_text（务必用 ASR 核对真实内容，别信文件名）。
_LIGHT = ("ref/light.wav", "虽然付出了点代价，但我觉得自己在救世主这行干的挺不错的呢。")
SOVITS_REFS = {k: _LIGHT for k in ("开心", "忧伤", "埋怨", "温柔", "中立")}


def _to_44k_mono(audio_bytes, suffix=".wav"):
    """统一转成设备要的格式：44100Hz / 16bit / 单声道，并做响度归一化。
    ★ edge-tts 出 24kHz、GPT-SoVITS 出 32kHz，而设备 I2S 固定按 44.1kHz 播，
      不重采样就会变调（音调/速度都不对）。
    ★★ 响度：SoVITS 合成结果直接跟着**参考音频的音量**走。素材里 light.wav 峰值只有
      -27 dBFS，导致合成出来 -21.5 dB，而 edge 是 -5 dB —— 差 16~20 dB，听着就是
      '声音好小'。用 EBU R128（loudnorm I=-16 LUFS，真峰值 -1.5 dB）统一拉到广播级，
      两种引擎的音量就一致了。"""
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
             # loudnorm 先做（只按真实语音算响度），apad 在末尾补 350ms 静音。
             # ★★ 2026-11-24：设备端播完会切 I2S 时钟回 16k / 关 PA，而 DMA 里
             #   还有几十~两百 ms 音频没放出去 ⇒ 最后一个字被截断（"话没说完"）。
             #   在这里补静音，被切掉的就只是静音，语音本身完整。
             "-af", "loudnorm=I=-16:TP=-1.5:LRA=11,apad=pad_dur=0.35",
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
    # ★ text_split_method 必须用 cut0（整句不切）：
    #   默认的 cut5 按标点切段，段间加 0.3s 间隔 ⇒ 短句听起来一顿一顿，
    #   而且每段独立做韵律预测，重音会落在每个短语上（"终于""好久"被顶出来）。
    #   整句一次生成则韵律连贯。回复本身 ≤40 字，不会超模型上限。
    #   ★ 万一将来放开长度限制，超过 60 字自动退回 cut3（按句号切，不会碎到逗号）。
    split = "cut0" if len(text) <= 60 else "cut3"
    body = {
        "text": text,
        "text_lang": "zh",
        "ref_audio_path": os.path.join(SOVITS_ROOT, ref),
        "prompt_text": prompt,
        "prompt_lang": "zh",
        "media_type": "wav",
        "text_split_method": split,
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


# ---------------------------------------------------------------- 人设检索（RAG）
# 人设分两层，解决"长 prompt 被稀释"的问题：
#   ① tools/persona.txt   → 核心人设（性格 + 输出格式），每次都在，控制在 300 字内
#   ② tools/persona/*.md  → 补充设定（背景、口癖、关系、世界观），**想写几万字都行**
#      每次对话按话题检索，只把最相关的 3 条拼进去 ⇒ 不稀释、不失忆
# 向量化用 LM Studio 里现成的 Qwen3-Embedding-0.6B（本地、零成本、约 50ms）。
PERSONA_DIR   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "persona")
EMBED_URL     = os.environ.get("EMBED_URL", "http://127.0.0.1:1234/v1/embeddings")
EMBED_MODEL   = os.environ.get("EMBED_MODEL", "")      # 空 = 自动探测带 embed 的模型
RETRIEVE_K    = int(os.environ.get("RETRIEVE_K", "3"))
RETRIEVE_MIN  = float(os.environ.get("RETRIEVE_MIN", "0.38"))   # 宁缺毋滥：召回错的设定比不召回更伤

# ★ 露骨内容改用"注释隔离"处理（见 tools/persona 里 <!-- --> 包裹的章节），
#   不再靠关键词过滤——词表会把"亲亲抱抱"这种正常亲昵一起误伤掉（2026-11-24 用户指出）。
#   这里保留开关只作兜底，默认关闭。
SKIP_NSFW = os.environ.get("SKIP_NSFW", "0") == "1"
SENSITIVE_WORDS = (
    "乳房", "下体", "口交", "乳交", "性器", "肉棒", "小穴", "淫水", "射精",
    "奶子", "做爱", "性爱", "裸体", "全裸", "插入", "抽插", "高潮", "淫", "骚",
)

_snippets = []                       # [(单位向量, 原文)]
_embed_cache = {"id": None}


def _pick_embed_model():
    import json
    if _embed_cache["id"]:
        return _embed_cache["id"]
    mid = EMBED_MODEL
    if not mid:
        try:
            with _local_opener().open("http://127.0.0.1:1234/v1/models", timeout=10) as r:
                ms = json.load(r).get("data") or []
            cands = [m["id"] for m in ms if "embed" in m["id"].lower()]
            mid = cands[0] if cands else ""
        except Exception:
            mid = ""
    _embed_cache["id"] = mid
    return mid


def _embed(texts):
    """调 LM Studio /v1/embeddings。失败返回 None（调用方降级处理）。"""
    import json
    import urllib.request

    model = _pick_embed_model()
    if not model:
        return None
    req = urllib.request.Request(
        EMBED_URL,
        data=json.dumps({"model": model, "input": texts},
                        ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json"})
    try:
        with _local_opener().open(req, timeout=60) as r:
            d = json.load(r)
        return [it["embedding"] for it in d["data"]]
    except Exception as e:
        print(f"[rag] embedding 失败: {e}", flush=True)
        return None


def _norm(v):
    import math
    n = math.sqrt(sum(x * x for x in v))
    return [x / n for x in v] if n > 0 else v


def _chunk(text, max_len=180):
    """按行切片，**保留标题作为上下文前缀**，过长再按句号切。

    ★ 先剥掉 HTML 注释块：persona/*.md 里用 <!-- --> 包住了露骨性描写章节，
      这些内容不参与索引（也不影响共读文件），而正常的亲昵/感情设定照常保留。
    ★ 标题不能丢：像 "## 5. 情感表达铁律：坦率，不傲娇" 本身就是一条规则，
      只留正文"我说话不绕弯子"的话，模型（和检索）不知道这属于哪一条。
      所以把最近两级标题拼成前缀：`[情感表达铁律 · 核心原则] 我说话不绕弯子。`"""
    import re as _re

    text = _re.sub(r"<!--.*?-->", "", text, flags=_re.S)   # 剥掉注释块

    out = []
    stack = []                                  # 标题栈（按层级截断）
    for para in text.split("\n"):
        raw = para.strip()
        if not raw or raw.startswith("prompt:"):    # 跳过 YAML 外壳
            continue

        m = _re.match(r"^(#{1,6})\s+(.+)$", raw)
        if m:
            lvl = len(m.group(1))
            stack = stack[: lvl - 1] + [m.group(2).strip()]
            continue

        body = raw.lstrip("-*0123456789.、 ").strip()
        if len(body) < 8:                        # 过短行（纯编号/符号）丢掉
            continue
        prefix = f"[{' · '.join(stack[-2:])}] " if stack else ""

        if len(body) <= max_len:
            out.append(prefix + body)
            continue
        buf = ""
        for seg in body.replace("。", "。|").replace("；", "；|").split("|"):
            if buf and len(buf) + len(seg) > max_len:
                out.append(prefix + buf)
                buf = seg
            else:
                buf += seg
        if buf:
            out.append(prefix + buf)
    return out


ALWAYS_FILE = os.path.join(PERSONA_DIR, "_always.md")
_always_cache = {"text": None}


def _load_always():
    """常驻设定：文件名以 `_` 开头，不切片、不检索，每次对话都直接拼进 system。
    用于"必须 100% 在场"的底色设定（关系、称呼等），避免靠检索碰运气。"""
    if _always_cache["text"] is None:
        try:
            with open(ALWAYS_FILE, encoding="utf-8") as f:
                _always_cache["text"] = f.read().strip()
        except OSError:
            _always_cache["text"] = ""
    return _always_cache["text"]


def build_persona_index():
    """读 tools/persona/*.md → 切片 → 算向量。启动时调一次，失败不影响对话。

    ★ 文件名以 `_` 开头的**不参与索引**，而是作为"常驻设定"直接拼进 system（见
      _load_always）。这样做是因为有些设定必须 100% 在场、不能靠检索碰运气：
      比如"我和你的关系"——问"我是谁"时语义太模糊，检索会命中群聊身份解析那类
      元规则，真正的家人关系反而被挤掉（2026-11-24 实测：回答说"你是朋友"）。"""
    global _snippets
    _snippets = []
    if not os.path.isdir(PERSONA_DIR):
        return 0
    chunks = []
    for fn in sorted(os.listdir(PERSONA_DIR)):
        if not fn.lower().endswith(".md") or fn.startswith("_"):
            continue
        try:
            with open(os.path.join(PERSONA_DIR, fn), encoding="utf-8") as f:
                chunks += _chunk(f.read())
        except OSError:
            continue
    if not chunks:
        return 0

    # 排除露骨片段（文件不动，只是不参与检索）
    dropped = 0
    if SKIP_NSFW:
        kept = [c for c in chunks if not any(w in c for w in SENSITIVE_WORDS)]
        dropped = len(chunks) - len(kept)
        chunks = kept
        if dropped:
            print(f"[rag] 已排除 {dropped} 条露骨片段（SKIP_NSFW=1，可用环境变量关闭）", flush=True)
    if not chunks:
        return 0
    # 分批做 embedding：人设可能有几百条，一次全塞进去请求体过大容易超时
    vecs = []
    for i in range(0, len(chunks), 32):
        part = _embed(chunks[i: i + 32])
        if not part:
            print("[rag] 索引建立失败（embedding 不可用）→ 只用人设全文", flush=True)
            return 0
        vecs += part
    if len(vecs) != len(chunks):
        return 0
    _snippets = [(_norm(v), t) for v, t in zip(vecs, chunks)]
    print(f"[rag] 人设索引就绪：{len(_snippets)} 条（来自 {PERSONA_DIR}）", flush=True)
    return len(_snippets)


def retrieve(query, k=None):
    """返回与 query 最相关的 k 条设定。"""
    if not _snippets:
        return []
    qv = _embed([query])
    if not qv:
        return []
    q = _norm(qv[0])
    scored = [(sum(a * b for a, b in zip(q, v)), t) for v, t in _snippets]
    scored = [x for x in scored if x[0] >= RETRIEVE_MIN]
    scored.sort(key=lambda x: -x[0])
    return [t for _, t in scored[: (k or RETRIEVE_K)]]


# ---------------------------------------------------------------- 对话记忆
# ★ 语音对话必须记住上文，否则"她刚才说什么"这类指代全失效，
#   关系也无法自然推进（用户 2026-11-24：'从家人聊到更亲密' 需要连续上下文）。
#   存在内存里：重启即清空（故意如此——设备每次开机是"重新见面"）。
#   ★ 10 分钟没说话就自动清空，避免昨天的半截话影响今天。
_CHAT_HIST = collections.deque(maxlen=12)      # 12 条 = 6 轮
_LAST_TS = [0.0]
_HIST_IDLE_CLEAR = 600                          # 秒


def _hist_prepare():
    """超过空闲时间就清空历史。"""
    import time as _t
    now = _t.time()
    if now - _LAST_TS[0] > _HIST_IDLE_CLEAR:
        _CHAT_HIST.clear()
    _LAST_TS[0] = now


def _hist_push(user_text, reply):
    _CHAT_HIST.append({"role": "user", "content": user_text})
    _CHAT_HIST.append({"role": "assistant", "content": reply})


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

    # 核心人设 + 常驻底色设定 + 按话题检索出的补充设定
    system = PERSONA
    always = _load_always()
    if always:
        system += "\n\n" + always
    try:
        extra = retrieve(user_text)
    except Exception:
        extra = []
    if extra:
        system += (
            "\n\n【角色设定 —— 描述她\"遇到什么会怎么反应\"，必须照着演】\n"
            + "\n".join(f"- {x}" for x in extra)
            + "\n★ 硬性规定：设定里写她会**回避 / 不说 / 否认 / 忽然安静 / 岔开话题**的，"
              "就必须真的那样做——直接正面回答等于毁掉人设。"
              "和当前话题无关的设定完全忽略，不要硬凑。")

    _hist_prepare()
    body = {
        "model": model,
        "messages": ([{"role": "system", "content": system}]
                     + list(_CHAT_HIST)
                     + [{"role": "user", "content": user_text}]),
        "temperature": 0.8,
        "max_tokens": 120,
        "stream": False,
    }
    hdrs = {"Content-Type": "application/json"}
    if LLM_KEY:
        hdrs["Authorization"] = f"Bearer {LLM_KEY}"     # 云端 API 鉴权
    # ★ 失败自动重试一次：首次 TLS 握手、云端抖动、审核误判都可能一次不成功
    #   （用户实测"第一次对话失败、之后都正常"，这个重试就是为那种情况兜底）
    import time as _time

    d = None
    for attempt in (1, 2):
        req = urllib.request.Request(
            LLM_URL, data=json.dumps(body).encode("utf-8"), headers=hdrs)
        try:
            with _local_opener().open(req, timeout=90) as r:
                d = json.load(r)
            break
        except urllib.error.HTTPError as ex:
            detail = ""
            try:
                detail = ex.read().decode("utf-8", "replace")[:400]
            except Exception:
                pass
            print(f"[chat] 第{attempt}次 HTTP {ex.code}: {detail}", flush=True)
            if ex.code == 400 and len(_CHAT_HIST) > 0:
                # 400 多半是历史里某条触发审核 ⇒ 清历史、只带当前句
                print("[chat] 400 且有历史 → 清空历史后重试", flush=True)
                _CHAT_HIST.clear()
                body["messages"] = [{"role": "system", "content": system},
                                    {"role": "user", "content": user_text}]
            elif attempt == 1:
                _time.sleep(0.6)          # 网络/服务抖动，歇一下再试
            else:
                raise
        except Exception as ex:
            print(f"[chat] 第{attempt}次失败: {type(ex).__name__}", flush=True)
            if attempt == 1:
                _time.sleep(0.6)
            else:
                raise
    if d is None:
        raise RuntimeError("chat failed twice")
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
    _hist_push(user_text, content)      # 记入上下文（存纯台词，不带情绪标签）
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
        # ---------- 人设热重载：改完 tools/persona/*.md 不用重启服务 ----------
        if self.path == "/api/persona/reload":
            try:
                import json
                # ★ 顺带重读 persona.txt（核心人设），这样改人设也不用重启服务
                global PERSONA
                PERSONA = os.environ.get("PERSONA") or _load_persona()
                _always_cache["text"] = None          # 常驻设定也要重读
                n = build_persona_index()
                self._send_bytes(
                    json.dumps({"count": n, "persona_chars": len(PERSONA)}).encode(),
                    "application/json; charset=utf-8")
            except Exception as e:
                self.send_error(500, str(e))
            return

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
    try:
        build_persona_index()
    except Exception as e:
        print(f"[rag] 建索引异常（不影响对话）: {e}", flush=True)
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
