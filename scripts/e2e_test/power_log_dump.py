"""power_log 功耗日志 BLE 导出脚本（bleak 独立 BLE 连接）。

独立连接 StickS3 设备（VoiceStickApp 必须断开，BLE 独占单连接），
通过 control_rx 下发 power_log 命令、订阅 state_tx 收分片响应，
重组二进制日志流并解析为 CSV。协议约定见 Doc/Plan/power-mode-energy-profiling.md
与 Doc/Ref/protocol.md。

流程：连接设备 → 下发 time_anchor（当前 epoch 秒）→ 下发一次 dump(offset=0)
启动流式会话，逐片接收直到 eof → 落盘原始 .bin 与解析后 .csv（输出目录
power_logs/，文件名带时间戳）。

用法：
  python power_log_dump.py --device-id 5A74
  python power_log_dump.py --device-id 5A74 --clear   # 导出成功后清空设备日志
  python power_log_dump.py --self-test                # 合成数据自测解析器（不连设备）

二进制流格式（power_log_read 逻辑流）：
  头 16 字节：'PWRL' | version=1 | entry_size=12 | reserved u16 | entry_count u32 LE
              | wrap_count u32 LE
  条目 12 字节（packed）：uptime_s u32 LE | vbat_mv u16 LE | mode u8 | flags u8
              | reserved u8[4]
  flags：bit0=充电中 bit1=USB供电 bit2=周期采样 bit3=关机段恢复记录 bit4=时间锚点
  （bit4 时 mode=0xFF，reserved[0..3] 存 uint32 LE epoch 秒）

CSV 列：relative_s, wall_time, mode_name, vbat_mv, charging, usb_powered,
        is_periodic, is_poweroff_segment, is_time_anchor
"""
import argparse
import asyncio
import base64
import csv
import json
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:  # bleak 未安装时仅 --self-test 可用
    BleakClient = None
    BleakScanner = None

# BLE GATT UUID（协议 Doc/Ref/protocol.md）
STATE_TX = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102"
CONTROL_RX = "8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103"

# state_tx JSON 帧头：version(u8)=1 type(u8)=0x10 payload_len(u16 LE)
STATE_HEADER_FMT = "<BBH"
STATE_TYPE_JSON = 0x10

# 二进制流格式常量
MAGIC = b"PWRL"
VERSION = 1
HEADER_LEN = 16
ENTRY_SIZE = 12
CHUNK_MAX = 160  # 单片原始数据上限（字节），控制 BLE MTU 压力

FLAG_CHARGING = 0x01
FLAG_USB = 0x02
FLAG_PERIODIC = 0x04
FLAG_POWEROFF_SEGMENT = 0x08
FLAG_TIME_ANCHOR = 0x10

MODE_ANCHOR = 0xFF
MODE_NAMES = {
    0: "S0_ACTIVE",
    1: "S1_RESTING",
    2: "S2_SCREEN_OFF",
    3: "S3_POWER_OFF",
    4: "RECORDING",
    5: "ADVERTISING",
    6: "OTA",
    MODE_ANCHOR: "TIME_ANCHOR",
}

CSV_COLUMNS = [
    "relative_s", "wall_time", "mode_name", "vbat_mv", "charging",
    "usb_powered", "is_periodic", "is_poweroff_segment", "is_time_anchor",
]


# ---------------------------------------------------------------- 解析（纯函数，可离线自测）

