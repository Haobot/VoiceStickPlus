"""StickS3 分模式功耗分析报告（Doc/Plan/power-mode-energy-profiling.md 第 7 节，阶段 3）。

输入 power_log_dump.py 产出的 CSV（或直接喂导出的 .bin），结合 power_model.json
的每模式电流常数，输出 Markdown 报告并打印终端摘要：

  1. 各模式驻留时长与占比（充电/USB 供电时段单列，时长仍列出但不计入耗电归属）；
  2. 各模式估算能耗 mAh = 时长(h) x I_mode(mA)，按占比排序；
  3. 纯电池长时段（>10min 单模式）VBAT 线性回归斜率 -> 估算该模式实测平均电流，
     与模型值并列对比（换算用 VBAT-SOC 线性近似，见 power_model.json battery 段）；
  4. 按本次记录的使用分布估算整机续航。

用法：
    python power_report.py power_log.csv [--model power_model.json] [--out report.md]
    python power_report.py power_log.bin
    python power_report.py --self-test

CSV 约定（power_log_dump.py 产出；本脚本对列名做别名容错）：
    必需列：uptime_s（相对秒）、mode（数字 0..6 或名字如 S0_ACTIVE/RECORDING，
            锚点行用 TIME_ANCHOR/255 表示）、vbat_mv
    标志列：flags（十进制或 0x 十六进制，bit0=充电 bit1=USB bit2=周期采样 bit3=关机段
            恢复 bit4=时间锚点）；若无 flags 列则用 charging,usb 两列（1/0/true/false）
    可选列：wall_time（对齐后 wall clock，本报告不参与计算）
    以 '#' 开头的行为注释（dump 脚本可写元信息），自动跳过。

BIN 格式见统一约定：16 字节头（'PWRL' 魔数）+ 12 字节定长条目。

依赖：纯 stdlib；有 numpy 时用 numpy 做回归，无则纯 Python 最小二乘。
"""
import argparse
import csv
import io
import json
import os
import struct
import sys

try:
    import numpy as np
    _HAVE_NP = True
except ImportError:
    _HAVE_NP = False

# 与 firmware/components/power_log/include/power_log.h 的 power_mode_t 一致
MODE_NAMES = [
    "S0_ACTIVE", "S1_RESTING", "S2_SCREEN_OFF", "S3_POWER_OFF",
    "RECORDING", "ADVERTISING", "OTA",
]
MODE_COUNT = len(MODE_NAMES)
MODE_ANCHOR = 0xFF  # 时间锚点条目（flags bit4）

FLAG_CHARGING = 0x01
FLAG_USB = 0x02
FLAG_SAMPLE = 0x04
FLAG_SHUTDOWN_RECOVER = 0x08
FLAG_TIME_ANCHOR = 0x10

BIN_MAGIC = b"PWRL"
BIN_HEADER_SIZE = 16
BIN_ENTRY_SIZE = 12

REGRESSION_MIN_DURATION_S = 600   # 纯电池长时段门槛：>10min 单模式
REGRESSION_MIN_POINTS = 3

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODEL_PATH = os.path.join(SCRIPT_DIR, "power_model.json")


# ---------------------------------------------------------------- 数据模型

class Entry:
    """一条日志记录（CSV 或 BIN 解析后的统一内部表示）。"""
    __slots__ = ("uptime_s", "vbat_mv", "mode", "flags")

    def __init__(self, uptime_s, vbat_mv, mode, flags):
        self.uptime_s = float(uptime_s)
        self.vbat_mv = float(vbat_mv)
        self.mode = mode      # int 0..6，或 None 表示锚点行
        self.flags = flags

    @property
    def charging(self):
        return bool(self.flags & (FLAG_CHARGING | FLAG_USB))


# ---------------------------------------------------------------- 输入解析

