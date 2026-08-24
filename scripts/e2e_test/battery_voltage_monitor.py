"""电池电压监测与可视化系统（VoiceStick StickS3，bleak + matplotlib）。

以固定 60 秒间隔采集设备电池电压，持续监测 60 分钟（约 60 个数据点），
随后绘制时间-电压曲线并保存：

  - CSV 原始数据（时间戳 + 电压 + 充电/供电状态 + 电源模式），实时增量落盘；
  - PNG + SVG 曲线图（X 轴时间、Y 轴电压、数据点标记、标题、充电段底色）。

电压来源是固件 power_log 组件的 60s 周期 VBAT 采样（vbat_mv 原始毫伏值），
经 BLE control_rx/state_tx 的 power_log 命令族导出（协议见
Doc/Ref/protocol.md「Power Log Export」）。数据点时间戳由 time_anchor 锚点把
设备 uptime 映射到墙钟，采样间隔由设备端 esp_timer 保证，不受桌面端调度抖动
影响；查询节拍用单调时钟绝对对齐，无累积漂移。

两种采集模式：
  live（默认）  保持 BLE 连接，每 60s 增量导出新采样，实时打印与落盘。
                注意：电池供电下设备空闲约 11 分钟会自动关机断连（即使
                BLE 连接中），长时间监测请保持 USB 供电（脚本会检测供电
                状态并警告；USB 下曲线为充电/浮充电压）。
  --offline     只在首尾连接：下发 time_anchor 后断开，等待监测时长让设备
                自动采样，结束时重连一次性导出。USB 供电下可完整覆盖，
                且期间桌面端可自由使用。

异常处理：依赖缺失、设备未找到/被桌面端占用、BLE 断连、导出超时或乱序
（周期内重试，连续失败中止）、电压读数无效（vbat_mv=0 标记 invalid）、
设备重启（uptime 回退）、输出目录不可写均有明确报错；任何中断/失败路径
都会保存已采集数据（CSV + 部分数据曲线）。

用法：
  python battery_voltage_monitor.py --device-id 5A74            # live 模式
  python battery_voltage_monitor.py --device-id 5A74 --offline  # 挂机模式
  python battery_voltage_monitor.py --demo                      # 合成数据验证绘图链路
  python battery_voltage_monitor.py --self-test                 # 离线自测（不连设备）

依赖：bleak、matplotlib（未列入根目录 requirements.txt，需另行 pip install）。
运行前请退出 VoiceStickApp（BLE 独占单连接）。
"""
import argparse
import asyncio
import base64
import csv
import json
import random
import struct
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import power_log_dump as pld  # 复用协议常量与解析（STATE_TX/CONTROL_RX/FLAG_*/parse_power_log 等）

DEFAULT_INTERVAL_S = 60
DEFAULT_DURATION_S = 3600
DUMP_TIMEOUT_S = 20.0        # 单个分片等待超时（片间固定 30ms，20s 已很宽松）
BATTERY_QUERY_TIMEOUT_S = 3.0
MAX_ATTEMPTS_PER_CYCLE = 3   # 单个采集周期内的导出尝试次数
MAX_FAILED_CYCLES = 3        # 连续失败周期数上限，超过即中止
EMPTY_ROUNDS_WARN = 3        # 连续无新数据周期数上限（60s 对 60s 偶发一轮空正常）
TAIL_ENTRIES = 4             # live 基线探测时回看的历史条目数（仅用于 baseline uptime）

CSV_COLUMNS = [
    "seq", "timestamp_iso", "epoch_s", "uptime_s", "vbat_mv", "vbat_v",
    "charging", "usb_powered", "mode_name", "valid",
]


class MonitorAbort(Exception):
    """监测中途中止（断连/重启/连续失败/用户中断），携带已采集数据行。"""

    def __init__(self, reason: str, rows: list):
        super().__init__(reason)
        self.rows = rows


# ---------------------------------------------------------------- 纯函数（可离线自测）

