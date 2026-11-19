"""
MP3 -> WAV（本板专用转换器）

★ 关键约束（血教训）：**绝不做线性插值重采样**。
   44.1kHz 源直接直通（零重采样，音质无损）；
   其他采样率一律拒绝并提示 —— 因为线性插值会把噪声底抬高 15~33dB（实测数据），
   表现为播放时持续"雪花嘶嘶"声。

输出：同目录、同名 .wav，单声道 16bit 44.1kHz（固件 AUD_SAMPLE_RATE 一致）。
"""
import os
import sys
import wave
import array

import miniaudio

TARGET_SR = 44100


def convert_one(path):
    try:
        d = miniaudio.mp3_read_file_s16(path)
    except Exception as e:
        print(f"[失败] {os.path.basename(path)}: 解码错误 {e}")
        return False

    sr, nch = d.sample_rate, d.nchannels
    pcm = d.samples
    dur = d.duration
    kb = len(pcm) * 2 / 1024

    print(f"\n输入: {os.path.basename(path)}")
    print(f"      {sr} Hz, {nch} ch, {dur:.1f}s")

    if sr != TARGET_SR:
        print(f"[拒绝] 采样率 {sr} Hz ≠ {TARGET_SR} Hz")
        print("       不能做线性插值（会把噪声抬高 15~33dB，播放时是持续嘶嘶声）")
        print("       请先用 foobar2000 / Audacity / ffmpeg 做高质量重采样到 44.1kHz：")
        print(f"       ffmpeg -i \"{os.path.basename(path)}\" -ar 44100 -ac 1 out.wav")
        return False

    # 混单声道
    total = len(pcm) // nch
    mono = array.array("h", [0]) * total
    if nch == 1:
        mono = pcm
    else:
        for i in range(total):
            base = i * nch
            mono[i] = sum(pcm[base:base + nch]) // nch

    out = os.path.splitext(path)[0] + ".wav"
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(TARGET_SR)
        w.writeframes(mono.tobytes())

    print(f"[完成] {os.path.basename(out)}  {os.path.getsize(out)/1024/1024:.2f} MB")
    print(f"       拷到 SD 卡根目录即可（建议改英文名，如 TEST.WAV）")
    return True


def main():
    files = sys.argv[1:]
    if not files:
        print("用法: 把 MP3 文件拖到 mp3转wav.bat 上")
        return 1

    ok = fail = 0
    for f in files:
        if convert_one(f):
            ok += 1
        else:
            fail += 1

    print(f"\n===== 成功 {ok} 个，失败 {fail} 个 =====")
    return 0 if fail == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