def parse_mode(value):
    """CSV 的 mode 列：数字（10/16 进制）或模式名；锚点返回 None。"""
    s = str(value).strip()
    if not s:
        return None
    upper = s.upper()
    if upper in ("TIME_ANCHOR", "ANCHOR", "0xFF"):
        return None
    if upper.startswith("POWER_MODE_"):
        upper = upper[len("POWER_MODE_"):]
    if upper in MODE_NAMES:
        return MODE_NAMES.index(upper)
    try:
        v = int(s, 0)
    except ValueError:
        raise ValueError(f"无法解析 mode 值: {value!r}")
    if v == MODE_ANCHOR or v < 0 or v >= MODE_COUNT:
        return None if v == MODE_ANCHOR else _unknown_mode(v)
    return v


_unknown_seen = set()


def _unknown_mode(v):
    if v not in _unknown_seen:
        _unknown_seen.add(v)
        print(f"警告: 未知 mode 值 {v}，按 UNKNOWN_{v} 处理", file=sys.stderr)
    return v  # 保留原值，MODE_NAMES 索引不到的地方走 fallback 名字


def mode_name(m):
    return MODE_NAMES[m] if 0 <= m < MODE_COUNT else f"UNKNOWN_{m}"


def _parse_flags(row):
    """优先 flags 列，否则由 charging/usb/is_* 布尔列合成（对齐 power_log_dump.py 的 CSV 列）。"""
    for key in ("flags", "flag"):
        if key in row and row[key] not in (None, ""):
            return int(str(row[key]).strip(), 0)
    flags = 0
    for key, bit in (("charging", FLAG_CHARGING), ("usb", FLAG_USB),
                     ("is_charging", FLAG_CHARGING), ("is_usb", FLAG_USB),
                     ("usb_powered", FLAG_USB), ("is_usb_powered", FLAG_USB),
                     ("is_periodic", FLAG_SAMPLE),
                     ("is_poweroff_segment", FLAG_SHUTDOWN_RECOVER),
                     ("is_time_anchor", FLAG_TIME_ANCHOR)):
        if key in row and row[key] not in (None, ""):
            v = str(row[key]).strip().lower()
            if v in ("1", "true", "yes"):
                flags |= bit
    return flags


def parse_csv_text(text):
    """解析 CSV 文本 -> (entries, meta)。'#' 开头行为注释。"""
    lines = [ln for ln in text.splitlines() if not ln.lstrip().startswith("#")]
    reader = csv.DictReader(io.StringIO("\n".join(lines)))
    if reader.fieldnames is None:
        raise ValueError("CSV 缺少表头")
    # 列名别名：内部统一用小写去空格名查找
    entries = []
    for raw in reader:
        row = {(k or "").strip().lower(): v for k, v in raw.items()}
        t = None
        for key in ("uptime_s", "relative_s", "rel_time_s", "time_s", "t_s"):
            if key in row and row[key] not in (None, ""):
                t = float(row[key])
                break
        if t is None:
            raise ValueError(f"CSV 行缺少时间列: {raw}")
        mode = None
        for key in ("mode", "mode_name", "state"):
            if key in row and row[key] not in (None, ""):
                mode = parse_mode(row[key])
                break
        flags = _parse_flags(row)
        if flags & FLAG_TIME_ANCHOR:
            mode = None
        vbat = 0.0
        for key in ("vbat_mv", "vbat", "vbat_mvavg"):
            if key in row and row[key] not in (None, ""):
                vbat = float(row[key])
                break
        entries.append(Entry(t, vbat, mode, flags))
    entries.sort(key=lambda e: e.uptime_s)
    return entries, {}


