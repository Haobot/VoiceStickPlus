#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""spike 串口日志采集（自动重连版）。

设备重启时 USB JTAG 重枚举会杀掉旧监听（ClearCommError 异常），
本版捕获异常后循环重开串口直至总时长用完，保证日志连续。

用法：python serial_listen.py [COM口号] [秒数，默认 2400]
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM19"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
OUT = __file__.rsplit("\\", 1)[0] + "\\build\\spike_log.txt"

t0 = time.time()
last_notice = 0.0
while time.time() - t0 < DURATION:
    try:
        with serial.Serial(PORT, 115200, timeout=0.5) as s, open(OUT, "ab") as f:
            print(f"listening {PORT} -> {OUT}", flush=True)
            last_notice = 0.0
            while time.time() - t0 < DURATION:
                data = s.read(4096)
                if data:
                    stamp = f"[{time.time() - t0:7.1f}s] ".encode()
                    f.write(stamp + data)
                    f.flush()
                    sys.stdout.write(data.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
    except serial.SerialException as e:
        now = time.time() - t0
        if now - last_notice > 5:
            print(f"\n[监听] 串口异常（{e.__class__.__name__}），2s 后重开…", flush=True)
            last_notice = now
        time.sleep(2)
print("done", flush=True)
