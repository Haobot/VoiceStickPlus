"""L4 WASAPI 抓取 CABLE Output PCM（微信输入法音频源）。

微信输入法模式：VoiceStickApp 渲染 16kHz PCM 到 CABLE Input（渲染端），CABLE
Output（采集端）输出 48kHz PCM 供微信输入法采集。本工具抓 CABLE Output PCM，
断言音频层正确（非静音、能量、连续性），供 L4 真机验证。

用法：python loopback_capture.py --duration 5 --out l4_cable.wav
需与 VoiceStickApp wechat 模式 + L3 固件回放同时运行（app 渲染 -> CABLE ->
本工具抓取）。
"""
import argparse
import wave

import numpy as np
import sounddevice as sd


def find_cable_output_wasapi():
    """找 Windows WASAPI 的 CABLE Output 设备（微信输入法采集源）。

    CABLE Output 在 MME/DirectSound/WASAPI/WDM-KS 多个 host API 下重复枚举，
    微信输入法用 WASAPI，故只匹配 Windows WASAPI 条目（48kHz 2ch）。
    """
    devs = sd.query_devices()
    hostapis = sd.query_hostapis()
    for i, d in enumerate(devs):
        if "CABLE Output" in d["name"] and "Virtual Cable" in d["name"]:
            if hostapis[d["hostapi"]]["name"] == "Windows WASAPI":
                return i
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=5.0, help="抓取时长（秒）")
    ap.add_argument("--out", default="l4_cable.wav", help="输出 wav 路径")
    args = ap.parse_args()

    dev = find_cable_output_wasapi()
    if dev is None:
        print("FAIL: 未找到 Windows WASAPI CABLE Output 设备")
        return 1
    info = sd.query_devices(dev)
    sr = int(info["default_samplerate"])
    channels = min(info["max_input_channels"], 2)
    print(f"抓取 CABLE Output dev={dev} sr={sr} ch={channels} duration={args.duration}s")

    data = sd.rec(int(args.duration * sr), samplerate=sr, channels=channels,
                  device=dev, dtype="int16")
    sd.wait()

    with wave.open(args.out, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        wf.writeframes(data.tobytes())

    # 分析：下混 mono 后断言非静音 + 能量 + 连续性。
    mono = data[:, 0] if channels > 1 else data.flatten()
    abs_mono = np.abs(mono.astype(np.int64))
    peak = int(abs_mono.max())
    energy = float(abs_mono.mean())
    nonzero_ratio = float(np.mean(abs_mono > 500))  # 非静音样本比例（|sample|>500）
    # 连续性：静音段（连续 |sample|<500 的样本）最长占比，过高说明断流。
    silent = (abs_mono <= 500).astype(np.int8)
    max_silent_run = 0
    cur = 0
    for s in silent:
        cur = cur + 1 if s else 0
        if cur > max_silent_run:
            max_silent_run = cur
    max_silent_ratio = max_silent_run / len(mono) if len(mono) else 1.0
    print(f"peak={peak} energy={energy:.1f} nonzero_ratio={nonzero_ratio:.3f} "
          f"max_silent_ratio={max_silent_ratio:.3f}")
    print(f"已保存 {args.out}（{len(mono)} 样本 = {len(mono) / sr:.2f}s）")

    ok = True
    if peak < 1000:
        print(f"FAIL: CABLE PCM 静音（peak={peak} < 1000），检查 VoiceStickApp 是否在 wechat 模式渲染")
        ok = False
    # 微信模式 CABLE 电平较低（见 low-level-asr-masks-capture-gain-issue：peak 高但平均能量低，
    # 微信靠服务端 AGC 拉起识别），且短句说话段占比小。故 nonzero_ratio/max_silent_ratio 仅在
    # 极端情况断言，主判据是 peak 有音频信号。
    if nonzero_ratio < 0.005:
        print(f"FAIL: 非静音样本比例极低（{nonzero_ratio:.4f} < 0.005），几乎无音频信号")
        ok = False
    if max_silent_ratio > 0.95:
        print(f"FAIL: 最长静音段占比过高（{max_silent_ratio:.3f} > 0.95），疑似断流")
        ok = False
    if ok:
        print(f"OK: CABLE 音频层断言通过（peak={peak} 有音频信号；微信识别请人工确认）")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