def parse_bare_entries(data: bytes) -> list[dict]:
    """解析不带 16 字节头的裸条目流（增量 offset 导出得到的数据）。"""
    if len(data) % pld.ENTRY_SIZE != 0:
        raise ValueError(f"裸条目流长度 {len(data)} 不是 {pld.ENTRY_SIZE} 的倍数")
    out = []
    for off in range(0, len(data), pld.ENTRY_SIZE):
        uptime_s, vbat_mv, mode, flags = struct.unpack_from("<IHBB", data, off)
        reserved = data[off + 8:off + 12]
        entry = {
            "uptime_s": uptime_s, "vbat_mv": vbat_mv, "mode": mode, "flags": flags,
            "is_time_anchor": bool(flags & pld.FLAG_TIME_ANCHOR),
        }
        if entry["is_time_anchor"]:
            entry["epoch"] = struct.unpack("<I", reserved)[0]
        out.append(entry)
    return out


def collect_anchors(entries: list[dict]) -> list[tuple[int, int]]:
    """收集 (uptime_s, epoch_s) 锚点对。"""
    return [(e["uptime_s"], e["epoch"]) for e in entries if e["is_time_anchor"]]


def entry_epoch(e: dict, anchors: list[tuple[int, int]]):
    """按最近锚点把 uptime 映射为 epoch 秒；对不上（设备重启回绕）返回 None。"""
    best = None
    for a_u, a_e in anchors:
        if e["uptime_s"] >= a_u and (best is None or a_u > best[0]):
            best = (a_u, a_e)
    if best is None:
        return None
    return best[1] + (e["uptime_s"] - best[0])


def periodic_entries(entries: list[dict], last_uptime: int) -> tuple[list[dict], int]:
    """过滤出 uptime > last_uptime 的周期采样条目，返回 (条目列表, 新最大 uptime)。"""
    picked, max_u = [], last_uptime
    for e in entries:
        if e["is_time_anchor"] or not (e["flags"] & pld.FLAG_PERIODIC):
            continue
        if e["uptime_s"] <= last_uptime:
            continue
        picked.append(e)
        max_u = max(max_u, e["uptime_s"])
    return picked, max_u


def detect_restart(entries: list[dict], last_uptime: int) -> bool:
    """增量条目流中出现 uptime 回退即视为设备重启过（uptime 计数归零）。

    只对"本次导出新增的条目"调用（live 模式的增量流）；全量日志里的
    历史条目 uptime 天然小于基线，不适用本函数。
    """
    return any(e["uptime_s"] < last_uptime for e in entries
               if not e["is_time_anchor"])


def to_point(e: dict, anchors: list[tuple[int, int]], seq: int) -> dict:
    """把 power_log 条目转成 CSV 行（vbat_mv=0 为设备 ADC 读失败，标记 invalid）。"""
    ep = entry_epoch(e, anchors)
    vbat_mv = e["vbat_mv"]
    return {
        "seq": seq,
        "timestamp_iso": (datetime.fromtimestamp(ep).isoformat(timespec="seconds")
                          if ep is not None else ""),
        "epoch_s": ep if ep is not None else "",
        "uptime_s": e["uptime_s"],
        "vbat_mv": vbat_mv,
        "vbat_v": f"{vbat_mv / 1000:.3f}",
        "charging": int(bool(e["flags"] & pld.FLAG_CHARGING)),
        "usb_powered": int(bool(e["flags"] & pld.FLAG_USB)),
        "mode_name": pld.MODE_NAMES.get(e["mode"], f"UNKNOWN_{e['mode']}"),
        "valid": int(vbat_mv > 0),
    }


# ---------------------------------------------------------------- 输出预检与 CSV 记录器

def preflight_outdir(outdir: Path) -> None:
    """启动前预检输出目录：可创建、可写入；失败给出明确错误。"""
    try:
        outdir.mkdir(parents=True, exist_ok=True)
        probe = outdir / ".write_probe"
        probe.write_text("ok", encoding="utf-8")
        probe.unlink()
    except OSError as exc:
        print(f"FAIL: 输出目录不可用（{outdir}）：{exc}", file=sys.stderr)
        print("请检查路径是否合法、磁盘是否可写、是否有足够权限。", file=sys.stderr)
        raise SystemExit(1)


