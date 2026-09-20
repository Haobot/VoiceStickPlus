#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""P1 网关切换器验收harness（真机）。

把设计稿 §4 的验收口径做成可重复执行的一条命令：采集设备串口日志 + 解析 app 日志，
逐项给 PASS/FAIL。需要人手的两项（侧键切换器、非目标拒绝）以交互提示方式引导操作者完成。

用法：
  # 全量（含侧键切换器手操，会提示你按侧键）
  python scripts/e2e_test/gateway_switch_acceptance.py --port COM19 --address 70:04:1D:DC:53:AA

  # 只跑自动化部分（切换 soak），不提示手操
  python scripts/e2e_test/gateway_switch_acceptance.py --rounds 5 --skip-manual

前置：VoiceStick.exe 已运行且设备已连接（app 日志出现 stage=ready）。

验收项与判据（来源 Doc/Plan/xiaomi-gateway-p1-switcher.md §4）：
  1. 设备侧切换 ≤2s             —— 串口日志 "gateway_select_target" → "accept_peer (state=connected)"
  2. 目标机 app 恢复（记录值）    —— app 日志 "device disconnected" → "stage=ready"
  3. 切换期间小米链路零断开       —— 切换窗口内无新的 gw_hid/gw_atvv 行
  4. 订阅时 MTU 截断告警为 0      —— 无 "exceeds notify budget"
  5. 侧键切换器（人手）           —— 串口日志出现"侧键预览目标/侧键切换"、切换后 accept_peer
