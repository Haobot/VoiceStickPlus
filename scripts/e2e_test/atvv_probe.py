# 小米蓝牙遥控器 2 Pro ATVV L3 真机探针（bleak 独立连接）。
# 量测链路时延与稳定性：GET_CAPS→CAPS 延迟、MIC_OPEN→首音频帧延迟、
# 会话帧数/字节数、STOP→尾包时延分布、长连接静置稳定性
# （静置计时模式参考 ble_idle_stability_probe.py）。
#
# 与 VoiceStick.exe 互斥（BLE 单连接）：检测到其在运行会提示先退出。
#
# 用法：
#   python scripts/e2e_test/atvv_probe.py [--address ADDR | --name NAME]
#       [--sessions 2] [--idle 30] [--duration 120] [--out report.json]
#
# 退出码：0 正常完成；2 设备未找到/连接失败/ VoiceStick.exe 占用；3 无 ATVV 服务。
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from atvv_capture import (ATVV_CONTROL_UUID, ATVV_AUDIO_UUID, ATVV_SERVICE_UUID,  # noqa: E402
                          ATVV_TX_UUID, CMD_GET_CAPS, CMD_MIC_CLOSE,
                          CMD_MIC_OPEN_ACK, NAME_WHITELIST, OP_AUDIO_SYNC,
                          OP_CAPS, OP_MIC_OPEN, OP_STOP, OP_STREAM_START,
                          SCAN_TIMEOUT_S, parse_caps, ts)  # noqa: E402


def voicestick_running() -> bool:
    """检测 VoiceStick.exe 是否在运行（它会独占 BLE 连接）。"""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq VoiceStick.exe", "/NH"],
            capture_output=True, text=True, timeout=10).stdout
        return "VoiceStick.exe" in out
    except Exception:  # noqa: BLE001 - 检测失败不阻塞主流程
        return False


