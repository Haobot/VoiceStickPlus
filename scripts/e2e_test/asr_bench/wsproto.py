"""纯 stdlib 的 WebSocket 客户端 + 配置读取。

协议帧处理逻辑复刻 replay_volcengine_asr.py，凭据只从 config.toml 读取。
"""
from __future__ import annotations

import base64
import os
import secrets
import socket
import ssl
import struct
from pathlib import Path
from urllib.parse import urlparse


def load_windows_config() -> dict[str, str]:
    """读取 %APPDATA%/VoiceStick/config.toml 顶层键值（不解析 section，够凭据用）。"""
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return {}
    path = Path(appdata) / "VoiceStick" / "config.toml"
    values: dict[str, str] = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return values
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
            value = value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        values[key.strip()] = value
    return values


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("socket closed")
        chunks += chunk
    return bytes(chunks)


def send_ws_frame(sock: socket.socket, opcode: int, payload: bytes | str,
                  fin: bool = True) -> None:
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    first = (0x80 if fin else 0) | opcode
    length = len(payload)
    header = bytearray([first])
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack(">H", length)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", length)
    mask = secrets.token_bytes(4)
    masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(bytes(header) + mask + masked)


def recv_ws_frame(sock: socket.socket) -> tuple[int, bytes]:
    first, second = read_exact(sock, 2)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(sock, 8))[0]
    mask = read_exact(sock, 4) if masked else b""
    payload = read_exact(sock, length)
    if masked:
        payload = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return opcode, payload


def websocket_handshake(url: str, timeout: float,
                        extra_headers: list[str] | None = None) -> socket.socket:
    """建立 WebSocket 连接；extra_headers 用于火山 X-Api-* 或腾讯签名 URL 场景。"""
    parsed = urlparse(url)
    host = parsed.hostname
    if not host:
        raise ValueError("URL has no host")
    port = parsed.port or (443 if parsed.scheme == "wss" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query
    raw = socket.create_connection((host, port), timeout=timeout)
    raw.settimeout(timeout)
    sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) \
        if parsed.scheme == "wss" else raw

    key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
    headers = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}:{port}" if parsed.port else f"Host: {host}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
        "User-Agent: VoiceStick-ASR-Bench/1.0",
    ]
    if extra_headers:
        headers.extend(extra_headers)
    headers += ["", ""]
    sock.sendall("\r\n".join(headers).encode("ascii"))
    response = bytearray()
    while b"\r\n\r\n" not in response:
        response += sock.recv(4096)
        if len(response) > 65536:
            raise RuntimeError("oversized handshake response")
    status = response.split(b"\r\n", 1)[0].decode("iso-8859-1", errors="replace")
    if " 101 " not in status:
        raise RuntimeError(f"websocket upgrade failed: {status}")
    return sock


def demux_ogg_packets(path: str | Path) -> list[bytes]:
    """按 Ogg 段表（lacing）正确拆出单个 Opus 包，跳过 OpusHead/OpusTags。

    一个包可能跨多个 255 字节段，lacing 值 < 255 的段标志包结束；
    一页可含多个包（ffmpeg 封装）也可能一页一包（桌面端 OggOpusMuxer）。
    """
    data = Path(path).read_bytes()
    frames = []
    pos = 0
    current = bytearray()
    while pos + 27 <= len(data):
        if data[pos:pos + 4] != b"OggS":
            break
        nseg = data[pos + 26]
        header_size = 27 + nseg
        lacing = data[pos + 27:pos + header_size]
        seg_pos = pos + header_size
        for lace in lacing:
            current += data[seg_pos:seg_pos + lace]
            seg_pos += lace
            if lace < 255:  # 包结束
                pkt = bytes(current)
                current = bytearray()
                if pkt and not pkt.startswith(b"Opus"):
                    frames.append(pkt)
        pos = seg_pos
    if current and not bytes(current).startswith(b"Opus"):
        frames.append(bytes(current))
    return frames
