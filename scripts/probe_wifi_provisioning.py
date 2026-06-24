"""voice_net 端到端配网探测脚本。

用法：
    python scripts/probe_wifi_provisioning.py                 # 仅请求一次 wifi_status
    python scripts/probe_wifi_provisioning.py --ssid X --password Y  # 下发凭据 + 等待状态
    python scripts/probe_wifi_provisioning.py --clear         # 清空凭据

依赖：`pip install bleak`。在 Windows 上用 WinRT BLE。

只服务于本地开发调试，不会归档到 PR；后续 Wi-Fi 面板上线后这个脚本可删。
"""

import argparse
import asyncio
import getpass
import json
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100"
STATE_UUID   = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"
CONTROL_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103"


def parse_state_frame(data: bytearray) -> dict | None:
    """state_tx 4 字节头 + JSON。返回解析后的 JSON dict 或 None。"""
    if len(data) < 4 or data[0] != 1 or data[1] != 0x10:
        return None
    payload_len = data[2] | (data[3] << 8)
    if len(data) < 4 + payload_len:
        return None
    try:
        return json.loads(data[4:4 + payload_len].decode("utf-8"))
    except Exception:
        return None


async def find_device(prefix: str, scan_seconds: float = 5.0):
    print(f"[scan] looking for {prefix}-*... ({scan_seconds:.0f}s)", flush=True)
    devices = await BleakScanner.discover(timeout=scan_seconds)
    for d in devices:
        name = d.name or ""
        if name.startswith(prefix + "-"):
            print(f"[scan] hit: {name} @ {d.address}", flush=True)
            return d
    print("[scan] no VS-* device found", flush=True)
    return None


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prefix", default="VS")
    ap.add_argument("--ssid", help="下发 wifi_set 的 SSID")
    ap.add_argument("--password", default="", help="下发 wifi_set 的密码（默认空表示开放网络）")
    ap.add_argument("--password-prompt", action="store_true", help="交互式输入密码（不回显，避免密码出现在命令行历史）")
    ap.add_argument("--clear", action="store_true", help="下发 wifi_clear")
    ap.add_argument("--scan", type=float, default=5.0, help="BLE 扫描时长（秒）")
    ap.add_argument("--listen", type=float, default=35.0, help="订阅 state_tx 监听总时长（秒）")
    args = ap.parse_args()
    if args.password_prompt:
        args.password = getpass.getpass("Wi-Fi password: ")

    dev = await find_device(args.prefix, args.scan)
    if not dev:
        sys.exit(2)

    statuses: list[dict] = []
    last_print_ts = 0.0

    def on_state(_handle, data: bytearray):
        nonlocal last_print_ts
        evt = parse_state_frame(data)
        if not evt:
            return
        if evt.get("event") == "wifi_status":
            statuses.append(evt)
            now = time.time()
            # 控制台节流：同一秒内多次推送只打印一条
            if now - last_print_ts >= 0.05:
                print(f"[wifi_status] {json.dumps(evt, ensure_ascii=False)}", flush=True)
                last_print_ts = now

    async with BleakClient(dev.address) as client:
        print(f"[ble] connected to {dev.address}", flush=True)
        await client.start_notify(STATE_UUID, on_state)
        print(f"[ble] subscribed to state_tx", flush=True)

        if args.clear:
            cmd = json.dumps({"event": "wifi_clear"})
            print(f"[ble] tx control_rx: {cmd}", flush=True)
            await client.write_gatt_char(CONTROL_UUID, cmd.encode("utf-8"), response=False)
        elif args.ssid:
            cmd = json.dumps({"event": "wifi_set", "ssid": args.ssid, "password": args.password})
            # 日志侧脱敏：不打印密码
            print(f"[ble] tx control_rx: wifi_set ssid={args.ssid} password=<redacted>", flush=True)
            await client.write_gatt_char(CONTROL_UUID, cmd.encode("utf-8"), response=False)
        else:
            cmd = json.dumps({"event": "wifi_status_request"})
            print(f"[ble] tx control_rx: {cmd}", flush=True)
            await client.write_gatt_char(CONTROL_UUID, cmd.encode("utf-8"), response=False)

        print(f"[ble] listening {args.listen:.0f}s for wifi_status...", flush=True)
        try:
            await asyncio.sleep(args.listen)
        except KeyboardInterrupt:
            pass

        await client.stop_notify(STATE_UUID)
        print(f"[ble] stop. received {len(statuses)} wifi_status frames", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