class Recorder:
    """CSV 增量写入器：每收到一个数据点立即落盘，中断不丢已采数据。"""

    def __init__(self, outdir: Path, prefix: str):
        self.path = outdir / f"{prefix}.csv"
        self.rows: list[dict] = []
        self._f = open(self.path, "w", newline="", encoding="utf-8")
        self._w = csv.writer(self._f)
        self._w.writerow(CSV_COLUMNS)
        self._f.flush()

    def add(self, point: dict) -> None:
        self.rows.append(point)
        self._w.writerow([point[c] for c in CSV_COLUMNS])
        self._f.flush()

    def close(self) -> None:
        if not self._f.closed:
            self._f.close()


# ---------------------------------------------------------------- BLE 层

class MonitorReceiver:
    """state_tx 订阅回调：power_log 分片与 battery_status 事件分两个队列。"""

    def __init__(self):
        self.queue: asyncio.Queue[dict] = asyncio.Queue()   # power_log 分片
        self.battery: asyncio.Queue[dict] = asyncio.Queue()  # battery_status 事件

    def on_state(self, _sender, data: bytearray):
        if len(data) < 4:
            return
        _version, ftype, payload_len = struct.unpack(pld.STATE_HEADER_FMT, bytes(data[:4]))
        if ftype != pld.STATE_TYPE_JSON or payload_len == 0:
            return
        try:
            evt = json.loads(bytes(data[4:4 + payload_len]).decode("utf-8"))
        except Exception:  # noqa: BLE001
            return
        if "power_log" in evt:
            self.queue.put_nowait(evt["power_log"])
        elif evt.get("event") == "battery_status":
            self.battery.put_nowait(evt)


async def send_json(client, obj: dict) -> None:
    await client.write_gatt_char(pld.CONTROL_RX, json.dumps(obj).encode("utf-8"),
                                 response=False)


async def dump_stream(client, rx: MonitorReceiver, offset: int,
                      timeout: float = DUMP_TIMEOUT_S) -> tuple[bytes, int]:
    """下发一次 dump(offset) 并收齐分片，返回 (增量字节流, 会话 total)。

    静默版 pld.dump_log：支持任意起始 offset（增量导出），越界 offset 会被
    设备钳到 total 并立即回 eof 空片（用于探测日志大小）。
    """
    await send_json(client, {"power_log": {"cmd": "dump", "offset": offset,
                                           "max": pld.CHUNK_MAX}})
    buf = bytearray()
    total = None
    while True:
        frag = await rx.next_fragment(timeout=timeout)
        off = frag.get("offset")
        data = base64.b64decode(frag.get("data", ""))
        if off != offset + len(buf):
            raise RuntimeError(f"分片 offset 不连续：期望 {offset + len(buf)}，"
                               f"收到 {off}（丢片或乱序）")
        total = frag.get("total", total)
        buf.extend(data)
        if frag.get("eof"):
            if total is not None and len(buf) != total - offset:
                raise RuntimeError(f"eof 但字节数不符：收到 {len(buf)}，"
                                   f"期望 {total - offset}")
            return bytes(buf), int(total)
        if not data:
            raise RuntimeError("收到空分片且未置 eof，传输停滞")


async def probe_log_size(client, rx: MonitorReceiver) -> int:
    """用越界 offset 探测设备日志当前大小（total），不传历史数据。"""
    await send_json(client, {"power_log": {"cmd": "dump", "offset": 1_000_000_000,
                                           "max": 16}})
    frag = await rx.next_fragment(timeout=DUMP_TIMEOUT_S)
    total = frag.get("total")
    if total is None:
        raise RuntimeError("探测日志大小时响应缺少 total 字段")
    return int(total)


async def query_battery_status(client, rx: MonitorReceiver):
    """下发 battery_status_request 等待供电状态，超时返回 None。"""
    try:
        await send_json(client, {"event": "battery_status_request"})
        return await asyncio.wait_for(rx.battery.get(), timeout=BATTERY_QUERY_TIMEOUT_S)
    except asyncio.TimeoutError:
        return None


