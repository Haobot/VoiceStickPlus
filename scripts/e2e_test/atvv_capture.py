# 小米蓝牙遥控器 2 Pro ATVV 语音会话采集工具（E2E golden 数据抓取）。
# 基于 bleak 直连遥控器 ATVV GATT 服务，应答 MIC_OPEN，接收裸 IMA/DVI ADPCM
# 音频流，按会话落盘原始 ADPCM 与解码后的 16kHz mono WAV，供后续单元测试
# 与 ASR 离线评测使用。
#
# 用法：
#   python scripts/e2e_test/atvv_capture.py [--address ADDR | --name NAME]
#       [--out-dir DIR] [--gain-db 0.0] [--duration 60] [--max-sessions 3]
#   python scripts/e2e_test/atvv_capture.py --self-test   # 纯逻辑自测，不连设备
#
# 注意：
#   - 依赖 bleak（未列入根 requirements.txt，需自行 pip install bleak）。
#   - 设备通常需先在 Windows 系统蓝牙设置里完成配对（Bond）才能连接。
#   - --self-test 不依赖 bleak，可在无设备/无 bleak 环境运行。
import argparse
import asyncio
import array
import ctypes
import json
import math
import struct
import sys
import time
import wave
from pathlib import Path

# ---- ATVV GATT 定义 -------------------------------------------------------
ATVV_SERVICE_UUID = "AB5E0001-5A21-4F05-BC7D-AF01F617B664"
ATVV_TX_UUID = "AB5E0002-5A21-4F05-BC7D-AF01F617B664"      # 主机写命令（write without response）
ATVV_AUDIO_UUID = "AB5E0003-5A21-4F05-BC7D-AF01F617B664"   # notify：裸 ADPCM 字节流
ATVV_CONTROL_UUID = "AB5E0004-5A21-4F05-BC7D-AF01F617B664"  # notify：1 字节 opcode + 参数

# Control opcode
OP_STOP = 0x00
OP_STREAM_START = 0x04
OP_MIC_OPEN = 0x08
OP_AUDIO_SYNC = 0x0A
OP_CAPS = 0x0B

# TX 命令
CMD_GET_CAPS = bytes([0x0A, 0x01, 0x00, 0x00, 0x03, 0x03])  # GET_CAPS v1.0
CMD_MIC_OPEN_ACK = 0x0C
CMD_MIC_CLOSE = 0x0D

CODEC_8K = 0x01
CODEC_16K = 0x02
DEFAULT_FRAME_LEN = 120       # CAPS 帧长为 0 时的默认值（120 字节 = 240 采样/帧）
TAIL_WINDOW_S = 0.150         # STOP 之后仍接收音频尾包的时间窗
SCAN_TIMEOUT_S = 15.0

# 设备名白名单（trim + 小写比较）；或广播数据包含 ATVV service UUID
NAME_WHITELIST = (
    "mi rc",
    "xiaomi bluetooth remote 2 pro",
    "小米蓝牙语音遥控器",
    "rc001",
    "rc003",
)


