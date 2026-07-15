"""扫描 BLE 设备，列出 VS-* 广播名（用于确认设备实际广播名与地址）。"""
import asyncio
from bleak import BleakScanner


async def scan():
    print("等待 6 秒让设备完成启动广播...")
    await asyncio.sleep(6)
    print("扫描 12 秒...")
    devs = await BleakScanner.discover(timeout=12.0)
    vs = [d for d in devs if d.name and d.name.upper().startswith("VS-")]
    for d in vs:
        print(f"  FOUND: {d.name}  address={d.address}")
    print(f"共扫描到 {len(devs)} 个设备，其中 {len(vs)} 个 VS-*")


if __name__ == "__main__":
    asyncio.run(scan())