async def find_device(device_id: str):
    if pld.BleakClient is None:
        print("FAIL: 缺少 bleak 依赖，请先 pip install bleak", file=sys.stderr)
        raise SystemExit(1)
    name = f"VS-{device_id.upper()}"
    print(f"扫描设备 {name}（确保 VoiceStickApp 已退出，BLE 独占单连接）...")
    device = await pld.BleakScanner.find_device_by_name(name, timeout=15.0)
    if device is None:
        raise RuntimeError(
            f"未找到 {name}：确认设备已开机广播；若 VoiceStickApp 正在运行请先"
            f"退出桌面端；若设备已自动关机请按主键唤醒后重试。")
    return device


def print_battery_warning(batt) -> None:
    if not batt:
        print("提示：未能获取设备供电状态（battery_status 超时），继续监测。")
        return
    charging = bool(batt.get("charging"))
    usb = bool(batt.get("usb_powered"))
    print(f"设备供电状态：level={batt.get('level')}%  charging={charging}  "
          f"usb_powered={usb}")
    if not (charging or usb):
        print("警告：当前为电池供电。设备空闲约 11 分钟会自动关机并断开 BLE"
              "（连接态也不例外），60 分钟监测大概率中断。建议插 USB 供电"
              "（此时曲线为充电/浮充电压）或改用 --offline 模式。")


# ---------------------------------------------------------------- live 模式