"""
import argparse
import datetime as dt
import os
import re
import subprocess
import sys
import threading
import time

try:
    import serial
except ImportError:
    print("需要 pyserial: pip install pyserial")
    sys.exit(2)

EXE = os.path.join("desktop", "windows", "build-x64", "VoiceStick.exe")
DEFAULT_PORT = "COM19"
DEFAULT_ADDRESS = "70:04:1D:DC:53:AA"

TS_RE = re.compile(r"^\S+ \((\d+)\) ")
APP_TS_RE = re.compile(r"^\[\w+ (\d\d:\d\d:\d\d\.\d+)\]")


class SerialTap:
    """后台串口采集：显式保持 DTR/RTS 低，避免 USB-Serial-JTAG 把跳变当复位。"""

    def __init__(self, port, baud=115200, reset=False):
        self.port = port
        self.baud = baud
        self.reset = reset
        self.lines = []
        self._stop = False
        self._thread = None

    def start(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2, dsrdtr=False, rtscts=False)
        if self.reset:
            self.ser.dtr = False
            time.sleep(0.1)
            self.ser.dtr = True
            time.sleep(0.3)
            self.ser.dtr = False
        else:
            self.ser.dtr = False
            self.ser.rts = False

        def run():
            buf = b""
            while not self._stop:
                try:
                    data = self.ser.read(512)
                except Exception:
                    break
                if not data:
                    continue
                buf += data
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    self.lines.append(raw.decode("utf-8", errors="replace").rstrip("\r"))
        self._thread = threading.Thread(target=run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop = True
        if self._thread:
            self._thread.join(timeout=2)
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.ser.close()


def log(text):
    print(text, flush=True)


def app_log_path():
    base = os.environ.get("LOCALAPPDATA", "")
    return os.path.join(base, "VoiceStick", "VoiceStickApp.log")


def read_app_tail(n=4000):
    path = app_log_path()
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.readlines()[-n:]


def app_last_timestamp():
    """app 日志最后一行的时刻（用于切分本轮新增行；按行数切片会因窗口滚动而错位）。"""
    lines = read_app_tail()
    for line in reversed(lines):
        m = APP_TS_RE.match(line)
        if m:
            return m.group(1)
    return None


def wait_for_ready(timeout_s=45.0):
    """等 app 与设备重新连上（打开串口可能让设备复位，必须等 stage=ready 再开始）。

    判据：最后一条 stage=ready 出现在最后一条 device disconnected 之后。
    """
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        lines = read_app_tail(400)
        last_ready = last_disc = -1
        for i, line in enumerate(lines):
            if "stage=ready" in line:
                last_ready = i
            elif "device disconnected" in line:
                last_disc = i
        if last_ready > last_disc and last_ready >= 0:
            return True
        time.sleep(1.0)
    return False


def app_lines_since(mark):
    if not mark:
        return read_app_tail()
    out = []
    for line in read_app_tail():
        m = APP_TS_RE.match(line)
        if m and m.group(1) > mark:
            out.append(line)
    return out


# 小米链路「有事件」的判据：只在链路建立/断开时才出现的行；开机时的 CCCD 枚举等噪声不算。
XIAOMI_LINK_RE = ("已连接小米", "链路就绪", "断开", "disc_all_svcs", "重新连接", "小米 central 链路启动")


def send(exe, *args):
    """调用 VoiceStick.exe 子命令（转发给已运行实例）。"""
    cmd = [exe] + list(args)
    try:
        subprocess.run(cmd, cwd=os.getcwd(), timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception as exc:  # noqa: BLE001
        log(f"  !! 调用 {cmd} 失败: {exc}")
        return False


def parse_device_events(lines):
    """从串口行里抽 (ts_ms, text)。"""
    out = []
    for line in lines:
        m = TS_RE.match(line)
        if m:
            out.append((int(m.group(1)), line))
    return out


def run_switch_rounds(exe, rounds, pause_s, tap, app_mark):
    log(f"\n=== 阶段 1：桌面端驱动切换 {rounds} 轮 ===")
    results = []
    for i in range(rounds):
        log(f"-- 第 {i + 1}/{rounds} 轮：clear → self")
        send(exe, "--gateway-target", "clear")
        time.sleep(2)
        app_mark = app_last_timestamp()
        before_dev = len(tap.lines)
        send(exe, "--gateway-target", "self")
        time.sleep(pause_s)
        dev = parse_device_events(tap.lines[before_dev:])
        sel = next((ts for ts, l in dev if "self -> #0" in l), None)
        acc = next((ts for ts, l in dev if "accept_peer (state=connected)" in l), None)
        zone = [l for _, l in dev
                if ("gw_hid" in l or "gw_atvv" in l)
                and any(tok in l for tok in XIAOMI_LINK_RE)]
        trunc = [l for _, l in dev if "exceeds notify budget" in l]
        app_new = app_lines_since(app_mark)
        disc = next((l for l in app_new if "device disconnected" in l and "restarting scan" in l), None)
        ready = next((l for l in app_new if "stage=ready" in l), None)
        rec = None
        if disc and ready:
            try:
                rec = (dt.datetime.strptime(APP_TS_RE.match(ready).group(1), "%H:%M:%S.%f")
                       - dt.datetime.strptime(APP_TS_RE.match(disc).group(1), "%H:%M:%S.%f")).total_seconds()
            except Exception:
                rec = None
        results.append({
            "round": i + 1,
            "switch_ms": (acc - sel) if (sel is not None and acc is not None) else None,
            "recovery_s": rec,
            "xiaomi_events": len(zone),
            "truncation": len(trunc),
        })
        log(f"   切换={results[-1]['switch_ms']}ms  恢复={results[-1]['recovery_s']}s  "
            f"小米链路事件={len(zone)}  截断告警={len(trunc)}")
    return results


def report(results, manual_findings):
    log("\n=== 验收结果 ===")
    switches = [r["switch_ms"] for r in results if r["switch_ms"] is not None]
    recoveries = [r["recovery_s"] for r in results if r["recovery_s"] is not None]
    xiaomi = sum(r["xiaomi_events"] for r in results)
    trunc = sum(r["truncation"] for r in results)

    def verdict(ok, name, detail):
        log(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}")

    if switches:
        verdict(max(switches) <= 2000, "设备侧切换 ≤2s",
                f"{len(switches)}/{len(results)} 轮成功 avg={int(sum(switches) / len(switches))}ms "
                f"worst={max(switches)}ms")
    else:
        verdict(False, "设备侧切换 ≤2s", "本轮没抓到任何切换（设备未就绪或未发生切换）")
    verdict(xiaomi == 0, "切换期间小米链路零断开", f"切换窗口内 gw_hid/gw_atvv 事件 {xiaomi} 次")
    verdict(trunc == 0, "订阅时 MTU 截断告警 0", f"{trunc} 次")
    if recoveries:
        log(f"[INFO] 目标机 app 恢复（记录，无硬门槛）: "
            f"{', '.join(f'{r:.2f}s' for r in recoveries)}（修复前同口径 14–26s）")
    for item, ok, detail in manual_findings:
        verdict(ok, item, detail)


def manual_phase(tap):
    """人手阶段：侧键切换器。返回 [(项, 是否通过, 说明)]。"""
    log("\n=== 阶段 2：侧键切换器手操（需要你动手） ===")
    log("请在下面倒计时内完成：")
    log("  1) 短按侧键一次      → 屏幕应显示当前目标名（前缀 '>'）")
    log("  2) 3 秒内再短按一次  → 应轮流切换到下一个目标")
    for i in range(12, 0, -1):
        log(f"  ... {i}")
        time.sleep(1)
    dev = parse_device_events(tap.lines)
    preview = [l for _, l in dev if "侧键预览目标" in l]
    cycled = [l for _, l in dev if "侧键切换" in l]
    accepted = [l for _, l in dev if "accept_peer (state=connected)" in l]
    findings = [
        ("侧键短按预览当前目标", bool(preview), f"'侧键预览目标' {len(preview)} 次"),
        ("3s 内再短按切换", bool(cycled) and bool(accepted),
         f"'侧键切换' {len(cycled)} 次 / accept {len(accepted)} 次"),
    ]
    return findings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--address", default=DEFAULT_ADDRESS)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--pause", type=float, default=14.0)
    ap.add_argument("--skip-manual", action="store_true")
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    exe = os.path.abspath(args.exe)
    if not os.path.exists(exe):
        log(f"找不到 {exe}")
        return 2

    log(f"采集串口 {args.port}（不扰动设备）…")
    tap = SerialTap(args.port)
    tap.start()
    time.sleep(1.5)

    if not wait_for_ready():
        log("!! 等待 app 与设备就绪超时（app 日志未见 stage=ready）；请确认 VoiceStick.exe 已连上设备")
        tap.stop()
        return 3
    log("app 已就绪（stage=ready）")

    results = run_switch_rounds(exe, args.rounds, args.pause, tap, None)
    manual = [] if args.skip_manual else manual_phase(tap)
    tap.stop()
    report(results, manual)
    log("\n提示：非目标拒绝需要第二个 identity address（第二台机器/第二个适配器），本 harness 覆盖不到。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
