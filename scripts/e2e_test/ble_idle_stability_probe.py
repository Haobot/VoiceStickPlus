# BLE 连接稳定性隔离实验：排除 VoiceStick.exe，用独立 bleak 客户端连接
# VS-091C 并静置 200 秒，记录连接/通知/断连时间点。
# 若 ~60 秒被杀 -> OS/驱动层问题；若稳定 -> VoiceStick.exe 会话管理问题。
import asyncio
import time

from bleak import BleakScanner, BleakClient

STATE_TX_UUID = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"


def ts() -> str:
    return f"{time.monotonic() - T0:7.2f}s"


T0 = time.monotonic()


async def main() -> None:
    print(f"{ts()} scanning for VS-091C ...", flush=True)
    device = await BleakScanner.find_device_by_name("VS-091C", timeout=30)
    if device is None:
        print(f"{ts()} device not found", flush=True)
        return
    print(f"{ts()} found: {device.name} [{device.address}]", flush=True)

    count = 0

    def on_notify(_sender, data: bytearray) -> None:
        nonlocal count
        count += 1
        if count % 20 == 1:
            print(f"{ts()} notify #{count} len={len(data)}", flush=True)

    async with BleakClient(device, disconnected_callback=lambda _: print(f"{ts()} !!! DISCONNECTED callback", flush=True)) as client:
        print(f"{ts()} connected", flush=True)
        await client.start_notify(STATE_TX_UUID, on_notify)
        print(f"{ts()} subscribed; idling 200s ...", flush=True)
        for i in range(40):
            await asyncio.sleep(5)
            if not client.is_connected:
                print(f"{ts()} !!! is_connected=False at check {i}", flush=True)
                break
        else:
            print(f"{ts()} SURVIVED 200s, total notify={count}", flush=True)
        print(f"{ts()} test end, is_connected={client.is_connected}", flush=True)


asyncio.run(main())