async def live_monitor(args, outdir: Path, prefix: str) -> list[dict]:
    device = await find_device(args.device_id)
    rx = MonitorReceiver()
    recorder = None
    try:
        async with pld.BleakClient(device, timeout=20.0) as client:
            print(f"已连接 {device.name}")
            await client.start_notify(pld.STATE_TX, rx.on_state)
            try:
                print_battery_warning(await query_battery_status(client, rx))

                if args.clear_first:
                    await send_json(client, {"power_log": {"cmd": "clear"}})
                    print("已清空设备端功耗日志（--clear-first）")
                    await asyncio.sleep(0.3)

                epoch = int(time.time())
                await send_json(client, {"power_log": {"cmd": "time_anchor",
                                                       "epoch": epoch}})
                print(f"已下发 time_anchor epoch={epoch}")
                await asyncio.sleep(0.3)

                # 基线：探测日志大小，仅回看尾部少量条目拿 baseline uptime，
                # 不全量导出（历史日志最大 256KB，全量走 BLE 约需 50s）。
                total = await probe_log_size(client, rx)
                last_size = total
                last_uptime = 0
                anchors = []
                if total > pld.HEADER_LEN:
                    start = max(pld.HEADER_LEN, total - pld.ENTRY_SIZE * TAIL_ENTRIES)
                    tail, last_size = await dump_stream(client, rx, offset=start)
                    tail_entries = parse_bare_entries(tail)
                    anchors = collect_anchors(tail_entries)
                    last_uptime = max((e["uptime_s"] for e in tail_entries), default=0)
                print(f"日志基线：size={last_size}B  baseline_uptime={last_uptime}s")

                recorder = Recorder(outdir, prefix)
                cycles = max(1, args.duration // args.interval)
                print(f"开始监测：{cycles} 个周期 × {args.interval}s"
                      f"（预计 {cycles * args.interval // 60} 分钟），"
                      f"数据实时写入 {recorder.path.name}")

                t0 = time.monotonic()
                failed_cycles = 0
                empty_rounds = 0
                for i in range(1, cycles + 1):
                    delay = t0 + i * args.interval - time.monotonic()
                    if delay > 0:
                        await asyncio.sleep(delay)

                    blob = None
                    err = None
                    for attempt in range(1, MAX_ATTEMPTS_PER_CYCLE + 1):
                        try:
                            blob, new_total = await dump_stream(client, rx,
                                                                offset=last_size)
                            break
                        except (asyncio.TimeoutError, RuntimeError) as exc:
                            err = exc
                            print(f"  周期 {i}：第 {attempt} 次导出失败（{exc}）")
                    if blob is None:
                        failed_cycles += 1
                        if failed_cycles >= MAX_FAILED_CYCLES:
                            raise RuntimeError(
                                f"连续 {MAX_FAILED_CYCLES} 个采集周期导出失败，"
                                f"中止监测（已保存 {len(recorder.rows)} 点）")
                        continue
                    failed_cycles = 0
                    if new_total < last_size:
                        raise RuntimeError(
                            f"日志 total 回退（{last_size} -> {new_total}），"
                            f"设备端日志被清空或覆盖，中止监测")

                    entries = parse_bare_entries(blob)
                    if detect_restart(entries, last_uptime):
                        raise RuntimeError(
                            "设备疑似重启（uptime 回退），时间连续性被破坏，"
                            "中止监测。已采集数据已保存。")
                    anchors.extend(collect_anchors(entries))
                    picked, last_uptime = periodic_entries(entries, last_uptime)
                    last_size = new_total

                    if not picked:
                        empty_rounds += 1
                        if empty_rounds >= EMPTY_ROUNDS_WARN:
                            print(f"  周期 {i}：连续 {empty_rounds} 轮无新采样，"
                                  f"设备端 60s 周期采样疑似停滞")
                        else:
                            print(f"  周期 {i}/{cycles}：无新采样点（相位错开，正常）")
                        continue
                    empty_rounds = 0
                    for e in picked:
                        point = to_point(e, anchors, len(recorder.rows) + 1)
                        recorder.add(point)
                        mark = " 充电中" if point["charging"] else ""
                        invalid = "  [无效读数 vbat=0]" if not point["valid"] else ""
                        print(f"  周期 {i}/{cycles}  {point['timestamp_iso']}  "
                              f"{point['vbat_mv']} mV{mark}{invalid}")
            finally:
                await client.stop_notify(pld.STATE_TX)
    except (KeyboardInterrupt, asyncio.CancelledError):
        raise MonitorAbort("用户中断（Ctrl+C）", recorder.rows if recorder else [])
    except RuntimeError as exc:
        raise MonitorAbort(str(exc), recorder.rows if recorder else [])
    except Exception as exc:  # noqa: BLE001  bleak 断连等
        raise MonitorAbort(
            f"BLE 连接失败/断开（{exc}）。常见原因：设备因电池供电空闲策略自动"
            f"关机、超出范围、或被其他程序占用。已采集数据已保存。",
            recorder.rows if recorder else [])
    finally:
        if recorder is not None:
            recorder.close()
    return recorder.rows


# ---------------------------------------------------------------- offline 模式

async def offline_monitor(args, outdir: Path, prefix: str) -> list[dict]:
    # 会话 1：锚点 + 供电状态
    device = await find_device(args.device_id)
    rx = MonitorReceiver()
    start_epoch = int(time.time())
    async with pld.BleakClient(device, timeout=20.0) as client:
        print(f"已连接 {device.name}")
        await client.start_notify(pld.STATE_TX, rx.on_state)
        try:
            print_battery_warning(await query_battery_status(client, rx))
            await send_json(client, {"power_log": {"cmd": "time_anchor",
                                                   "epoch": start_epoch}})
            print(f"已下发 time_anchor epoch={start_epoch}，随后断开连接")
        finally:
            await client.stop_notify(pld.STATE_TX)

    duration_s = max(args.interval, args.duration)
    print(f"等待 {duration_s}s（设备端每 {args.interval}s 自动采样；"
          f"Ctrl+C 可提前结束并导出已有数据）...")
    try:
        end_wall = time.monotonic() + duration_s
        while True:
            remain = end_wall - time.monotonic()
            if remain <= 0:
                break
            await asyncio.sleep(min(300, remain))
            print(f"  ... 剩余 {int(end_wall - time.monotonic())}s")
    except (KeyboardInterrupt, asyncio.CancelledError):
        print("提前结束，导出监测窗口内已有数据")
    end_epoch = int(time.time())

    # 会话 2：重连导出
    device = await find_device(args.device_id)
    rx = MonitorReceiver()
    async with pld.BleakClient(device, timeout=20.0) as client:
        print(f"已重新连接 {device.name}")
        await client.start_notify(pld.STATE_TX, rx.on_state)
        try:
            blob, _ = await dump_stream(client, rx, offset=0)
        finally:
            await client.stop_notify(pld.STATE_TX)

    _header, entries = pld.parse_power_log(blob)
    anchors = collect_anchors(entries)
    rows, unaligned = [], 0
    for e in entries:
        if e["is_time_anchor"] or not (e["flags"] & pld.FLAG_PERIODIC):
            continue
        ep = entry_epoch(e, anchors)
        if ep is None:
            unaligned += 1
            continue
        if start_epoch - 5 <= ep <= end_epoch + 5:
            rows.append(to_point(e, anchors, len(rows) + 1))
    if unaligned:
        print(f"警告：{unaligned} 条采样无法对齐墙钟时间（设备可能在监测期间"
              f"重启过），已跳过")
    if not rows:
        raise MonitorAbort(
            "监测窗口内没有采样点：设备可能在监测期间关机（电池供电空闲策略）"
            "或采样停滞。可插 USB 供电后重试。", [])

    recorder = Recorder(outdir, prefix)
    try:
        for r in rows:
            recorder.add(r)
    finally:
        recorder.close()
    return recorder.rows


# ---------------------------------------------------------------- 绘图

def plot_and_save(rows: list[dict], outdir: Path, prefix: str,
                  device_label: str, formats: list[str]) -> list[Path]:
    valid = [r for r in rows if r["valid"] and isinstance(r["epoch_s"], (int, float))]
    if len(valid) < 2:
        raise RuntimeError(f"有效电压数据点不足（{len(valid)} 个），无法绘制曲线")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        raise RuntimeError("缺少 matplotlib 依赖（原始数据已保存 CSV），"
                           "请先 pip install matplotlib")

    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei",
                                       "Noto Sans CJK SC", "sans-serif"]
    plt.rcParams["axes.unicode_minus"] = False

    t0 = valid[0]["epoch_s"]
    xs = [(r["epoch_s"] - t0) / 60.0 for r in valid]
    ys = [r["vbat_mv"] for r in valid]

    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(xs, ys, "-", color="#1f77b4", lw=1.3, marker="o", ms=4,
            label="电池电压")

    # 充电段底色：把连续 charging=1 的点区间涂浅绿
    chg = [(x, r) for x, r in zip(xs, valid) if r["charging"]]
    if chg:
        seg_start = chg[0][0]
        labeled = False
        for j in range(1, len(chg) + 1):
            if j == len(chg) or chg[j][0] - chg[j - 1][0] > 2.0:
                ax.axvspan(max(0.0, seg_start - 0.5), chg[j - 1][0] + 0.5,
                           color="green", alpha=0.12,
                           label="充电中" if not labeled else None)
                labeled = True
                if j < len(chg):
                    seg_start = chg[j][0]

    span_min = (valid[-1]["epoch_s"] - t0) / 60.0
    ax.set_xlabel("时间（分钟，自监测开始）")
    ax.set_ylabel("电池电压（mV）")
    ax.set_title(f"{device_label} 电池电压监测"
                 f"（{len(valid)} 点 | {valid[0]['timestamp_iso']} ~ "
                 f"{valid[-1]['timestamp_iso']} | 覆盖 {span_min:.0f} 分钟）")
    ax.set_xlim(-max(1.0, span_min * 0.02), span_min * 1.02)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="best")
    fig.tight_layout()

    paths = []
    for fmt in formats:
        p = outdir / f"{prefix}.{fmt}"
        fig.savefig(p, dpi=150)
        paths.append(p)
    plt.close(fig)
    return paths