class AtvvProbe:
    def __init__(self, args) -> None:
        self.args = args
        self.client = None
        self.connected = False
        self.caps = None
        self.version = 0

        self.get_caps_ts = None
        self.caps_ts = None
        self.mic_open_ts = None
        self.ack_written_ts = None
        self.stream_start_ts = None
        self.first_audio_ts = None
        self.stop_ts = None
        self.tail_deltas_ms = []     # 当前会话 STOP 后各音频包相对 STOP 的延迟
        self.frame_bytes = 0
        self.frame_count = 0
        self.session_open = False

        self.sessions = []
        self.button_sessions = 0     # 实际按键会话数（STREAM_START 起算）
        self.stream_session_id = None  # STREAM_START 记录的遥控器 session id
        self.disconnects = []        # 静置期断连时刻
        self.idle_checks = 0
        self.idle_ok = True

    # ---- notify 回调（只做时间戳与计数，见 atvv_capture 的线程契约注释） ----
    def on_audio(self, _sender, data: bytearray) -> None:
        now = time.monotonic()
        if self.session_open and self.first_audio_ts is None:
            self.first_audio_ts = now
        if self.stop_ts is not None:
            self.tail_deltas_ms.append(round((now - self.stop_ts) * 1000.0, 1))
        self.frame_bytes += len(data)
        self.frame_count += 1

    def on_control(self, _sender, data: bytearray) -> None:
        data = bytes(data)
        if not data:
            return
        now = time.monotonic()
        op = data[0]
        if op == OP_CAPS:
            try:
                self.caps = parse_caps(data)
                self.version = self.caps["version"]
                self.caps_ts = now
                print(f"{ts()} CAPS: ver=0x{self.version:04x} "
                      f"codecs=0x{self.caps['codecs']:02x} "
                      f"frame_len={self.caps['frame_len']}", flush=True)
            except ValueError as exc:
                print(f"{ts()} WARN bad CAPS: {exc}", flush=True)
        elif op == OP_MIC_OPEN:
            self._close_session_if_open()
            self.mic_open_ts = now
            self.first_audio_ts = None
            self.stream_start_ts = None
            self.stop_ts = None
            self.tail_deltas_ms = []
            self.frame_bytes = 0
            self.frame_count = 0
            print(f"{ts()} MIC_OPEN", flush=True)
            if self.version >= 0x0100:
                payload = bytes([CMD_MIC_OPEN_ACK, 0x00])
            else:
                payload = bytes([CMD_MIC_OPEN_ACK, 0x00, 0x02])
            asyncio.ensure_future(self._tx(payload, mark_ack=True))
        elif op == OP_STREAM_START:
            self.stream_start_ts = now
            self.session_open = True
            self.button_sessions += 1  # 实际按键会话数（--sessions 以此计数）
            # 与 atvv_capture 一致：STREAM_START 第 4 字节是遥控器 session id
            self.stream_session_id = data[3] if len(data) > 3 else 0
            print(f"{ts()} STREAM_START session={self.stream_session_id}",
                  flush=True)
        elif op == OP_AUDIO_SYNC:
            print(f"{ts()} AUDIO_SYNC: {data.hex()}", flush=True)
        elif op == OP_STOP:
            self.stop_ts = now
            self.session_open = False
            print(f"{ts()} STOP", flush=True)
            # STOP 即结算该会话指标（对齐 _close_session_if_open docstring）；
            # STOP 后到达的尾包不再计入 frames/bytes，只经 tail_deltas_ms 记录
            self._close_session_if_open()

    def on_disconnect(self, _client) -> None:
        self.connected = False
        self.disconnects.append(round(time.monotonic(), 3))
        print(f"{ts()} !!! disconnected", flush=True)

    async def _tx(self, payload: bytes, mark_ack: bool = False) -> None:
        try:
            await self.client.write_gatt_char(ATVV_TX_UUID, payload, response=False)
            if mark_ack:
                self.ack_written_ts = time.monotonic()
        except Exception as exc:
            print(f"{ts()} WARN TX write failed: {exc}", flush=True)

    def _close_session_if_open(self) -> None:
        """STOP 或新 MIC_OPEN 时结算上一会话统计（tail_deltas_ms 共享引用，
        结算后 STOP 尾包仍继续累积进该条目）。"""
        if self.mic_open_ts is None:
            return
        entry = {
            "mic_open_to_first_audio_ms": (
                round((self.first_audio_ts - self.mic_open_ts) * 1000.0, 1)
                if self.first_audio_ts is not None else None),
            "mic_open_to_ack_written_ms": (
                round((self.ack_written_ts - self.mic_open_ts) * 1000.0, 1)
                if self.ack_written_ts is not None else None),
            "mic_open_to_stream_start_ms": (
                round((self.stream_start_ts - self.mic_open_ts) * 1000.0, 1)
                if self.stream_start_ts is not None else None),
            "frames": self.frame_count,
            "bytes": self.frame_bytes,
            "tail_deltas_ms": self.tail_deltas_ms,  # 共享引用：尾包继续累积
        }
        if entry["frames"] > 0 or self.first_audio_ts is not None:
            self.sessions.append(entry)
            print(f"{ts()} session closed: frames={entry['frames']} "
                  f"bytes={entry['bytes']} "
                  f"first_audio={entry['mic_open_to_first_audio_ms']}ms "
                  f"tail={entry['tail_deltas_ms']}", flush=True)
        self.mic_open_ts = None

    # ---- 主流程 ----
    async def run(self) -> int:
        from bleak import BleakClient

        device = await self._find_device()
        if device is None:
            return 2
        print(f"{ts()} connecting {device.name} [{device.address}] ...", flush=True)
        self.client = BleakClient(device, disconnected_callback=self.on_disconnect)
        try:
            await self.client.connect()
        except Exception as exc:
            print(f"{ts()} connect failed: {exc}", flush=True)
            print("提示：设备需先在 Windows 系统蓝牙设置里完成配对（Bond）。",
                  flush=True)
            return 2
        self.connected = True
        print(f"{ts()} connected", flush=True)

        service = self.client.services.get_service(ATVV_SERVICE_UUID)
        if service is None:
            print(f"{ts()} ATVV service not found（未配对或非目标设备）", flush=True)
            await self.client.disconnect()
            return 3
        await self.client.start_notify(ATVV_CONTROL_UUID, self.on_control)
        await self.client.start_notify(ATVV_AUDIO_UUID, self.on_audio)
        self.get_caps_ts = time.monotonic()
        await self._tx(CMD_GET_CAPS)
        print(f"{ts()} subscribed, GET_CAPS sent; press voice key "
              f"{self.args.sessions} time(s) ...", flush=True)

        deadline = time.monotonic() + self.args.duration
        while self.connected:
            await asyncio.sleep(0.1)
            # 按实际按键会话数（STREAM_START 起算）且当前会话已 STOP 结算后退出；
            # 旧口径 len(self.sessions) 在结算滞后会 off-by-one 等到 duration 上限
            if (self.button_sessions >= self.args.sessions
                    and not self.session_open):
                break
            if time.monotonic() >= deadline:
                print(f"{ts()} duration limit reached", flush=True)
                break

        # 静置稳定性阶段
        if self.connected and self.args.idle > 0:
            print(f"{ts()} idling {self.args.idle}s ...", flush=True)
            idle_end = time.monotonic() + self.args.idle
            while time.monotonic() < idle_end and self.connected:
                await asyncio.sleep(min(5.0, idle_end - time.monotonic()))
                self.idle_checks += 1
                if not self.client.is_connected:
                    self.idle_ok = False
                    break
            if not self.connected:
                self.idle_ok = False

        self._close_session_if_open()
        if self.connected:
            # 用 STREAM_START 记录的遥控器 session id 收尾；无记录（未按过键）
            # 时回落 0（与协议缺省一致）
            sid = self.stream_session_id if self.stream_session_id is not None else 0
            await self._tx(bytes([CMD_MIC_CLOSE, sid & 0xFF])
                           if self.version >= 0x0100 else bytes([CMD_MIC_CLOSE]))
            await self.client.disconnect()
        return 0

    async def _find_device(self):
        from bleak import BleakScanner

        if self.args.address:
            from bleak import BLEDevice
            return BLEDevice(self.args.address, self.args.name or None, {}, -60)
        if self.args.name:
            return await BleakScanner.find_device_by_name(
                self.args.name, timeout=SCAN_TIMEOUT_S)
        print(f"{ts()} scanning whitelist ...", flush=True)
        try:
            advs = await BleakScanner.discover(timeout=SCAN_TIMEOUT_S,
                                               return_adv=True)
            for dev, adv in advs.values():
                name = (dev.name or adv.local_name or "").strip().lower()
                uuids = [u.lower() for u in (adv.service_uuids or [])]
                if name in NAME_WHITELIST or ATVV_SERVICE_UUID.lower() in uuids:
                    return dev
        except TypeError:
            for dev in await BleakScanner.discover(timeout=SCAN_TIMEOUT_S):
                if (dev.name or "").strip().lower() in NAME_WHITELIST:
                    return dev
        print(f"{ts()} no whitelisted device found", flush=True)
        return None

    def report(self) -> dict:
        caps_latency_ms = None
        if self.caps_ts is not None and self.get_caps_ts is not None:
            caps_latency_ms = round((self.caps_ts - self.get_caps_ts) * 1000.0, 1)
        tail_all = [d for s in self.sessions for d in s["tail_deltas_ms"]]
        return {
            "device": self.args.address or self.args.name or "whitelist",
            "caps": self.caps,
            "get_caps_to_caps_ms": caps_latency_ms,
            "sessions": self.sessions,
            "tail_delta_stats_ms": {
                "count": len(tail_all),
                "max": max(tail_all) if tail_all else None,
                "mean": (round(sum(tail_all) / len(tail_all), 1)
                         if tail_all else None),
            },
            "idle": {"seconds": self.args.idle, "checks": self.idle_checks,
                     "ok": self.idle_ok},
            "disconnects": self.disconnects,
        }


async def amain(args) -> int:
    if voicestick_running():
        print("检测到 VoiceStick.exe 正在运行：BLE 单连接互斥，"
              "请先退出 VoiceStick.exe 再运行本探针。", flush=True)
        return 2
    probe = AtvvProbe(args)
    rc = await probe.run()
    rep = probe.report()
    text = json.dumps(rep, ensure_ascii=False, indent=2)
    print("---- probe report ----")
    print(text)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"report written: {args.out}")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(
        description="小米遥控器 ATVV L3 真机探针：时延量测 + 静置稳定性")
    ap.add_argument("--address", help="直接连接指定 MAC")
    ap.add_argument("--name", help="按设备名连接")
    ap.add_argument("--sessions", type=int, default=2,
                    help="采集 N 次按键会话后进入静置阶段（默认 2）")
    ap.add_argument("--idle", type=float, default=30.0,
                    help="静置稳定性时长秒数（默认 30，0 跳过）")
    ap.add_argument("--duration", type=float, default=120.0,
                    help="会话采集阶段总时长上限秒数（默认 120）")
    ap.add_argument("--out", default=None, help="JSON 报告输出路径")
    args = ap.parse_args()
    try:
        return asyncio.run(amain(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
