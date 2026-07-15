"""L4 微信输入法真机验证（半自动）。

编排 loopback_capture 自动抓取 CABLE Output PCM + 人工按设备按键说语料 +
人工确认微信识别结果。验证 wechat 路径：设备录音 -> VoiceStickApp 解码渲染 ->
CABLE -> 微信输入法识别。

BLE 单连接约束：VoiceStickApp 连设备时 L3 bleak 不能连，故用人工按键说话
替代 L3 自动回放（L3 已单独验证固件回放链路）。

前置（用户需自行确认）：
  1. config.toml [output] target = wechat_input_method（当前可能 focused_app，
     需改后重启 VoiceStickApp）
  2. VoiceStickApp 已启动并连接设备（屏幕 Ready）
  3. 微信输入法语音快捷键 = ctrl+win（与 config hotkey 一致）
  4. CABLE 已装；系统默认录音设备 = CABLE Output（或 config 开 auto_switch）

用法：python run_l4_wechat.py [--duration 8] [--phrase "今天天气不错"]
"""
import argparse
import os
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=float, default=8.0, help="抓取时长（秒，含按键说话）")
    ap.add_argument("--phrase", default="今天天气不错", help="测试语料（人工朗读）")
    ap.add_argument("--out", default="l4_cable.wav", help="loopback 输出 wav")
    args = ap.parse_args()

    print("=== L4 微信输入法真机验证（半自动）===")
    print("前置：config wechat / VoiceStickApp 连设备 / 微信 ctrl+win / CABLE Output")
    print(f"\n5 秒后启动 loopback 抓取 {args.duration}s，请准备长按设备主键说语料...")
    for i in range(5, 0, -1):
        print(f"  {i}...")
        time.sleep(1)

    print(f"\n启动 loopback 抓取 CABLE Output {args.duration}s ...")
    loopback = os.path.join(os.path.dirname(os.path.abspath(__file__)), "loopback_capture.py")
    proc = subprocess.Popen(
        [sys.executable, loopback,
         "--duration", str(args.duration), "--out", args.out],
    )

    print(f">>> {args.duration}s 抓取窗口已开始 <<<")
    print(f">>> 现在长按设备主键（或 Ctrl+Win）说「{args.phrase}」，说完松开 <<<")
    print(">>> 同时观察微信输入法候选框是否出现该文字 <<<")

    proc.wait()
    rc = proc.returncode

    print("\n--- L4 结果 ---")
    if rc == 0:
        print("音频层断言通过：CABLE Output 收到非静音 PCM（能量/连续性达标）。")
        print(f"请人工确认：微信输入法候选框是否出现「{args.phrase}」？")
        print("  - 是 -> wechat 端到端链路通")
        print("  - 否 -> 检查微信输入法设置/热键/CABLE 连接（音频层已验证）")
        return 0
    else:
        print(f"音频层断言失败（exit={rc}）：CABLE Output 未收到有效音频。")
        print("排查：VoiceStickApp 是否 wechat 模式渲染？设备是否连接受录音？CABLE 连接？")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