def print_summary(rows: list[dict]) -> None:
    valid = [r for r in rows if r["valid"] and isinstance(r["epoch_s"], (int, float))]
    invalid = len(rows) - len(valid)
    if not valid:
        print("统计：无有效数据点")
        return
    volts = [r["vbat_mv"] for r in valid]
    drop = volts[0] - volts[-1]
    chg = sum(1 for r in valid if r["charging"])
    print(f"统计：共 {len(rows)} 点（有效 {len(valid)}"
          f"{'，无效 ' + str(invalid) if invalid else ''}），"
          f"电压 {min(volts)}~{max(volts)} mV，"
          f"首点 {volts[0]} mV -> 末点 {volts[-1]} mV"
          f"（{'下降' if drop >= 0 else '上升'} {abs(drop)} mV），"
          f"充电中采样 {chg} 点")


# ---------------------------------------------------------------- demo / 自测

def demo_rows() -> list[dict]:
    """合成 61 点放电曲线（60 分钟，60s 间隔），验证绘图与保存链路。"""
    random.seed(2026)
    t0 = time.time() - 3600
    rows = []
    v = 4146.0
    for i in range(61):
        v -= 6.0 + 0.02 * i + random.uniform(-2.0, 2.0)
        ep = int(t0 + i * 60)
        rows.append({
            "seq": i + 1,
            "timestamp_iso": datetime.fromtimestamp(ep).isoformat(timespec="seconds"),
            "epoch_s": ep, "uptime_s": 1000 + i * 60,
            "vbat_mv": round(v), "vbat_v": f"{v / 1000:.3f}",
            "charging": 0, "usb_powered": 0,
            "mode_name": "S2_SCREEN_OFF", "valid": 1,
        })
    return rows