def parse_bin(data):
    """解析 PWRL 二进制 -> (entries, meta)。"""
    if len(data) < BIN_HEADER_SIZE or data[:4] != BIN_MAGIC:
        raise ValueError("不是有效的 PWRL 二进制（魔数不匹配）")
    version = data[4]
    entry_size = data[5]
    count, wrap_count = struct.unpack_from("<II", data, 8)
    if version != 1 or entry_size != BIN_ENTRY_SIZE:
        raise ValueError(f"不支持的 PWRL 版本/条目大小: v{version} entry={entry_size}")
    entries = []
    off = BIN_HEADER_SIZE
    for _ in range(count):
        if off + BIN_ENTRY_SIZE > len(data):
            break
        uptime_s, vbat_mv, mode, flags = struct.unpack_from("<IHBB", data, off)
        if flags & FLAG_TIME_ANCHOR or mode == MODE_ANCHOR:
            entries.append(Entry(uptime_s, 0.0, None, flags))
        else:
            entries.append(Entry(uptime_s, vbat_mv,
                                 mode if mode < MODE_COUNT else _unknown_mode(mode),
                                 flags))
        off += BIN_ENTRY_SIZE
    entries.sort(key=lambda e: e.uptime_s)
    return entries, {"wrap_count": wrap_count, "format": "bin"}


def load_input(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] == BIN_MAGIC:
        return parse_bin(data)
    return parse_csv_text(data.decode("utf-8-sig"))


# ---------------------------------------------------------------- 分析

def build_intervals(entries):
    """把条目序列切成 [t_i, t_i+1) 区间，归属该时刻模式；锚点行只跳过不切断。

    每条非锚点条目的 mode 字段都是「当前模式」（周期采样也如此，见设计 5.2），
    因此无需区分切换事件与采样即可归属时长。
    返回 [(t_start, t_end, mode, charging)]。
    """
    seq = [e for e in entries if e.mode is not None]
    intervals = []
    for i in range(len(seq) - 1):
        t0, t1 = seq[i].uptime_s, seq[i + 1].uptime_s
        if t1 > t0:
            intervals.append((t0, t1, seq[i].mode, seq[i].charging))
    return intervals


def aggregate_durations(intervals):
    """每模式：总时长 / 充电时长 / 纯电池时长（秒）。"""
    agg = {}  # mode -> [total, charging, battery]
    for t0, t1, mode, charging in intervals:
        dt = t1 - t0
        a = agg.setdefault(mode, [0.0, 0.0, 0.0])
        a[0] += dt
        if charging:
            a[1] += dt
        else:
            a[2] += dt
    return agg


def estimate_energy(agg, currents_ma):
    """每模式估算能耗 mAh = 纯电池时长(h) x I_mode(mA)。"""
    energy = {}
    for mode, (_, _, batt_s) in agg.items():
        energy[mode] = batt_s / 3600.0 * currents_ma.get(mode, 0.0)
    return energy


def linreg(xs, ys):
    """最小二乘回归 -> (slope, intercept)。有 numpy 用 numpy。"""
    if _HAVE_NP:
        slope, intercept = np.polyfit(np.asarray(xs), np.asarray(ys), 1)
        return float(slope), float(intercept)
    n = len(xs)
    sx = sum(xs)
    sy = sum(ys)
    sxx = sum(x * x for x in xs)
    sxy = sum(x * y for x, y in zip(xs, ys))
    denom = n * sxx - sx * sx
    if denom == 0:
        return 0.0, sy / n
    slope = (n * sxy - sx * sy) / denom
    intercept = (sy - slope * sx) / n
    return slope, intercept


def collect_battery_runs(entries):
    """收集纯电池单模式连续段（模式不变且全程未充电），用于 VBAT 回归。

    段边界：模式变化或出现充电/USB 标志。锚点行跳过。
    返回 [(mode, [(t, vbat_mv), ...])]，点含段首切换条目。
    """
    runs = []
    cur_mode = None
    cur_pts = []

    def close():
        nonlocal cur_mode, cur_pts
        if cur_mode is not None and len(cur_pts) >= 2:
            runs.append((cur_mode, cur_pts))
        cur_mode = None
        cur_pts = []

    for e in entries:
        if e.mode is None:
            continue
        if e.charging:
            close()
            continue
        if e.mode != cur_mode:
            close()
            cur_mode = e.mode
            cur_pts = [(e.uptime_s, e.vbat_mv)]
        else:
            cur_pts.append((e.uptime_s, e.vbat_mv))
    close()
    return runs


