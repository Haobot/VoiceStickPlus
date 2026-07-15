"""L3 固件回放真机验证脚本（bleak 独立 BLE 连接）。

独立连接 StickS3 设备（VoiceStickApp 必须断开，BLE 独占 MAX_CONNECTIONS=1），
下发 test_playback 设回放 PCM 文件 + remote_button_down/up 驱动录音，订阅 audio_tx 收
Opus 帧，统计帧数/字节/首帧延迟并保存裸 Opus 包。配合固件串口日志 "playback set" 确认
回放生效。

用法：
  python run_l3_firmware.py --device-id 5A74 --file short_01.pcm --duration 5

验证通过判据：收到 audio_tx 帧数 > 0；固件串口日志含 "playback set: /spiffs/<file>"。
完整 PCM 内容比对（Ogg 封装+解码+相关性）作为后续增强。
"""
import argparse
import asyncio
import json
import struct
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

# BLE GATT UUID（协议 Doc/Ref/protocol.md）
AUDIO_TX = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101"
STATE_TX = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"
CONTROL_RX = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103"

# AudioBleFrame 头 16 字节：version(u8) type(u8) header_len(u16) session_id(u32)
# seq(u32) flags(u8) reserved(u8) payload_len(u16)
AUDIO_HEADER_FMT = "<BBHIIBBH"
AUDIO_HEADER_LEN = 16
AUDIO_TYPE = 0x01
FLAG_START = 0x01
FLAG_END = 0x02


class AudioCollector:
    def __init__(self):
        self.frames = []  # (session_id, seq, flags, payload)
        self.first_frame_time = None
        self.start_time = None

    def on_audio(self, _sender, data: bytearray):
        if len(data) < AUDIO_HEADER_LEN:
            return
        version, ftype, header_len, session_id, seq, flags, _reserved, payload_len = \
            struct.unpack(AUDIO_HEADER_FMT, bytes(data[:AUDIO_HEADER_LEN]))
        if ftype != AUDIO_TYPE:
            return
        payload = bytes(data[AUDIO_HEADER_LEN:AUDIO_HEADER_LEN + payload_len])
        self.frames.append((session_id, seq, flags, payload))
        if self.first_frame_time is None:
            self.first_frame_time = asyncio.get_event_loop().time()

    def on_state(self, _sender, data: bytearray):
        if len(data) < 4:
            return
        _version, ftype, payload_len = struct.unpack("<BBH", bytes(data[:4]))
        if ftype == 0x10 and payload_len > 0:
            try:
                evt = json.loads(bytes(data[4:4 + payload_len]).decode("utf-8"))
                print(f"  state: {evt.get('event')} {evt.get('button', '')} sid={evt.get('session_id', '')}")
            except Exception:  # noqa: BLE001
                pass


async def run(args: argparse.Namespace) -> int:
    name = f"VS-{args.device_id.upper()}"
    print(f"扫描设备 {name}（确保 VoiceStickApp 已断开）...")
    device = await BleakScanner.find_device_by_name(name, timeout=15.0)
    if device is None:
        print(f"FAIL: 未找到 {name}，确认设备已开机广播且 VoiceStickApp 已断开", file=sys.stderr)
        return 1

    collector = AudioCollector()
    async with BleakClient(device, timeout=20.0) as client:
        print(f"已连接 {device.name}")
        await client.start_notify(AUDIO_TX, collector.on_audio)
        await client.start_notify(STATE_TX, collector.on_state)

        # 1. 设回放文件
        cmd = json.dumps({"event": "test_playback", "file": args.file}).encode("utf-8")
        await client.write_gatt_char(CONTROL_RX, cmd, response=False)
        print(f"已下发 test_playback file={args.file}")

        await asyncio.sleep(0.3)

        # 2. 开始录音（REMOTE 源，跳过 hold 阈值）
        collector.start_time = asyncio.get_event_loop().time()
        cmd = json.dumps({"event": "remote_button_down", "button": "primary", "request_id": 1}).encode("utf-8")
        await client.write_gatt_char(CONTROL_RX, cmd, response=False)
        print("已下发 remote_button_down")

        # 3. 收帧 duration 秒
        await asyncio.sleep(args.duration)

        # 4. 停止录音
        cmd = json.dumps({"event": "remote_button_up", "button": "primary", "request_id": 1}).encode("utf-8")
        await client.write_gatt_char(CONTROL_RX, cmd, response=False)
        print("已下发 remote_button_up")

        # 等 drain 帧 + end 帧
        await asyncio.sleep(1.0)
        await client.stop_notify(AUDIO_TX)
        await client.stop_notify(STATE_TX)

    # 统计
    n = len(collector.frames)
    total_bytes = sum(len(f[3]) for f in collector.frames)
    end_frames = sum(1 for f in collector.frames if f[2] & FLAG_END)
    print(f"\n收到 {n} 个音频帧，payload 共 {total_bytes} 字节，end 帧 {end_frames}")
    if collector.first_frame_time and collector.start_time:
        print(f"首帧延迟: {(collector.first_frame_time - collector.start_time) * 1000:.0f} ms")

    if n == 0:
        print("FAIL: 未收到任何音频帧（检查固件是否回放、BLE 是否就绪）", file=sys.stderr)
        return 1

    # 保存裸 Opus 包序列（供后续 Ogg 封装+解码比对）
    out = Path(args.out)
    with open(out, "wb") as f:
        for _sid, _seq, _flags, payload in collector.frames:
            if payload:  # 跳过 end 空帧
                f.write(payload)
    print(f"已保存 {sum(1 for f in collector.frames if f[3])} 个 Opus 包到 {out}")
    print("OK: 固件回放链路工作（请核对串口日志含 'playback set' 确认回放源）")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="L3 firmware playback verification via BLE.")
    ap.add_argument("--device-id", required=True, help="设备 ID（VS-XXXX 的 XXXX，如 5A74）")
    ap.add_argument("--file", required=True, help="回放 PCM 文件名（SPIFFS 内，如 short_01.pcm）")
    ap.add_argument("--duration", type=float, default=5.0, help="录音时长（秒）")
    ap.add_argument("--out", default="l3_captured_opus.bin", help="保存裸 Opus 包路径")
    args = ap.parse_args()
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
