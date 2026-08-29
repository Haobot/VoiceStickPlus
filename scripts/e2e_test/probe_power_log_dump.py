#!/usr/bin/env python3
"""power_log dump 行为诊断脚本（临时排查工具）。

场景：桌面端监测窗口探测（offset 超界）总能收到空 eof 片，
但随后的增量 dump（offset==total）无任何响应。本脚本直连设备复现：
    1. probe:    dump offset=1000000000          -> 预期空 eof 片 (offset==total)
    2. dump_eq:  dump offset=<total>             -> 复现桌面端无响应？
    3. dump_lt:  dump offset=<total-12>          -> 读最后一条是否正常
    4. dump_gt:  dump offset=<total-60> max=160  -> 正常读多片
仅做诊断，不 clear、不写 anchor，不影响设备数据。
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100"
STATE_TX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"
CONTROL_RX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103"
DEVICE_NAME = "VS-091C"


def dump_payload(offset: int, max_bytes: int) -> bytes:
    return ( '{"power_log":{"cmd":"dump","offset":%d,"max":%d}}' % (offset, max_bytes) ).encode()


async def main() -> int:
    print("scanning for", DEVICE_NAME, "...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").startswith("VS-")
        or SERVICE_UUID in (ad.service_uuids or []),
        timeout=15.0)
    if device is None:
        print("ERROR: device not found (awake? advertising?)")
        return 1
    print("found:", device.address, device.name)

    frames: list[str] = []

    def on_notify(_handle, data: bytearray):
        text = bytes(data).decode("utf-8", errors="replace")
        frames.append(text)
        print("  <-", text)

    async with BleakClient(device, timeout=15.0) as client:
        print("connected, mtu =", client.mtu_size)
        await client.start_notify(STATE_TX_UUID, on_notify)

        total = 0
        steps = [("probe", dump_payload(1000000000, 160))]
        for name, payload in steps:
            frames.clear()
            print(f"[{name}] ->", payload.decode())
            await client.write_gatt_char(CONTROL_RX_UUID, payload, response=False)
            await asyncio.sleep(2.5)
            if not frames:
                print(f"[{name}] NO RESPONSE")
            else:
                # 从 probe 响应里取 total
                import json
                for f in frames:
                    try:
                        obj = json.loads(f)
                        if "power_log" in obj:
                            total = obj["power_log"].get("total", 0)
                    except Exception:
                        pass

        if total <= 0:
            print("cannot determine total; aborting further steps")
            return 1
        print("total =", total)

        followups = [
            ("dump_eq_total", dump_payload(total, 160)),
            ("dump_lt_12", dump_payload(max(0, total - 12), 160)),
            ("dump_lt_60", dump_payload(max(0, total - 60), 160)),
        ]
        for name, payload in followups:
            frames.clear()
            print(f"[{name}] ->", payload.decode())
            await client.write_gatt_char(CONTROL_RX_UUID, payload, response=False)
            await asyncio.sleep(2.5)
            if not frames:
                print(f"[{name}] NO RESPONSE")

        await client.stop_notify(STATE_TX_UUID)
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