def regress_runs(runs, capacity_mah, vbat_range_mv):
    """对满足条件的长时段做 VBAT 回归，换算实测平均电流。

    线性近似：I(mA) = -dV/dt(mV/h) / vbat_range_mv * capacity_mah
    （假设该电压区间内 VBAT-SOC 近似线性，见设计第 8/9 节的精度说明）。
    返回 [{mode, duration_s, points, slope_mv_h, current_ma}]。
    """
    results = []
    for mode, pts in runs:
        duration = pts[-1][0] - pts[0][0]
        if duration < REGRESSION_MIN_DURATION_S or len(pts) < REGRESSION_MIN_POINTS:
            continue
        xs = [(t - pts[0][0]) / 3600.0 for t, _ in pts]  # 小时
        ys = [v for _, v in pts]
        slope, _ = linreg(xs, ys)  # mV/h
        if slope >= 0:
            continue  # 电压未下降（读数噪声或刚拔电），不伪造数据
        current_ma = -slope / vbat_range_mv * capacity_mah
        results.append({
            "mode": mode,
            "duration_s": duration,
            "points": len(pts),
            "slope_mv_h": slope,
            "current_ma": current_ma,
        })
    return results


def estimate_battery_life(agg, energy, capacity_mah):
    """按本次记录的使用分布估算整机续航。"""
    batt_s = sum(a[2] for a in agg.values())
    total_mah = sum(energy.values())
    if batt_s <= 0 or total_mah <= 0:
        return None
    avg_ma = total_mah / (batt_s / 3600.0)
    return {
        "battery_hours": batt_s / 3600.0,
        "total_mah": total_mah,
        "avg_ma": avg_ma,
        "runtime_h": capacity_mah / avg_ma,
    }


# ---------------------------------------------------------------- 报告渲染

def fmt_dur(seconds):
    seconds = int(round(seconds))
    h, rem = divmod(seconds, 3600)
    m, s = divmod(rem, 60)
    if h:
        return f"{h}h{m:02d}m"
    if m:
        return f"{m}m{s:02d}s"
    return f"{s}s"


