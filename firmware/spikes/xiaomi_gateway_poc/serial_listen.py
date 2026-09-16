#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""spike 串口日志采集：后台监听 COM19，写 build/spike_log.txt（带相对时间戳）。

用法：python serial_listen.py [COM口号] [秒数，默认 1800]
按 Ctrl+C 或超时自动退出。供 Phase 0 spike 真机验证采证。
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM19"
DURATION = int(sys.argv[2]) if len(sys.argv) > 2 else 1800
OUT = __file__.rsplit("\\", 1)[0] + "\\build\\spike_log.txt"

t0 = time.time()
with serial.Serial(PORT, 115200, timeout=0.5) as s, open(OUT, "ab") as f:
    print(f"listening {PORT} -> {OUT} for {DURATION}s", flush=True)
    while time.time() - t0 < DURATION:
        data = s.read(4096)
        if data:
            stamp = f"[{time.time() - t0:7.1f}s] ".encode()
            f.write(stamp + data)
            f.flush()
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
print("done", flush=True)
