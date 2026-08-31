#!/usr/bin/env python3
"""临时诊断：抓取 ESP32-S3 USB-JTAG 串口运行时日志 N 秒，写入文件。

用法：python capture_serial_log.py COM6 180 serial_capture.log
注意：打开后立即拉低 DTR/RTS，避免触发 USB-JTAG 复位陷阱。
"""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    port, duration, out_path = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    ser = serial.Serial(port, 115200, timeout=0.5)
    ser.dtr = False
    ser.rts = False
    print(f"capturing {port} @115200 for {duration:.0f}s -> {out_path}")
    deadline = time.time() + duration
    count = 0
    with open(out_path, "w", encoding="utf-8", errors="replace") as out:
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                out.write(text)
                out.flush()
                count += len(chunk)
    ser.close()
    print(f"done, {count} bytes captured")
    return 0


if __name__ == "__main__":
    sys.exit(main())