def render_report(entries, meta, agg, energy, currents_ma, regressions,
                  life, model, input_name):
    total_s = sum(a[0] for a in agg.values())
    chg_s = sum(a[1] for a in agg.values())
    batt_s = sum(a[2] for a in agg.values())
    total_mah = sum(energy.values())
    span_s = (entries[-1].uptime_s - entries[0].uptime_s) if len(entries) >= 2 else 0

    out = []
    w = out.append
    w(f"# VoiceStick 分模式功耗报告")
    w("")
    w(f"- 输入：`{input_name}`")
    w(f"- 记录条目数：{len(entries)}，覆盖时长：{fmt_dur(span_s)}")
    if "wrap_count" in meta:
        w(f"- 环形回卷计数：{meta['wrap_count']}")
    w(f"- 回归方法：{'numpy.polyfit' if _HAVE_NP else '纯 Python 最小二乘'}")
    w(f"- 电池参数：{model['battery']['capacity_mah']} mAh，"
      f"VBAT 线性区间 {model['battery']['vbat_empty_mv']}–{model['battery']['vbat_full_mv']} mV")
    w("")

    # 1. 驻留时长
    w("## 1. 各模式驻留时长与占比")
    w("")
    w("| 模式 | 总时长 | 占比 | 其中充电/USB | 纯电池时长 |")
    w("|---|---:|---:|---:|---:|")
    for mode in sorted(agg, key=lambda m: -agg[m][0]):
        a = agg[mode]
        pct = a[0] / total_s * 100 if total_s else 0
        w(f"| {mode_name(mode)} | {fmt_dur(a[0])} | {pct:.1f}% | "
          f"{fmt_dur(a[1])} | {fmt_dur(a[2])} |")
    w(f"| **合计** | **{fmt_dur(total_s)}** | 100% | {fmt_dur(chg_s)} | {fmt_dur(batt_s)} |")
    w("")
    w("> 充电/USB 供电时段只计时长、不计入下方耗电归属。")
    w("")

    # 2. 估算能耗
    w("## 2. 各模式估算能耗（mAh = 纯电池时长 × 模型电流）")
    w("")
    w("| 排名 | 模式 | 模型电流 (mA) | 纯电池时长 | 估算能耗 (mAh) | 占比 |")
    w("|---:|---|---:|---:|---:|---:|")
    for rank, mode in enumerate(sorted(energy, key=lambda m: -energy[m]), 1):
        a = agg[mode]
        pct = energy[mode] / total_mah * 100 if total_mah else 0
        w(f"| {rank} | {mode_name(mode)} | {currents_ma.get(mode, 0):.1f} | "
          f"{fmt_dur(a[2])} | {energy[mode]:.2f} | {pct:.1f}% |")
    w(f"| | **合计** | | | **{total_mah:.2f}** | 100% |")
    w("")

    # 3. VBAT 回归
    w("## 3. 纯电池长时段 VBAT 回归（实测电流 vs 模型）")
    w("")
    if regressions:
        w("| 模式 | 段时长 | 采样点 | dV/dt (mV/h) | 实测电流 (mA) | 模型电流 (mA) | 偏差 |")
        w("|---|---:|---:|---:|---:|---:|---:|")
        for r in regressions:
            model_i = currents_ma.get(r["mode"], 0.0)
            dev = (r["current_ma"] - model_i) / model_i * 100 if model_i else float("nan")
            w(f"| {mode_name(r['mode'])} | {fmt_dur(r['duration_s'])} | {r['points']} | "
              f"{r['slope_mv_h']:.1f} | {r['current_ma']:.1f} | {model_i:.1f} | {dev:+.0f}% |")
        w("")
        w("> 实测电流由 VBAT 线性回归斜率按 VBAT-SOC 线性近似换算，"
          "绝对值 ±20% 量级参考（设计第 9 节）；偏差持续偏大时应按第 8 节标定并更新 power_model.json。")
    else:
        w("本次记录无满足条件的纯电池长时段（>10min 单模式、≥3 采样点、VBAT 下降），跳过。")
    w("")

    # 4. 续航估算
    w("## 4. 整机续航估算")
    w("")
    if life:
        w(f"- 本次记录纯电池时长：{life['battery_hours']:.2f} h，"
          f"估算耗电：{life['total_mah']:.2f} mAh")
        w(f"- 按此使用分布的平均电流：**{life['avg_ma']:.1f} mA**")
        w(f"- 满电（{model['battery']['capacity_mah']} mAh）估算续航：**{life['runtime_h']:.1f} 小时**"
          f"（约 {life['runtime_h'] * 60:.0f} 分钟）")
    else:
        w("纯电池数据不足，无法估算续航。")
    w("")
    w("---")
    w(f"模型电流来源：{model.get('_comment', 'power_model.json')}")
    return "\n".join(out)


def print_summary(agg, energy, currents_ma, regressions, life):
    total_s = sum(a[0] for a in agg.values())
    total_mah = sum(energy.values())
    print("=== 功耗报告摘要 ===")
    print(f"{'模式':<14}{'总时长':>10}{'占比':>7}{'纯电池':>10}{'mAh':>9}{'能耗占比':>9}")
    for mode in sorted(agg, key=lambda m: -agg[m][0]):
        a = agg[mode]
        pct = a[0] / total_s * 100 if total_s else 0
        epct = energy[mode] / total_mah * 100 if total_mah else 0
        print(f"{mode_name(mode):<14}{fmt_dur(a[0]):>10}{pct:>6.1f}%"
              f"{fmt_dur(a[2]):>10}{energy[mode]:>9.2f}{epct:>8.1f}%")
    print(f"{'合计':<14}{fmt_dur(total_s):>10}{'100%':>7}{'':>10}{total_mah:>9.2f}")
    if regressions:
        print("\n长时段回归（实测 vs 模型 mA）：")
        for r in regressions:
            print(f"  {mode_name(r['mode']):<14}{r['current_ma']:>7.1f} vs "
                  f"{currents_ma.get(r['mode'], 0):.1f}  (dV/dt {r['slope_mv_h']:.1f} mV/h)")
    if life:
        print(f"\n平均电流 {life['avg_ma']:.1f} mA，估算续航 {life['runtime_h']:.1f} h")
    print("===================")