def parse_power_log(data: bytes) -> tuple[dict, list[dict]]:
    """解析 power_log 二进制流，返回 (header_info, entries)。

    entries 中时间锚点条目带 epoch 字段；普通条目含 uptime_s/vbat_mv/mode/flags。
    wall_time 对齐在 write_csv 阶段完成。
    """
    if len(data) < HEADER_LEN:
        raise ValueError(f"数据太短（{len(data)} 字节），不足 16 字节头")
    if data[0:4] != MAGIC:
        raise ValueError(f"魔数不匹配：{data[0:4]!r}，期望 b'PWRL'")
    version = data[4]
    entry_size = data[5]
    if version != VERSION:
        raise ValueError(f"不支持的版本 {version}（期望 {VERSION}）")
    if entry_size != ENTRY_SIZE:
        raise ValueError(f"不支持的 entry_size {entry_size}（期望 {ENTRY_SIZE}）")
    entry_count, wrap_count = struct.unpack_from("<II", data, 8)
    header = {"version": version, "entry_size": entry_size,
              "entry_count": entry_count, "wrap_count": wrap_count}

    available = (len(data) - HEADER_LEN) // ENTRY_SIZE
    if entry_count > available:
        raise ValueError(
            f"头声明 {entry_count} 条但数据仅含 {available} 条（传输不完整？）")

    entries = []
    for i in range(entry_count):
        off = HEADER_LEN + i * ENTRY_SIZE
        uptime_s, vbat_mv, mode, flags = struct.unpack_from("<IHBB", data, off)
        reserved = data[off + 8:off + 12]
        entry = {
            "uptime_s": uptime_s,
            "vbat_mv": vbat_mv,
            "mode": mode,
            "flags": flags,
            "is_time_anchor": bool(flags & FLAG_TIME_ANCHOR),
        }
        if entry["is_time_anchor"]:
            if mode != MODE_ANCHOR:
                raise ValueError(f"条目 {i}：时间锚点 mode={mode:#x}，期望 0xFF")
            entry["epoch"] = struct.unpack("<I", reserved)[0]
        entries.append(entry)
    return header, entries


def resolve_wall_time(entries: list[dict]) -> None:
    """按时间锚点把 uptime 映射为 wall_time（就地写入，缺锚点留空字符串）。

    顺序扫描，锚点条目之后的条目用最近一个锚点对齐；uptime 早于锚点
    （跨重启回绕）时不强行对齐，留空。
    """
    anchor = None  # (anchor_uptime_s, epoch_s)
    for e in entries:
        if e["is_time_anchor"]:
            anchor = (e["uptime_s"], e["epoch"])
            e["wall_time"] = datetime.fromtimestamp(e["epoch"]).isoformat(timespec="seconds")
            continue
        if anchor is not None and e["uptime_s"] >= anchor[0]:
            e["wall_time"] = datetime.fromtimestamp(
                anchor[1] + (e["uptime_s"] - anchor[0])).isoformat(timespec="seconds")
        else:
            e["wall_time"] = ""


def write_csv(entries: list[dict], path: Path) -> None:
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(CSV_COLUMNS)
        for e in entries:
            flags = e["flags"]
            w.writerow([
                e["uptime_s"],
                e["wall_time"],
                MODE_NAMES.get(e["mode"], f"UNKNOWN_{e['mode']}"),
                e["vbat_mv"],
                int(bool(flags & FLAG_CHARGING)),
                int(bool(flags & FLAG_USB)),
                int(bool(flags & FLAG_PERIODIC)),
                int(bool(flags & FLAG_POWEROFF_SEGMENT)),
                int(e["is_time_anchor"]),
            ])


# ---------------------------------------------------------------- BLE 导出

class PowerLogReceiver:
    """收集 state_tx 上的 power_log 分片响应。"""

    def __init__(self):
        self.queue: asyncio.Queue[dict] = asyncio.Queue()

    def on_state(self, _sender, data: bytearray):
        if len(data) < 4:
            return
        _version, ftype, payload_len = struct.unpack(STATE_HEADER_FMT, bytes(data[:4]))
        if ftype != STATE_TYPE_JSON or payload_len == 0:
            return
        try:
            evt = json.loads(bytes(data[4:4 + payload_len]).decode("utf-8"))
        except Exception:  # noqa: BLE001
            return
        if "power_log" in evt:
            self.queue.put_nowait(evt["power_log"])

    async def next_fragment(self, timeout: float = 10.0) -> dict:
        return await asyncio.wait_for(self.queue.get(), timeout=timeout)


async def send_cmd(client, cmd: dict) -> None:
    payload = json.dumps({"power_log": cmd}).encode("utf-8")
    await client.write_gatt_char(CONTROL_RX, payload, response=False)


async def dump_log(client, rx: PowerLogReceiver, chunk: int) -> bytes:
    """下发一次 dump(offset=0) 启动流式会话，逐片接收直到 eof，返回重组后的二进制流。

    固件侧语义（见 Doc/Ref/protocol.md）：dump 是一次性流式会话，设备从 offset
    开始按固定间隔自动逐片上报到 EOF，无需每片重复请求；max 为单片大小提示。
    """
    buf = bytearray()
    await send_cmd(client, {"cmd": "dump", "offset": 0, "max": chunk})
    while True:
        frag = await rx.next_fragment()
        offset = frag.get("offset")
        data = base64.b64decode(frag.get("data", ""))
        if offset != len(buf):
            raise RuntimeError(
                f"分片 offset 不连续：期望 {len(buf)}，收到 {offset}（丢片或乱序）")
        buf.extend(data)
        total = frag.get("total")
        print(f"\r  已接收 {len(buf)}" + (f"/{total}" if total is not None else "") + " 字节",
              end="", flush=True)
        if frag.get("eof"):
            print()
            if total is not None and len(buf) != total:
                raise RuntimeError(f"eof 但总字节不符：收到 {len(buf)}，total={total}")
            if len(buf) == 0:
                raise RuntimeError("设备日志为空（无数据可导出）")
            return bytes(buf)
        if not data:
            raise RuntimeError("收到空分片且未置 eof，传输停滞")


