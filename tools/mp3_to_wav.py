# -*- coding: utf-8 -*-
# MP3 -> WAV (44.1kHz 16bit mono) for ESP32 playback
# ★★ 教训（11-16）：绝不要用线性插值重采样！实测它把噪声底抬高 15~33dB
#    （ENTERTN 原生 -101dBc → 线性插值后 -68dBc，听感就是持续"嘶嘶"）。
#    现在目标采样率 = 源采样率（MP3 基本都是 44.1k），固件 I2S 也跑 44.1k，
#    全程零重采样。若将来遇到 48k 音源，必须用带抗混叠滤波的重采样器。
import sys, os
import miniaudio

SRC = r"C:/Users/Admin/Desktop/sample_project/tools/music_src"
DST = r"C:/Users/Admin/Desktop/sample_project/tools/music_wav"
os.makedirs(DST, exist_ok=True)

TARGET_SR = 44100

def convert(name_in, name_out, max_seconds=None, start_seconds=0.0):
    path = os.path.join(SRC, name_in)
    d = miniaudio.mp3_read_file_s16(path)
    samples, sr, nch, dur = d.samples, d.sample_rate, d.nchannels, d.duration
    print(f"{name_in}: sr={sr} ch={nch} dur={dur:.1f}s frames={len(samples)//nch}")
    pcm = samples
    total_frames = len(pcm) // nch
    # mono mixdown
    mono = []
    for i in range(0, total_frames * nch, nch):
        mono.append(sum(pcm[i:i+nch]) // nch)
    # 只在源采样率 != 目标时重采样（本工程源=44.1k=目标 ⇒ 恒跳过）
    if sr != TARGET_SR:
        raise SystemExit(f"源 {sr}Hz 与目标 {TARGET_SR}Hz 不同：禁止线性插值，请改用抗混叠重采样")
    if start_seconds:
        mono = mono[int(start_seconds * TARGET_SR):]
    if max_seconds:
        mono = mono[:int(max_seconds * TARGET_SR)]
    # write wav
    import wave
    wpath = os.path.join(DST, name_out)
    with wave.open(wpath, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(TARGET_SR)
        w.writeframes(b"".join(
            v.to_bytes(2, "little", signed=True) for v in mono))
    dur = len(mono) / TARGET_SR
    print(f"  -> {name_out}: {os.path.getsize(wpath)/1024:.0f} KB, {dur:.1f}s @{TARGET_SR} mono16")

# 用户提供的高码率音源（320kbps）：取中间 115 秒（10.14MB，10MB 分区留 780KB 余量）
convert("M800002P3Rch3lsiaQ.mp3", "newsong.wav", max_seconds=115, start_seconds=72.35)
convert("blue_danube.mp3", "blue_danube.wav")
print("DONE")
