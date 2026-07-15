"""读取 ESP32 USB JTAG 串口日志（COM 口），用于排查固件运行时行为。

用法：python read_serial.py [COM_PORT] [duration_seconds]
默认 COM17，8 秒。
"""
import sys
import time

try:
    import serial
except ImportError:
    print("pyserial 未安装", file=sys.stderr)
    sys.exit(2)

port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

try:
    s = serial.Serial(port, 115200, timeout=1)
except Exception as e:
    print(f"open {port} failed: {e}", file=sys.stderr)
    sys.exit(1)

print(f"=== reading {port} for {dur}s ===")
end = time.time() + dur
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", errors="replace"))
        sys.stdout.flush()
s.close()
print(f"\n=== done ===")