async def run(args: argparse.Namespace) -> int:
    if BleakClient is None:
        print("FAIL: 缺少 bleak 依赖，请先 pip install bleak", file=sys.stderr)
        return 1

    name = f"VS-{args.device_id.upper()}"
    print(f"扫描设备 {name}（确保 VoiceStickApp 已断开）...")
    device = await BleakScanner.find_device_by_name(name, timeout=15.0)
    if device is None:
        print(f"FAIL: 未找到 {name}。确认设备已开机广播；"
              f"若 VoiceStickApp 正在运行请先退出桌面端（BLE 独占单连接）。",
              file=sys.stderr)
        return 1

    rx = PowerLogReceiver()
    try:
        async with BleakClient(device, timeout=20.0) as client:
            print(f"已连接 {device.name}")
            await client.start_notify(STATE_TX, rx.on_state)
            try:
                # 1. 时间锚点（当前 epoch 秒）
                epoch = int(time.time())
                await send_cmd(client, {"cmd": "time_anchor", "epoch": epoch})
                print(f"已下发 time_anchor epoch={epoch}")
                await asyncio.sleep(0.3)

                # 2. 下发一次 dump 启动流式会话，逐片接收直到 eof
                blob = await dump_log(client, rx, args.chunk)

                # 3. 可选：导出成功后清空设备日志
                if args.clear:
                    await send_cmd(client, {"cmd": "clear"})
                    print("已下发 clear（清空设备端日志）")
            finally:
                await client.stop_notify(STATE_TX)
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL: BLE 连接/传输失败：{exc}", file=sys.stderr)
        print("提示：若 VoiceStickApp 正在占用设备，请先退出桌面端再重试。", file=sys.stderr)
        return 1

    # 4. 落盘 + 解析
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    bin_path = outdir / f"power_log_{stamp}.bin"
    csv_path = outdir / f"power_log_{stamp}.csv"
    bin_path.write_bytes(blob)
    print(f"已保存原始日志 {len(blob)} 字节到 {bin_path}")

    try:
        header, entries = parse_power_log(blob)
    except ValueError as exc:
        print(f"FAIL: 日志解析失败：{exc}（原始数据已保存到 {bin_path}）", file=sys.stderr)
        return 1
    resolve_wall_time(entries)
    write_csv(entries, csv_path)
    n_anchor = sum(1 for e in entries if e["is_time_anchor"])
    print(f"解析 {header['entry_count']} 条记录（回卷计数 {header['wrap_count']}，"
          f"时间锚点 {n_anchor} 个），CSV 已保存到 {csv_path}")
    print("OK: 功耗日志导出完成")
    return 0


# ---------------------------------------------------------------- 合成数据自测

def build_synthetic_log() -> bytes:
    """构造一段符合统一格式的合成日志（含锚点/周期采样/关机段/回卷计数）。"""
    entries = [
        # (uptime_s, vbat_mv, mode, flags, reserved)
        (10, 4120, 0, FLAG_CHARGING | FLAG_USB, b"\x00" * 4),           # S0 充电中
        (70, 4115, 1, FLAG_CHARGING | FLAG_USB | FLAG_PERIODIC, b"\x00" * 4),
        (100, 4098, 0xFF, FLAG_TIME_ANCHOR, struct.pack("<I", 1754100000)),  # 锚点
        (130, 4090, 1, FLAG_PERIODIC, b"\x00" * 4),                      # 锚点后 30s
        (200, 4060, 4, 0, b"\x00" * 4),                                  # 录音
        (260, 4040, 3, FLAG_POWEROFF_SEGMENT, b"\x00" * 4),              # 关机段恢复
        (320, 4035, 5, 0, b"\x00" * 4),                                  # 广播
    ]
    body = b"".join(
        struct.pack("<IHBB", u, v, m, fl) + r for u, v, m, fl, r in entries)
    header = MAGIC + bytes([VERSION, ENTRY_SIZE, 0, 0]) + struct.pack("<II", len(entries), 2)
    return header + body