# ---------------------------------------------------------------- 模型加载

def load_model(path):
    with open(path, "r", encoding="utf-8") as f:
        model = json.load(f)
    batt = model.get("battery", {})
    capacity = float(batt.get("capacity_mah", 200))
    v_full = float(batt.get("vbat_full_mv", 4200))
    v_empty = float(batt.get("vbat_empty_mv", 3300))
    currents = {}
    for name, spec in model.get("modes", {}).items():
        key = name.upper()
        if key.startswith("POWER_MODE_"):
            key = key[len("POWER_MODE_"):]
        if key in MODE_NAMES:
            currents[MODE_NAMES.index(key)] = float(spec["current_ma"])
    missing = [mode_name(m) for m in range(MODE_COUNT) if m not in currents]
    if missing:
        print(f"警告: power_model.json 缺少模式电流: {', '.join(missing)}（按 0 计）",
              file=sys.stderr)
    return model, currents, capacity, v_full - v_empty


# ---------------------------------------------------------------- 自测

def _synthetic_entries():
    """构造覆盖各模式 + 充电段的合成数据（uptime_s, mode, flags）。

    vbat 按模型电流实时模拟：dV = 累计耗电 mAh / 容量 × VBAT 线性区间；
    充电段 vbat 回升。返回 (entries, expected)，expected 含各模式精确时长。
    """
    cap, vrange = 200.0, 900.0  # mAh, mV（4200->3300 线性近似）
    currents = {0: 90.0, 1: 70.0, 2: 45.0, 3: 0.5, 4: 130.0, 5: 60.0, 6: 120.0}

    # 时间线：(t, mode, flags)；样本每 60s 一条。None mode = 时间锚点。
    timeline = [(0, 0, 0)]
    timeline += [(t, 0, FLAG_SAMPLE) for t in range(60, 1200, 60)]        # S0 20min
    timeline.append((600, None, FLAG_TIME_ANCHOR))                        # 锚点行应被跳过
    timeline.append((1200, 4, 0))                                         # 录音 5min
    timeline += [(t, 4, FLAG_SAMPLE) for t in range(1260, 1500, 60)]
    timeline.append((1500, 1, 0))                                         # S1
    timeline += [(t, 1, FLAG_SAMPLE) for t in range(1560, 1800, 60)]
    timeline += [(t, 1, FLAG_SAMPLE | FLAG_CHARGING) for t in (1800, 1860, 1920)]  # 充电 3min
    timeline.append((1980, 1, FLAG_SAMPLE))                               # 拔电
    timeline.append((2040, 5, 0))                                         # 广播 10min
    timeline += [(t, 5, FLAG_SAMPLE) for t in range(2100, 2640, 60)]
    timeline.append((2640, 2, 0))                                         # S2 30min（回归段）
    timeline += [(t, 2, FLAG_SAMPLE) for t in range(2700, 4440, 60)]
    timeline.append((4440, 6, 0))                                         # OTA 3min
    timeline += [(t, 6, FLAG_SAMPLE) for t in (4500, 4560)]
    timeline.append((4620, 3, 0))                                         # S3 20min
    timeline += [(t, 3, FLAG_SAMPLE) for t in range(4680, 5820, 60)]
    timeline.append((5820, 0, FLAG_SHUTDOWN_RECOVER))                     # 关机段恢复
    timeline.append((5880, 0, FLAG_SAMPLE))

    # 结算 vbat：每条目记录「该时刻」电压 = 初始 - 之前累计耗电（充电段回升）
    vbat0 = 4150.0
    drain_mah = 0.0
    charge_mv = 0.0
    out = []
    prev = None
    for t, mode, flags in timeline:
        e = Entry(t, 0, mode, flags)
        if prev is not None:
            dt_h = (e.uptime_s - prev.uptime_s) / 3600.0
            if prev.flags & (FLAG_CHARGING | FLAG_USB):
                charge_mv += 50.0 * (e.uptime_s - prev.uptime_s) / 60.0  # 每分钟 +50mV
            elif prev.mode is not None:
                drain_mah += currents[prev.mode] * dt_h
        v = min(4200.0, vbat0 - drain_mah / cap * vrange + charge_mv)
        if e.mode is not None:
            e.vbat_mv = round(v)
        out.append(e)
        prev = e

    expected_dur = {  # 精确手算：[total, charging, battery]（秒）
        0: [1260, 0, 1260],    # 0..1200 + 5820..5880
        1: [540, 180, 360],    # 1500..2040，其中 1800..1980 充电
        2: [1800, 0, 1800],    # 2640..4440
        3: [1200, 0, 1200],    # 4620..5820
        4: [300, 0, 300],      # 1200..1500
        5: [600, 0, 600],      # 2040..2640
        6: [180, 0, 180],      # 4440..4620
    }
    return out, expected_dur, currents