class F5Suppressor:
    """采集期间用 WH_KEYBOARD_LL 低级键盘钩子吞掉 F5。

    小米遥控器 2 Pro 的语音键会经 OS 级 HID 通道连带发 F5（桌面端的 F5
    抑制在 VoiceStick.exe 里，而采集时主程序必须退出让出 BLE），故采集
    进程内做同样抑制，避免按语音键刷新前台浏览器/窗口。非 Windows 或
    钩子安装失败时降级为 no-op。
    """

    _WH_KEYBOARD_LL = 13
    _VK_F5 = 0x74
    _KEY_MSGS = (0x0100, 0x0101, 0x0104, 0x0105)  # KEYDOWN/KEYUP/SYSKEYDOWN/SYSKEYUP
    _WM_QUIT = 0x0012

    def __init__(self) -> None:
        self._hook = None
        self._thread = None
        self._tid = None
        self._ready = None
        self._proc_ref = None  # 保持回调引用，防 GC 回收后系统回调野指针

    def start(self) -> bool:
        if sys.platform != "win32":
            return False
        import threading
        self._ready = threading.Event()
        self._thread = threading.Thread(target=self._hook_thread, daemon=True)
        self._thread.start()
        self._ready.wait(timeout=2.0)
        ok = self._hook is not None
        print(f"{ts()} F5 suppress: {'on (capture 期间 F5 被吞)' if ok else 'unavailable, ignored'}",
              flush=True)
        return ok

    def stop(self) -> None:
        if self._tid is not None:
            try:
                import ctypes
                ctypes.windll.user32.PostThreadMessageW(self._tid, self._WM_QUIT, 0, 0)
            except Exception:
                pass
        if self._thread is not None:
            self._thread.join(timeout=2.0)

    def _hook_thread(self) -> None:
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        # 必须显式声明 64 位签名：ctypes 默认 restype=c_int 会把 HMODULE/HHOOK
        # 截成 32 位，SetWindowsHookExW 随即报 ERROR_MOD_NOT_FOUND(126) 静默
        # 装不上（探针实测：声明签名后安装成功且回调正常投递）。
        kernel32.GetModuleHandleW.restype = wintypes.HMODULE
        kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
        self._tid = kernel32.GetCurrentThreadId()
        try:
            class KBDLLHOOKSTRUCT(ctypes.Structure):
                _fields_ = [("vkCode", wintypes.DWORD),
                            ("scanCode", wintypes.DWORD),
                            ("flags", wintypes.DWORD),
                            ("time", wintypes.DWORD),
                            ("dwExtraInfo", ctypes.c_size_t)]

            HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int,
                                          wintypes.WPARAM, wintypes.LPARAM)
            user32.SetWindowsHookExW.restype = ctypes.c_void_p
            user32.SetWindowsHookExW.argtypes = [ctypes.c_int, ctypes.c_void_p,
                                                 wintypes.HMODULE, wintypes.DWORD]
            # CallNextHookEx 同样要声明 64 位签名：默认把 LPARAM 当 32 位 int，
            # 64 位指针值直接 OverflowError（回调内抛异常，按键事件泛滥时刷爆日志）。
            user32.CallNextHookEx.restype = ctypes.c_ssize_t
            user32.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                              wintypes.WPARAM, wintypes.LPARAM]

            def _proc(nCode, wParam, lParam):
                if nCode == 0 and wParam in self._KEY_MSGS:
                    vk = ctypes.cast(
                        lParam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents.vkCode
                    if vk == self._VK_F5:
                        return 1  # 吞掉，不再传给后续钩子与前台应用
                return user32.CallNextHookEx(None, nCode, wParam, lParam)

            self._proc_ref = HOOKPROC(_proc)
            hook = user32.SetWindowsHookExW(self._WH_KEYBOARD_LL, self._proc_ref,
                                            kernel32.GetModuleHandleW(None), 0)
        except Exception:
            hook = None
        self._hook = hook
        self._ready.set()
        if not hook:
            return
        # 低级钩子要求安装线程 pump 消息，否则回调不会被调用
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        user32.UnhookWindowsHookEx(ctypes.c_void_p(hook))


def toast(title: str, text: str) -> None:
    """Win32 信息弹窗（守护线程里弹模态框，不阻塞 BLE 回调）。

    采集器跑在后台无控制台反馈，用弹窗告诉用户采集器状态；非 Windows
    降级为 print。"""
    if sys.platform != "win32":
        print(f"{ts()} [toast] {title}: {text}", flush=True)
        return
    import threading
    threading.Thread(
        target=lambda: ctypes.windll.user32.MessageBoxW(None, text, title, 0x40),
        daemon=True).start()

# IMA/DVI ADPCM 公开标准常数（1992 年标准）
STEP_TABLE = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_TABLE = (-1, -1, -1, -1, 2, 4, 6, 8)

T0 = time.monotonic()


def ts() -> str:
    return f"{time.monotonic() - T0:7.2f}s"


def mono_ts() -> float:
    return round(time.monotonic() - T0, 3)


# ---- 纯逻辑：ADPCM 解码 / 帧累积 / CAPS 解析 / 后处理 ----------------------
class ImaAdpcmDecoder:
    """IMA/DVI ADPCM 解码器，4bit/采样，每字节高半字节先解码。"""

    def __init__(self) -> None:
        self.reset(0, 0)

    def reset(self, predictor: int = 0, step_index: int = 0) -> None:
        self.predictor = max(-32768, min(32767, predictor))
        self.step_index = max(0, min(88, step_index))

    def decode_nibble(self, nibble: int) -> int:
        step = STEP_TABLE[self.step_index]
        difference = step >> 3
        if nibble & 1:
            difference += step >> 2
        if nibble & 2:
            difference += step >> 1
        if nibble & 4:
            difference += step
        if nibble & 8:
            self.predictor -= difference
        else:
            self.predictor += difference
        self.predictor = max(-32768, min(32767, self.predictor))
        self.step_index += INDEX_TABLE[nibble & 7]
        self.step_index = max(0, min(88, self.step_index))
        return self.predictor

    def decode(self, data: bytes) -> list:
        out = []
        for b in data:
            out.append(self.decode_nibble(b >> 4))
            out.append(self.decode_nibble(b & 0x0F))
        return out


def ima_encode(pcm: list) -> tuple:
    """IMA/DVI ADPCM 编码器（公开标准算法，高半字节优先打包），供合成
    golden fixtures 与自测使用。返回 (encoded_bytes, expected_decoded)，
    expected_decoded 是编码器内部 predictor 轨迹（即标准解码的期望输出）。
    样本数为奇数时末尾补 0 低半字节（会多解出一个样本），调用方自行截断。"""
    encoded = bytearray()
    expected = []
    predictor = 0
    step_index = 0
    high = None
    for sample in pcm:
        step = STEP_TABLE[step_index]
        nibble = 0
        diff = step >> 3
        delta = sample - predictor
        if delta < 0:
            nibble = 8
            delta = -delta
        if delta >= step:
            nibble |= 4
            delta -= step
            diff += step
        if delta >= (step >> 1):
            nibble |= 2
            delta -= step >> 1
            diff += step >> 1
        if delta >= (step >> 2):
            nibble |= 1
            diff += step >> 2
        predictor = predictor - diff if (nibble & 8) else predictor + diff
        predictor = max(-32768, min(32767, predictor))
        step_index = max(0, min(88, step_index + INDEX_TABLE[nibble & 7]))
        expected.append(predictor)
        if high is None:
            high = nibble
        else:
            encoded.append((high << 4) | nibble)
            high = None
    if high is not None:
        encoded.append(high << 4)
    return bytes(encoded), expected


class FrameAccumulator:
    """Audio notify 是无帧头无序号的裸字节流，按协商帧长跨 notify 累积切帧。"""

    def __init__(self, frame_len: int = DEFAULT_FRAME_LEN) -> None:
        self.frame_len = frame_len
        self._buf = bytearray()

    def push(self, data: bytes) -> list:
        self._buf.extend(data)
        frames = []
        while len(self._buf) >= self.frame_len:
            frames.append(bytes(self._buf[: self.frame_len]))
            del self._buf[: self.frame_len]
        return frames

    def pending(self) -> int:
        return len(self._buf)

    def take_pending(self) -> bytes:
        data = bytes(self._buf)
        self._buf.clear()
        return data

    def clear(self) -> None:
        self._buf.clear()


def parse_caps(data: bytes) -> dict:
    """解析 CAPS 应答（0x0B）。返回 dict(version/codecs/interaction/frame_len/legacy)。

    v>=1.0：bytes[1:3] 版本大端；bytes[3] codec 位掩码；bytes[4] interaction；
            bytes[5:7] 帧长大端（0 -> 默认 120）。
    兼容分支：v>=1.0 但 codecs==0 且 len>=9 且 bytes[4]&0x03 != 0，
              按旧布局取 codecs=bytes[4]，interaction=0x03。
    v<1.0（旧版）：需 len>=9，codecs=bytes[4]，interaction=0。
    旧布局未定义帧长字段，一律用默认 120。
    """
    if len(data) < 3 or data[0] != OP_CAPS:
        raise ValueError(f"not a CAPS frame: {data.hex()}")
    version = (data[1] << 8) | data[2]
    frame_len = DEFAULT_FRAME_LEN
    legacy = False
    if version >= 0x0100:
        if len(data) < 5:
            raise ValueError(f"CAPS v1.0 frame too short: len={len(data)}")
        codecs = data[3]
        if codecs == 0 and len(data) >= 9 and (data[4] & 0x03) != 0:
            legacy = True
            codecs = data[4]
            interaction = 0x03
        else:
            interaction = data[4]
            if len(data) >= 7:
                fl = (data[5] << 8) | data[6]
                if fl:
                    frame_len = fl
    else:
        if len(data) < 9:
            raise ValueError(f"CAPS legacy frame too short: len={len(data)}")
        legacy = True
        codecs = data[4]
        interaction = 0
    return {
        "version": version,
        "codecs": codecs,
        "interaction": interaction,
        "frame_len": frame_len,
        "legacy": legacy,
    }


def smooth3(samples: list) -> list:
    """三点平滑：out[i]=(in[i-1]+2*in[i]+in[i+1])>>2，首尾不动。"""
    n = len(samples)
    if n < 3:
        return list(samples)
    out = [samples[0]]
    for i in range(1, n - 1):
        out.append((samples[i - 1] + 2 * samples[i] + samples[i + 1]) >> 2)
    out.append(samples[n - 1])
    return out


def apply_gain(samples: list, gain_db: float) -> list:
    """增益（gain_db 钳位到 ±24dB），int16 限幅。
    舍入用 half-away-from-zero，与 C++ std::lround 逐样本对齐（golden 对拍要求）。"""
    gain_db = max(-24.0, min(24.0, gain_db))
    if gain_db == 0.0:
        return [max(-32768, min(32767, s)) for s in samples]
    factor = math.pow(10.0, gain_db / 20.0)

    def lround(v: float) -> int:
        return int(math.floor(v + 0.5)) if v >= 0 else -int(math.floor(-v + 0.5))

    return [max(-32768, min(32767, lround(s * factor))) for s in samples]


def write_wav(path: Path, samples: list, sample_rate: int) -> None:
    pcm = array.array("h", samples)
    if sys.byteorder == "big":
        pcm.byteswap()
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm.tobytes())