def self_test() -> int:
    outdir = Path(__file__).parent / "battery_logs"
    print("== self_test：数据管道纯函数与异常路径 ==")

    blob = pld.build_synthetic_log()
    _header, entries = pld.parse_power_log(blob)
    anchors = collect_anchors(entries)
    assert anchors == [(100, 1754100000)], anchors

    # 锚点对齐：锚点后 30s 的条目
    e130 = next(e for e in entries if e["uptime_s"] == 130)
    assert entry_epoch(e130, anchors) == 1754100030
    # 锚点前的条目无法对齐
    e10 = next(e for e in entries if e["uptime_s"] == 10)
    assert entry_epoch(e10, anchors) is None

    # 周期条目过滤：只取 FLAG_PERIODIC 且 uptime > last_uptime
    picked, max_u = periodic_entries(entries, 0)
    assert [e["uptime_s"] for e in picked] == [70, 130], picked
    assert max_u == 130
    picked, _ = periodic_entries(entries, 100)
    assert [e["uptime_s"] for e in picked] == [130]

    # 无效读数（vbat=0）标记 invalid 但保留行
    bad = dict(e130, vbat_mv=0)
    p = to_point(bad, anchors, 1)
    assert p["valid"] == 0 and p["vbat_mv"] == 0 and p["epoch_s"] == 1754100030
    p = to_point(e130, anchors, 2)
    assert p["valid"] == 1 and p["charging"] == 0 and p["mode_name"] == "S1_RESTING"

    # 重启检测：仅对增量条目流有意义（历史条目 uptime 天然小于基线）
    inc_normal = [dict(e130, uptime_s=200), dict(e130, uptime_s=260)]
    assert not detect_restart(inc_normal, 130)
    inc_restarted = [dict(e130, uptime_s=5)]
    assert detect_restart(inc_restarted, 130)

    # 裸条目流解析（增量 offset 导出场景）
    bare = blob[pld.HEADER_LEN:pld.HEADER_LEN + 2 * pld.ENTRY_SIZE]
    bare_entries = parse_bare_entries(bare)
    assert [e["uptime_s"] for e in bare_entries] == [10, 70]
    try:
        parse_bare_entries(bare[:-1])
        raise AssertionError("非 12 倍数长度未报错")
    except ValueError:
        pass

    # Recorder CSV 落盘格式
    outdir.mkdir(parents=True, exist_ok=True)
    tmp = outdir / "_selftest.csv"
    rec = Recorder(outdir, "_selftest")
    rec.add(to_point(e130, anchors, 1))
    rec.add(to_point(bad, anchors, 2))
    rec.close()
    with open(tmp, newline="", encoding="utf-8") as f:
        rows = list(csv.reader(f))
    assert rows[0] == CSV_COLUMNS
    assert len(rows) == 3 and rows[1][3] == "130" and rows[1][4] == "4090"
    assert rows[2][9] == "0"
    tmp.unlink()

    # 输出目录预检：非法路径明确报错（Windows 非法字符 '<'）
    try:
        preflight_outdir(Path(outdir / "a<b"))
        raise AssertionError("非法路径未被拒绝")
    except SystemExit:
        pass

    # 坏数据解析（复用 pld.parse_power_log 的校验）
    try:
        pld.parse_power_log(b"XXXX" + blob[4:])
        raise AssertionError("坏魔数未报错")
    except ValueError:
        pass

    print("OK: 自测通过（锚点对齐/周期过滤/无效读数/重启检测/裸条目流/CSV/预检）")
    return 0