def self_test() -> int:
    blob = build_synthetic_log()
    header, entries = parse_power_log(blob)
    assert header["entry_count"] == 7 and header["wrap_count"] == 2, header
    resolve_wall_time(entries)

    def row(e):
        return [e["uptime_s"], e["wall_time"], MODE_NAMES.get(e["mode"], "?"),
                e["vbat_mv"], int(bool(e["flags"] & FLAG_CHARGING)),
                int(bool(e["flags"] & FLAG_USB)),
                int(bool(e["flags"] & FLAG_PERIODIC)),
                int(bool(e["flags"] & FLAG_POWEROFF_SEGMENT)),
                int(e["is_time_anchor"])]

    # 锚点前条目无 wall_time
    assert entries[0]["wall_time"] == "", entries[0]
    assert row(entries[0]) == [10, "", "S0_ACTIVE", 4120, 1, 1, 0, 0, 0]
    assert row(entries[1]) == [70, "", "S1_RESTING", 4115, 1, 1, 1, 0, 0]
    # 锚点条目
    assert entries[2]["epoch"] == 1754100000
    assert entries[2]["wall_time"] == datetime.fromtimestamp(1754100000).isoformat(timespec="seconds")
    assert row(entries[2])[2] == "TIME_ANCHOR" and row(entries[2])[8] == 1
    # 锚点后对齐：uptime 130 = epoch + 30
    assert entries[3]["wall_time"] == datetime.fromtimestamp(1754100030).isoformat(timespec="seconds")
    assert row(entries[3]) == [130, entries[3]["wall_time"], "S1_RESTING", 4090, 0, 0, 1, 0, 0]
    assert row(entries[4])[2] == "RECORDING" and row(entries[4])[1] != ""
    assert row(entries[5]) == [260, entries[5]["wall_time"], "S3_POWER_OFF", 4040, 0, 0, 0, 1, 0]
    assert row(entries[6])[2] == "ADVERTISING"

    # 落盘验证 CSV 内容
    tmp = Path(__file__).parent / "power_logs" / "_selftest.csv"
    tmp.parent.mkdir(parents=True, exist_ok=True)
    write_csv(entries, tmp)
    with open(tmp, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    assert rows[0] == CSV_COLUMNS, rows[0]
    assert len(rows) == 8
    assert rows[1][:4] == ["10", "", "S0_ACTIVE", "4120"]
    assert rows[4][0] == "130" and rows[4][1] != "" and rows[4][2] == "S1_RESTING"
    assert rows[3][2] == "TIME_ANCHOR" and rows[3][8] == "1"
    tmp.unlink()
    try:
        tmp.parent.rmdir()
    except OSError:
        pass

    # 异常路径：坏魔数 / 截断数据
    try:
        parse_power_log(b"XXXX" + blob[4:])
        raise AssertionError("坏魔数未报错")
    except ValueError:
        pass
    try:
        parse_power_log(blob[:-6])
        raise AssertionError("截断数据未报错")
    except ValueError:
        pass

    print("OK: 自测通过（解析/锚点对齐/CSV 落盘/异常路径均正确）")
    return 0


# ---------------------------------------------------------------- 入口

def main() -> int:
    ap = argparse.ArgumentParser(
        description="power_log 功耗日志 BLE 导出（VoiceStickApp 需先断开）。")
    ap.add_argument("--device-id", help="设备 ID（VS-XXXX 的 XXXX，如 5A74）")
    ap.add_argument("--outdir",
                    default=str(Path(__file__).parent / "power_logs"),
                    help="输出目录（默认 scripts/e2e_test/power_logs/）")
    ap.add_argument("--chunk", type=int, default=CHUNK_MAX,
                    help=f"单片最大原始字节数（默认 {CHUNK_MAX}）")
    ap.add_argument("--clear", action="store_true",
                    help="导出成功后下发 clear 清空设备端日志")
    ap.add_argument("--self-test", action="store_true",
                    help="用合成数据自测解析器，不连接设备")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.device_id:
        ap.error("需要 --device-id（或用 --self-test 离线自测）")
    return asyncio.run(run(args))


if __name__ == "__main__":
    sys.exit(main())