def _entries_to_csv(entries):
    lines = ["uptime_s,wall_time,mode,vbat_mv,charging,usb"]
    for e in entries:
        if e.mode is None:
            lines.append(f"{int(e.uptime_s)},,TIME_ANCHOR,0,0,0")
        else:
            chg = 1 if e.flags & FLAG_CHARGING else 0
            usb = 1 if e.flags & FLAG_USB else 0
            lines.append(f"{int(e.uptime_s)},{int(e.uptime_s)},{e.mode},"
                         f"{int(e.vbat_mv)},{chg},{usb}")
    return "\n".join(lines) + "\n"


def _entries_to_bin(entries):
    data = bytearray()
    data += BIN_MAGIC
    data += bytes([1, BIN_ENTRY_SIZE, 0, 0])
    data += struct.pack("<II", len(entries), 0)
    for e in entries:
        mode = MODE_ANCHOR if e.mode is None else e.mode
        data += struct.pack("<IHBB4s", int(e.uptime_s), int(e.vbat_mv),
                            mode, e.flags, b"\x00" * 4)
    return bytes(data)


def self_test():
    entries, expected_dur, currents = _synthetic_entries()
    failures = []

    def check(name, cond, detail=""):
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail else ''}")
        if not cond:
            failures.append(name)

    print("== power_report 自测 ==")

    # CSV 与 BIN 解析一致性
    csv_entries, _ = parse_csv_text(_entries_to_csv(entries))
    bin_entries, meta = parse_bin(_entries_to_bin(entries))
    check("CSV/BIN 条目数一致", len(csv_entries) == len(bin_entries) == len(entries),
          f"csv={len(csv_entries)} bin={len(bin_entries)} src={len(entries)}")
    iv_csv = build_intervals(csv_entries)
    iv_bin = build_intervals(bin_entries)
    check("CSV/BIN 区间一致",
          [(round(a), round(b), m, c) for a, b, m, c in iv_csv]
          == [(round(a), round(b), m, c) for a, b, m, c in iv_bin])

    # 时长归属（含充电段剔除）
    agg = aggregate_durations(iv_csv)
    for mode, (tot, chg, batt) in expected_dur.items():
        a = agg.get(mode, [0, 0, 0])
        check(f"{mode_name(mode)} 时长 total={tot} chg={chg} batt={batt}",
              abs(a[0] - tot) < 1e-6 and abs(a[1] - chg) < 1e-6 and abs(a[2] - batt) < 1e-6,
              f"got {a}")

    # 能耗 = 纯电池时长 × 模型电流
    energy = estimate_energy(agg, currents)
    for mode, (_, _, batt) in expected_dur.items():
        exp = batt / 3600.0 * currents[mode]
        check(f"{mode_name(mode)} 能耗 {exp:.3f} mAh",
              abs(energy[mode] - exp) < 1e-6, f"got {energy[mode]:.4f}")
    check("充电段不计入能耗（S1 充电 180s 无 mAh）",
          abs(energy[1] - 360 / 3600.0 * 70.0) < 1e-6)

    # 长时段 VBAT 回归：S0(90mA) / S2(45mA) 段应恢复出近似电流
    runs = collect_battery_runs(csv_entries)
    regs = regress_runs(runs, 200.0, 900.0)
    reg_by_mode = {r["mode"]: r for r in regs}
    check("回归段包含 S0 与 S2", 0 in reg_by_mode and 2 in reg_by_mode,
          f"got modes {sorted(reg_by_mode)}")
    for mode, tol in ((0, 0.05), (2, 0.05)):
        if mode in reg_by_mode:
            got = reg_by_mode[mode]["current_ma"]
            exp = currents[mode]
            check(f"{mode_name(mode)} 回归电流≈{exp}mA (±{tol * 100:.0f}%)",
                  abs(got - exp) / exp < tol, f"got {got:.2f}")

    # 续航估算
    life = estimate_battery_life(agg, energy, 200.0)
    batt_h = sum(v[2] for v in expected_dur.values()) / 3600.0
    tot_mah = sum(v[2] / 3600.0 * currents[m] for m, v in expected_dur.items())
    exp_avg = tot_mah / batt_h
    check("续航估算平均电流", life and abs(life["avg_ma"] - exp_avg) < 1e-6,
          f"got {life and life['avg_ma']:.4f} exp {exp_avg:.4f}")
    check("续航 = 容量/平均电流",
          life and abs(life["runtime_h"] - 200.0 / exp_avg) < 1e-6)

    # 端到端：走完整 render 路径不抛异常
    model = {"battery": {"capacity_mah": 200, "vbat_full_mv": 4200,
                         "vbat_empty_mv": 3300}, "_comment": "self-test"}
    md = render_report(csv_entries, {}, agg, energy, currents, regs, life, model,
                       "synthetic.csv")
    check("Markdown 渲染包含四个章节",
          all(s in md for s in ("## 1.", "## 2.", "## 3.", "## 4.")))

    # 纯 Python 与 numpy 回归一致性（有 numpy 时交叉验证）
    if _HAVE_NP:
        xs = [0.0, 1.0, 2.0, 3.0]
        ys = [4000.0, 3900.0, 3800.0, 3700.0]
        s_np, _ = linreg(xs, ys)
        check("numpy 回归斜率 -100", abs(s_np - (-100.0)) < 1e-9, f"got {s_np}")

    if failures:
        print(f"\n自测失败 {len(failures)} 项: {failures}")
        return 1
    print("\n自测全部通过")
    return 0


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description="StickS3 分模式功耗分析报告")
    ap.add_argument("input", nargs="?", help="power_log_dump.py 产出的 CSV 或 PWRL bin")
    ap.add_argument("--model", default=DEFAULT_MODEL_PATH,
                    help=f"power_model.json 路径（默认 {DEFAULT_MODEL_PATH}）")
    ap.add_argument("--out", help="Markdown 报告输出路径（默认 <输入名>_report.md）")
    ap.add_argument("--self-test", action="store_true", help="运行合成数据自测")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.input:
        ap.error("缺少输入文件（或 --self-test）")

    model, currents, capacity, vrange = load_model(args.model)
    entries, meta = load_input(args.input)
    if len(entries) < 2:
        print("错误: 有效条目不足 2 条，无法分析", file=sys.stderr)
        return 1

    intervals = build_intervals(entries)
    agg = aggregate_durations(intervals)
    energy = estimate_energy(agg, currents)
    regs = regress_runs(collect_battery_runs(entries), capacity, vrange)
    life = estimate_battery_life(agg, energy, capacity)

    md = render_report(entries, meta, agg, energy, currents, regs, life, model,
                       os.path.basename(args.input))
    out_path = args.out or os.path.splitext(args.input)[0] + "_report.md"
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(md + "\n")
    print_summary(agg, energy, currents, regs, life)
    print(f"Markdown 报告已写入: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