# ---------------------------------------------------------------- 入口

def main() -> int:
    ap = argparse.ArgumentParser(
        description="电池电压 60 分钟监测与可视化（VoiceStick StickS3，"
                    "VoiceStickApp 需先退出）")
    ap.add_argument("--device-id", help="设备 ID（VS-XXXX 的 XXXX，如 5A74）")
    ap.add_argument("--interval", type=int, default=DEFAULT_INTERVAL_S,
                    help=f"采集间隔秒（默认 {DEFAULT_INTERVAL_S}，"
                         f"与设备端 power_log 周期采样一致）")
    ap.add_argument("--duration", type=int, default=DEFAULT_DURATION_S,
                    help=f"监测总时长秒（默认 {DEFAULT_DURATION_S} = 60 分钟）")
    ap.add_argument("--outdir",
                    default=str(Path(__file__).parent / "battery_logs"),
                    help="输出目录（默认 scripts/e2e_test/battery_logs/）")
    ap.add_argument("--offline", action="store_true",
                    help="免保持连接模式：只在首尾连接，中间设备自动采样")
    ap.add_argument("--clear-first", action="store_true",
                    help="会话开始先清空设备端功耗日志（默认不清空；"
                         "清空可保证基线探测更干净，但会丢弃历史功耗记录）")
    ap.add_argument("--format", default="png,svg",
                    help="曲线图格式，逗号分隔（默认 png,svg）")
    ap.add_argument("--demo", action="store_true",
                    help="合成数据演示，验证绘图与保存链路（不连设备）")
    ap.add_argument("--self-test", action="store_true",
                    help="离线自测数据管道（不连设备）")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    formats = [f.strip().lower() for f in args.format.split(",") if f.strip()]
    bad = [f for f in formats if f not in ("png", "svg")]
    if bad or not formats:
        ap.error(f"--format 仅支持 png/svg，收到：{bad or '空'}")

    outdir = Path(args.outdir)
    preflight_outdir(outdir)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")

    if args.demo:
        prefix = f"battery_demo_{stamp}"
        label = "DEMO"
        rows = demo_rows()
        recorder = Recorder(outdir, prefix)
        try:
            for r in rows:
                recorder.add(r)
        finally:
            recorder.close()
        print(f"DEMO：合成 {len(rows)} 点放电曲线，CSV 已保存到 {recorder.path}")
    else:
        if not args.device_id:
            ap.error("需要 --device-id（或用 --demo / --self-test 离线验证）")
        if args.interval <= 0 or args.duration <= 0:
            ap.error("--interval 与 --duration 必须为正数")
        label = f"VS-{args.device_id.upper()}"
        prefix = f"battery_{label}_{stamp}"
        exit_code = 0
        try:
            if args.offline:
                rows = asyncio.run(offline_monitor(args, outdir, prefix))
            else:
                rows = asyncio.run(live_monitor(args, outdir, prefix))
        except MonitorAbort as exc:
            print(f"FAIL: {exc}", file=sys.stderr)
            rows = exc.rows
            exit_code = 1

    csv_path = outdir / f"{prefix}.csv"
    print(f"\n原始数据已保存：{csv_path}")
    print_summary(rows)

    try:
        paths = plot_and_save(rows, outdir, prefix, label, formats)
        for p in paths:
            print(f"曲线图已保存：{p}")
        print("OK: 电池电压监测完成")
    except RuntimeError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    return 0 if len(rows) >= 2 else 1


if __name__ == "__main__":
    sys.exit(main())