def read_wav_samples(path: Path) -> list:
    """读取 16bit mono WAV 为 int16 样本列表（仅供测试/评测）。"""
    with wave.open(str(path), "rb") as wf:
        assert wf.getsampwidth() == 2 and wf.getnchannels() == 1
        data = wf.readframes(wf.getnframes())
    pcm = array.array("h")
    pcm.frombytes(data)
    if sys.byteorder == "big":
        pcm.byteswap()
    return list(pcm)


# ---- 采集器 ---------------------------------------------------------------
class AtvvCapture:
    def __init__(self, args) -> None:
        self.args = args
        self.out_dir = Path(args.out_dir)
        self.out_dir.mkdir(parents=True, exist_ok=True)
        self._events_fp = open(self.out_dir / "events.jsonl", "w", encoding="utf-8")

        self.client = None
        self.connected = False
        self.caps = None               # parse_caps 结果
        self.version = 0               # 协议版本（大端 uint16，如 0x0100）
        self.chosen_codec = CODEC_16K

        self.decoder = ImaAdpcmDecoder()
        self.accum = FrameAccumulator()

        self.session_no = 0
        self.session_state = "idle"    # idle / open / stopping
        self.session_id = 0
        self.session_codec = CODEC_16K
        self.session_rate = 16000
        self.adpcm_chunks = []         # 原始 ADPCM 字节（raw 流）
        self.pcm_samples = []          # 解码后的 PCM（未做平滑/增益）
        self.frame_count = 0
        self.stop_ts = 0.0
        self.mic_open_ts = None
        self.first_audio_ts = None
        self.session_summaries = []
        self._tail_task = None
        # 解码段跟踪（供 session_<N>.json sidecar 精确复现解码路径）：
        # 会话开始 reset(0,0)；AUDIO_SYNC 按值重置并丢弃累积器中未凑满一帧的
        # 残余字节（这些字节保留在 .adpcm 原始流里但不参与解码）。
        self._segments = []            # 已闭合段：{offset, bytes, predictor, step_index}
        self._seg_offset = 0           # 当前段在 raw 流中的起始字节偏移
        self._seg_reset = (0, 0)       # 当前段的 reset 参数
        self._rx_bytes = 0             # 本会话已接收（追加进 chunks）的总字节数
        self._decoded_upto = 0         # 已解码字节的流偏移（不含被 SYNC 丢弃的残余）

    # ---- 事件日志 ----
    def log_event(self, event: str, **fields) -> None:
        rec = {"t": mono_ts(), "event": event}
        rec.update(fields)
        self._events_fp.write(json.dumps(rec, ensure_ascii=False) + "\n")
        self._events_fp.flush()

    def warn(self, msg: str, **fields) -> None:
        print(f"{ts()} WARN: {msg}", flush=True)
        self.log_event("warning", msg=msg, **fields)

    # ---- 会话生命周期 ----
    def _begin_session(self, interaction: int, codec: int, session_id: int) -> None:
        if self.session_state != "idle":
            self._finalize_session("superseded")
        self.session_no += 1
        self.session_state = "open"
        self.session_id = session_id
        self.session_codec = codec
        self.session_rate = 16000 if codec == CODEC_16K else 8000
        self.adpcm_chunks = []
        self.pcm_samples = []
        self.frame_count = 0
        self.first_audio_ts = None
        self._segments = []
        self._lvl_acc = 0
        self._lvl_n = 0
        self._seg_offset = 0
        self._seg_reset = (0, 0)
        self._rx_bytes = 0
        self._decoded_upto = 0
        # 遥控器固件每次会话从 0/0 重启编码器但可能不发 SYNC：硬重置
        self.decoder.reset(0, 0)
        self.accum.clear()

    def _close_segment(self) -> None:
        """闭合当前解码段（SYNC 重置或会话结束时调用）。"""
        self._segments.append({
            "offset": self._seg_offset,
            "bytes": self._decoded_upto - self._seg_offset,
            "predictor": self._seg_reset[0],
            "step_index": self._seg_reset[1],
        })

    def _finalize_session(self, reason: str) -> None:
        if self.session_state == "idle":
            return
        if self._tail_task is not None:
            self._tail_task.cancel()
            self._tail_task = None
        # 不足一帧的尾部字节也解码进 WAV（原始流已完整记录）
        tail = self.accum.take_pending()
        if tail:
            self.pcm_samples.extend(self.decoder.decode(tail))
            self._decoded_upto += len(tail)
        self._close_segment()
        raw = b"".join(self.adpcm_chunks)
        n = self.session_no
        adpcm_path = self.out_dir / f"session_{n}.adpcm"
        wav_path = self.out_dir / f"session_{n}.wav"
        raw_wav_path = self.out_dir / f"session_{n}.raw.wav"
        sidecar_path = self.out_dir / f"session_{n}.json"
        adpcm_path.write_bytes(raw)
        # raw.wav：纯解码（无平滑/增益），供单测 golden 逐样本对拍
        write_wav(raw_wav_path, self.pcm_samples, self.session_rate)
        samples = apply_gain(smooth3(self.pcm_samples), self.args.gain_db)
        write_wav(wav_path, samples, self.session_rate)
        duration_s = len(self.pcm_samples) / float(self.session_rate)
        latency_ms = None
        if self.mic_open_ts is not None and self.first_audio_ts is not None:
            latency_ms = round((self.first_audio_ts - self.mic_open_ts) * 1000.0, 1)
        summary = {
            "session": n,
            "reason": reason,
            "frames": self.frame_count,
            "adpcm_bytes": len(raw),
            "samples": len(self.pcm_samples),
            "duration_s": round(duration_s, 2),
            "sample_rate": self.session_rate,
            "first_audio_latency_ms": latency_ms,
        }
        # sidecar：golden 复现所需的全部参数（帧长/增益/逐段 reset 与字节区间）
        sidecar = {
            "session": n,
            "codec": self.session_codec,
            "sample_rate": self.session_rate,
            "frame_len": self.accum.frame_len,
            "gain_db": self.args.gain_db,
            "adpcm_bytes": len(raw),
            # consumed_adpcm_bytes 是已消费的 ADPCM 输入字节数（= 各段 bytes 之和），
            # 不是解码产出的 PCM 字节数；PCM 样本数见 samples。
            "consumed_adpcm_bytes": sum(s["bytes"] for s in self._segments),
            "samples": len(self.pcm_samples),
            "duration_s": round(duration_s, 3),
            "first_audio_latency_ms": latency_ms,
            "segments": self._segments,
            "files": {"adpcm": adpcm_path.name, "wav": wav_path.name,
                      "raw_wav": raw_wav_path.name},
        }
        sidecar_path.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2),
                                encoding="utf-8")
        self.session_summaries.append(summary)
        self.log_event("session_saved", file_adpcm=adpcm_path.name,
                       file_wav=wav_path.name, **summary)
        print(f"{ts()} session #{n} saved: frames={self.frame_count} "
              f"bytes={len(raw)} dur={duration_s:.2f}s "
              f"latency={latency_ms}ms ({reason})", flush=True)
        if sys.platform == "win32" and duration_s >= 0.2:
            toast("ATVV 采集", f"已录制第 {n} 段（{duration_s:.1f} 秒）")
        self.session_state = "idle"
        self.mic_open_ts = None

    async def _finalize_after_tail(self) -> None:
        try:
            await asyncio.sleep(TAIL_WINDOW_S + 0.01)
            if self.session_state == "stopping":
                self._finalize_session("stop")
        except asyncio.CancelledError:
            pass

    # ---- GATT 回调 ----
    # bleak 的 notify 回调在事件循环线程派发：回调内只做无锁的累积/状态变更，
    # 不做阻塞操作。
    def on_audio(self, _sender, data: bytearray) -> None:
        now = time.monotonic()
        if self.session_state == "open":
            pass
        elif self.session_state == "stopping":
            if now - self.stop_ts > TAIL_WINDOW_S:
                self.warn("audio tail packet beyond 150ms window dropped",
                          bytes=len(data))
                return
        else:
            self.warn("audio packet with no open session dropped", bytes=len(data))
            return
        if self.first_audio_ts is None:
            self.first_audio_ts = now
            self.log_event("first_audio", bytes=len(data))
        data = bytes(data)
        self.adpcm_chunks.append(data)
        self._rx_bytes += len(data)
        for frame in self.accum.push(data):
            self.frame_count += 1
            pcm = self.decoder.decode(frame)
            self.pcm_samples.extend(pcm)
            self._decoded_upto += len(frame)
            # 实时电平表（采集手法反馈）：每 0.5s 打印一次解码 RMS 柱条，
            # 说话期间应持续有柱条；柱条消失说明遥控器端送的是静音。
            self._lvl_acc += sum(s * s for s in pcm)
            self._lvl_n += len(pcm)
            if self._lvl_n >= 8000:
                rms = math.sqrt(self._lvl_acc / self._lvl_n)
                bars = "#" * min(40, int(rms / 100))
                print(f"{ts()} lvl {rms:6.0f} {bars}", flush=True)
                self._lvl_acc = 0
                self._lvl_n = 0

    def on_control(self, _sender, data: bytearray) -> None:
        data = bytes(data)
        if not data:
            return
        op = data[0]
        if op == OP_CAPS:
            try:
                self.caps = parse_caps(data)
            except ValueError as exc:
                self.warn(f"bad CAPS frame: {exc}")
                return
            self.version = self.caps["version"]
            self.accum.frame_len = self.caps["frame_len"]
            if self.caps["codecs"] & CODEC_16K:
                self.chosen_codec = CODEC_16K
            elif self.caps["codecs"] & CODEC_8K:
                self.chosen_codec = CODEC_8K
                self.warn("device only supports 8kHz codec")
            else:
                self.warn(f"no known codec in mask 0x{self.caps['codecs']:02x}")
            self.log_event("caps", version=f"0x{self.version:04x}",
                           codecs=self.caps["codecs"],
                           interaction=self.caps["interaction"],
                           frame_len=self.caps["frame_len"],
                           legacy=self.caps["legacy"])
            print(f"{ts()} CAPS: ver=0x{self.version:04x} "
                  f"codecs=0x{self.caps['codecs']:02x} "
                  f"interaction=0x{self.caps['interaction']:02x} "
                  f"frame_len={self.caps['frame_len']}", flush=True)
        elif op == OP_MIC_OPEN:
            self.mic_open_ts = time.monotonic()
            self.log_event("mic_open")
            print(f"{ts()} MIC_OPEN request", flush=True)
            # 主机应答 MIC_OPEN：v>=1.0 写 0x0C 0x00；旧版附带 codec 字节
            if self.version >= 0x0100:
                payload = bytes([CMD_MIC_OPEN_ACK, 0x00])
            else:
                payload = bytes([CMD_MIC_OPEN_ACK, 0x00, self.chosen_codec])
            self._tx(payload)
        elif op == OP_STREAM_START:
            interaction = data[1] if len(data) > 1 else 0
            codec = data[2] if len(data) > 2 else self.chosen_codec
            session_id = data[3] if len(data) > 3 else 0
            self._begin_session(interaction, codec, session_id)
            self.log_event("stream_start", interaction=interaction,
                           codec=codec, session_id=session_id)
            print(f"{ts()} STREAM_START: codec=0x{codec:02x} "
                  f"session={session_id}", flush=True)
        elif op == OP_AUDIO_SYNC:
            if len(data) >= 7:
                predictor = struct.unpack(">h", data[4:6])[0]
                step_index = data[6]
                if self.session_state in ("open", "stopping"):
                    # 闭合当前段；累积器中未凑满一帧的残余字节随重置丢弃
                    # （保留在 .adpcm 原始流里），下一段从 _rx_bytes 偏移开始。
                    self._close_segment()
                    pending = self.accum.pending()
                    self._seg_offset = self._decoded_upto + pending
                    self._decoded_upto = self._seg_offset
                    self._seg_reset = (predictor, step_index)
                self.decoder.reset(predictor, step_index)
                self.accum.clear()
                self.log_event("audio_sync", predictor=predictor,
                               step_index=step_index)
                print(f"{ts()} AUDIO_SYNC: predictor={predictor} "
                      f"step_index={step_index}", flush=True)
            else:
                self.warn(f"short AUDIO_SYNC frame: {data.hex()}")
        elif op == OP_STOP:
            self.stop_ts = time.monotonic()
            if self.session_state == "open":
                self.session_state = "stopping"
                self._tail_task = asyncio.ensure_future(self._finalize_after_tail())
            self.log_event("stop")
            print(f"{ts()} STOP", flush=True)
        else:
            self.log_event("control_unknown", opcode=op, raw=data.hex())

    def on_disconnect(self, _client) -> None:
        self.connected = False
        self.log_event("disconnected")
        print(f"{ts()} !!! disconnected", flush=True)

    # ---- TX 写命令 ----
    def _tx(self, payload: bytes) -> None:
        if self.client is None or not self.connected:
            return
        asyncio.ensure_future(self._tx_async(payload))

    async def _tx_async(self, payload: bytes) -> None:
        try:
            await self.client.write_gatt_char(ATVV_TX_UUID, payload, response=False)
        except Exception as exc:
            self.warn(f"TX write failed: {exc}")

    async def send_mic_close(self) -> None:
        if self.client is None or not self.connected:
            return
        if self.version >= 0x0100:
            payload = bytes([CMD_MIC_CLOSE, self.session_id & 0xFF])
        else:
            payload = bytes([CMD_MIC_CLOSE])
        try:
            await self.client.write_gatt_char(ATVV_TX_UUID, payload, response=False)
            self.log_event("mic_close", session_id=self.session_id)
            print(f"{ts()} MIC_CLOSE sent", flush=True)
        except Exception as exc:
            self.warn(f"MIC_CLOSE write failed: {exc}")

    # ---- 主流程 ----
    async def run(self) -> int:
        from bleak import BleakClient  # 延迟 import，self-test 不依赖 bleak

        deadline = time.monotonic() + self.args.duration
        last_rc = 0
        while time.monotonic() < deadline:
            rc = await self._serve_once(BleakClient, deadline)
            if rc == 0:
                break  # 正常收官（max_sessions 或 duration 到点）
            last_rc = rc
            # 遥控器睡眠断链/人格被占/短暂故障：等待后自动重连，直到时长上限。
            # 这保证采集器可常驻自服务：用户随时按键都能录，不用对齐窗口。
            print(f"{ts()} link lost (rc={rc}), reconnect in 2s ...", flush=True)
            await asyncio.sleep(2.0)
        self._print_summary()
        return 0 if self.session_summaries else last_rc

    async def _serve_once(self, BleakClient, deadline: float) -> int:
        """单次连接+值守。返回 0=正常收官；非 0=链路异常（外层自动重连）。"""
        self.connected = False
        device = await self._find_device()
        if device is None:
            return 2
        print(f"{ts()} connecting {device.name} [{device.address}] ...", flush=True)
        # Windows 上禁用 GATT 服务缓存：遥控器睡眠/断连后 OS 缓存可能陈旧，
        # 导致 ATVV service 假性 "not found"（重试也无法自愈）。
        kwargs = ({"winrt": {"use_cached_services": False}}
                  if sys.platform == "win32" else {})
        self.client = BleakClient(device, disconnected_callback=self.on_disconnect,
                                  **kwargs)
        try:
            await self.client.connect()
        except Exception as exc:
            print(f"{ts()} connect failed: {exc}", flush=True)
            print("提示：设备通常需先在 Windows 系统蓝牙设置里完成配对（Bond）。",
                  flush=True)
            return 2
        self.connected = True
        self.log_event("connect", name=device.name, address=device.address)
        print(f"{ts()} connected", flush=True)

        service = self.client.services.get_service(ATVV_SERVICE_UUID)
        if service is None:
            avail = [str(s.uuid) for s in self.client.services]
            print(f"{ts()} ATVV service {ATVV_SERVICE_UUID} not found; "
                  f"available services: {avail}", flush=True)
            print("提示：ATVV 语音人格被其他主机（如 VoiceStick.exe）占用，"
                  "或遥控器处于维护模式。", flush=True)
            await self.client.disconnect()
            self.connected = False
            return 3
        for uuid in (ATVV_TX_UUID, ATVV_AUDIO_UUID, ATVV_CONTROL_UUID):
            if service.get_characteristic(uuid) is None:
                print(f"{ts()} characteristic {uuid} missing", flush=True)
                await self.client.disconnect()
                self.connected = False
                return 3

        await self.client.start_notify(ATVV_CONTROL_UUID, self.on_control)
        await self.client.start_notify(ATVV_AUDIO_UUID, self.on_audio)
        await self._tx_async(CMD_GET_CAPS)
        print(f"{ts()} subscribed, GET_CAPS sent; press voice key on remote ...",
              flush=True)
        toast("ATVV 采集", "已连接遥控器。按住语音键贴嘴说话即可，"
                           "每录完一段会弹框确认。")

        hit_limit = False
        try:
            while self.connected:
                await asyncio.sleep(0.1)
                if time.monotonic() >= deadline:
                    print(f"{ts()} duration limit reached", flush=True)
                    hit_limit = True
                    break
                if (self.args.max_sessions > 0
                        and len(self.session_summaries) >= self.args.max_sessions):
                    print(f"{ts()} max_sessions={self.args.max_sessions} reached",
                          flush=True)
                    hit_limit = True
                    break
        except asyncio.CancelledError:
            hit_limit = True

        self._finalize_session("exit")
        await self.send_mic_close()
        if self.connected:
            await self.client.disconnect()
        self.connected = False
        return 0 if hit_limit else 4  # 4 = 对端断链，外层 2s 后自动重连

    async def _find_device(self):
        from bleak import BleakScanner

        if self.args.address:
            from bleak import BLEDevice
            print(f"{ts()} using --address {self.args.address}", flush=True)
            # bleak>=3.0：BLEDevice 仅收 address/name/details（rssi 位置传参 TypeError，
            # 关键字传参 DeprecationWarning 且无效果）。
            return BLEDevice(self.args.address, self.args.name or None, {})
        if self.args.name:
            print(f"{ts()} scanning for name '{self.args.name}' ...", flush=True)
            dev = await BleakScanner.find_device_by_name(
                self.args.name, timeout=SCAN_TIMEOUT_S)
            if dev is None:
                print(f"{ts()} device named '{self.args.name}' not found",
                      flush=True)
            return dev
        print(f"{ts()} scanning whitelist {NAME_WHITELIST} ...", flush=True)
        found = None
        try:
            advs = await BleakScanner.discover(timeout=SCAN_TIMEOUT_S,
                                               return_adv=True)
            for dev, adv in advs.values():
                name = (dev.name or adv.local_name or "").strip().lower()
                uuids = [u.lower() for u in (adv.service_uuids or [])]
                if name in NAME_WHITELIST or ATVV_SERVICE_UUID.lower() in uuids:
                    found = dev
                    break
        except TypeError:
            # 旧版 bleak 无 return_adv：只按名称匹配
            for dev in await BleakScanner.discover(timeout=SCAN_TIMEOUT_S):
                if (dev.name or "").strip().lower() in NAME_WHITELIST:
                    found = dev
                    break
        if found is None:
            print(f"{ts()} no whitelisted device found "
                  f"(scan {SCAN_TIMEOUT_S:.0f}s)", flush=True)
        return found

    def _print_summary(self) -> None:
        print(f"{ts()} ---- summary ----", flush=True)
        if self.caps:
            print(f"  CAPS: ver=0x{self.caps['version']:04x} "
                  f"codecs=0x{self.caps['codecs']:02x} "
                  f"frame_len={self.caps['frame_len']}", flush=True)
        else:
            print("  CAPS: (not received)", flush=True)
        print(f"  sessions: {len(self.session_summaries)}", flush=True)
        for s in self.session_summaries:
            print(f"    #{s['session']}: frames={s['frames']} "
                  f"bytes={s['adpcm_bytes']} dur={s['duration_s']}s "
                  f"rate={s['sample_rate']} "
                  f"first_audio_latency={s['first_audio_latency_ms']}ms",
                  flush=True)
        print(f"  output dir: {self.out_dir}", flush=True)

    def close(self) -> None:
        self._events_fp.close()


# ---- 自测（纯逻辑，不连设备，不依赖 bleak） ---------------------------------
def self_test() -> int:
    failures = []

    def check(name: str, fn) -> None:
        try:
            fn()
            print(f"PASS {name}", flush=True)
        except AssertionError as exc:
            failures.append(name)
            print(f"FAIL {name}: {exc}", flush=True)

    def test_adpcm_basic() -> None:
        # 0x11（高半字节先解码）从 predictor=0/step_index=0：
        # step=7, diff=0, nibble&1 -> +7>>2=1 -> predictor=1；再 1 -> 2
        dec = ImaAdpcmDecoder()
        got = dec.decode(bytes([0x11]))
        assert got == [1, 2], f"expect [1, 2], got {got}"

    def test_adpcm_reset() -> None:
        dec = ImaAdpcmDecoder()
        dec.decode(bytes([0x77, 0x77, 0x77]))
        assert (dec.predictor, dec.step_index) != (0, 0)
        dec.reset(0, 0)
        assert (dec.predictor, dec.step_index) == (0, 0)
        # AUDIO_SYNC 语义：指定 predictor/step_index，越界钳位
        dec.reset(30000, 200)
        assert dec.predictor == 30000 and dec.step_index == 88
        dec.reset(-40000, -5)
        assert dec.predictor == -32768 and dec.step_index == 0

    def test_frame_accumulator() -> None:
        acc = FrameAccumulator(frame_len=4)
        assert acc.push(b"\x01\x02") == []
        frames = acc.push(b"\x03\x04\x05")
        assert frames == [b"\x01\x02\x03\x04"], frames
        assert acc.pending() == 1
        frames = acc.push(b"\x06\x07\x08\x09\x0a")
        assert frames == [b"\x05\x06\x07\x08"], frames
        assert acc.pending() == 2
        assert acc.take_pending() == b"\x09\x0a"
        acc.push(b"\x01\x02")
        acc.clear()
        assert acc.pending() == 0

    def test_caps_v10() -> None:
        # 0B 01 00 02 03 00 78：v1.0，16kHz，interaction 3，帧长 120
        caps = parse_caps(bytes([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78]))
        assert caps["version"] == 0x0100 and caps["codecs"] == 0x02
        assert caps["interaction"] == 0x03 and caps["frame_len"] == 120
        assert not caps["legacy"]
        # 帧长为 0 -> 默认 120
        caps = parse_caps(bytes([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00]))
        assert caps["frame_len"] == DEFAULT_FRAME_LEN

    def test_caps_legacy_layout() -> None:
        # 兼容分支：v>=1.0 但 codecs==0，len>=9，bytes[4]&0x03 != 0
        caps = parse_caps(bytes([0x0B, 0x01, 0x00, 0x00, 0x02,
                                 0x00, 0x00, 0x00, 0x00]))
        assert caps["codecs"] == 0x02 and caps["interaction"] == 0x03
        assert caps["legacy"]
        # v<1.0 旧版：len>=9，codecs=bytes[4]，interaction=0
        caps = parse_caps(bytes([0x0B, 0x00, 0x09, 0x00, 0x02,
                                 0x00, 0x00, 0x00, 0x00]))
        assert caps["version"] == 0x0009 and caps["codecs"] == 0x02
        assert caps["interaction"] == 0 and caps["legacy"]

    def test_caps_8k_only() -> None:
        caps = parse_caps(bytes([0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78]))
        assert caps["codecs"] == CODEC_8K
        assert not (caps["codecs"] & CODEC_16K)

    def test_caps_bad() -> None:
        for bad in (b"", bytes([0x0B, 0x01]), bytes([0x07, 0x01, 0x00])):
            try:
                parse_caps(bad)
            except ValueError:
                continue
            raise AssertionError(f"no ValueError for {bad.hex()}")
        # 旧版长度不足
        try:
            parse_caps(bytes([0x0B, 0x00, 0x09, 0x00, 0x02]))
        except ValueError:
            return
        raise AssertionError("legacy short frame not rejected")

    def test_smooth() -> None:
        assert smooth3([0, 4, 0]) == [0, 2, 0]
        assert smooth3([1, 2]) == [1, 2]     # 首尾不动，短序列原样
        assert smooth3([]) == []
        # 中间点：(1 + 2*3 + 1)>>2 = 2
        assert smooth3([1, 3, 1, 100]) == [1, 2, (3 + 2 + 100) >> 2, 100]

    def test_adpcm_encoder_roundtrip() -> None:
        # 自写编码器 → 解码器：解码输出须与编码器 predictor 轨迹一致
        pcm = [int(math.sin(i * 0.05) * 12000.0 + i * 10) for i in range(480)]
        encoded, expected = ima_encode(pcm)
        dec = ImaAdpcmDecoder()
        assert dec.decode(encoded) == expected
        # 与既有标准向量交叉固定：0x11 起
        encoded2, expected2 = ima_encode([1, 2])
        assert encoded2[0] != 0 and expected2 == [1, 2]

    def test_gain() -> None:
        assert apply_gain([100, -100], 0.0) == [100, -100]
        # +6dB ≈ 1.9953 倍
        got = apply_gain([1000], 6.0)
        assert got == [1995], got
        # 舍入与 C++ std::lround 对齐（half away from zero）：±x.5 都远离 0
        f = math.pow(10.0, 6.0 / 20.0)
        half_pos = 0.5 / f       # 乘以 f 后恰为 +0.5
        half_neg = -half_pos
        assert apply_gain([half_pos, half_neg], 6.0) == [1, -1]
        # 限幅到 int16
        assert apply_gain([30000], 24.0) == [32767]
        # gain_db 钳位 ±24：30dB 按 24dB 处理
        assert apply_gain([1000], 30.0) == apply_gain([1000], 24.0)

    def test_session_sidecar_segments() -> None:
        # 离线模拟一次会话（不调 GATT 回调的 asyncio 路径），验证 raw.wav /
        # sidecar 段落记账与解码可复现性。
        import tempfile
        import types
        pcm_src = [int(math.sin(i * 0.1) * 8000) for i in range(800)]
        encoded, expected = ima_encode(pcm_src)   # 400 字节
        with tempfile.TemporaryDirectory() as td:
            args = types.SimpleNamespace(out_dir=td, gain_db=6.0,
                                         duration=1.0, max_sessions=1)
            cap = AtvvCapture(args)
            cap.accum.frame_len = 120
            cap._begin_session(3, CODEC_16K, 7)
            cap.on_audio(None, bytearray(encoded[:200]))
            # 模拟 AUDIO_SYNC：丢弃累积器残余（200-120=80 字节），重置 0/0
            cap.on_control(None, bytes([0x0A, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00]))
            cap.on_audio(None, bytearray(encoded[200:]))
            cap._finalize_session("stop")
            cap.close()
            out = Path(td)
            sidecar = json.loads((out / "session_1.json").read_text("utf-8"))
            assert sidecar["frame_len"] == 120 and sidecar["gain_db"] == 6.0
            segs = sidecar["segments"]
            assert len(segs) == 2, segs
            assert (segs[0]["offset"], segs[0]["bytes"]) == (0, 120)
            # 段 1 从丢弃残余后的偏移 200 开始，reset(0,0)
            assert (segs[1]["offset"], segs[1]["bytes"]) == (200, 200)
            assert (segs[1]["predictor"], segs[1]["step_index"]) == (0, 0)
            assert sidecar["consumed_adpcm_bytes"] == 320
            assert sidecar["adpcm_bytes"] == 400
            # 按 sidecar 段落从 .adpcm 复现解码，逐样本等于 raw.wav
            raw = (out / "session_1.adpcm").read_bytes()
            dec = ImaAdpcmDecoder()
            reproduced = []
            for seg in segs:
                dec.reset(seg["predictor"], seg["step_index"])
                reproduced += dec.decode(raw[seg["offset"]:
                                             seg["offset"] + seg["bytes"]])
            pcm_from_raw = read_wav_samples(out / "session_1.raw.wav")
            assert reproduced == pcm_from_raw
            # wav = raw 经 smooth3+gain(6dB)
            pcm_from_wav = read_wav_samples(out / "session_1.wav")
            assert pcm_from_wav == apply_gain(smooth3(reproduced), 6.0)
            assert (out / "events.jsonl").exists()

    check("adpcm_basic", test_adpcm_basic)
    check("adpcm_reset", test_adpcm_reset)
    check("adpcm_encoder_roundtrip", test_adpcm_encoder_roundtrip)
    check("frame_accumulator", test_frame_accumulator)
    check("caps_v10", test_caps_v10)
    check("caps_legacy_layout", test_caps_legacy_layout)
    check("caps_8k_only", test_caps_8k_only)
    check("caps_bad", test_caps_bad)
    check("smooth", test_smooth)
    check("gain", test_gain)
    check("session_sidecar_segments", test_session_sidecar_segments)

    total = 11
    print(f"---- self-test: {total - len(failures)}/{total} passed ----",
          flush=True)
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="小米蓝牙遥控器 2 Pro ATVV 语音会话采集工具（bleak）")
    ap.add_argument("--address", help="直接连接指定 MAC（AA:BB:...）")
    ap.add_argument("--name", help="按设备名连接")
    ap.add_argument("--out-dir", default=None,
                    help="输出目录（默认 scripts/e2e_test/fixtures/xiaomi/<timestamp>/）")
    ap.add_argument("--gain-db", type=float, default=0.0,
                    help="PCM 增益 dB（钳位 ±24，默认 0）")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="总采集时长上限秒数（默认 60）")
    ap.add_argument("--max-sessions", type=int, default=3,
                    help="采够 N 次按键会话后自动退出（默认 3，0 为不限）")
    ap.add_argument("--self-test", action="store_true",
                    help="纯逻辑自测（ADPCM/切帧/CAPS/后处理），不连设备")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    try:
        import bleak  # noqa: F401
    except ImportError:
        print("缺少 bleak，请先 pip install bleak；或用 --self-test 跑纯逻辑自测。",
              flush=True)
        return 1

    if args.out_dir is None:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        # 默认落在脚本旁 fixtures/xiaomi/<ts>/，即单测 golden 扫描根
        # scripts/e2e_test/fixtures/xiaomi/（与 CWD 无关）。
        args.out_dir = str(Path(__file__).resolve().parent
                           / "fixtures" / "xiaomi" / stamp)

    cap = AtvvCapture(args)
    suppressor = F5Suppressor()
    suppressor.start()
    try:
        return asyncio.run(cap.run())
    except KeyboardInterrupt:
        print(f"{ts()} interrupted", flush=True)
        return 0
    finally:
        suppressor.stop()
        cap.close()


if __name__ == "__main__":
    sys.exit(main())
