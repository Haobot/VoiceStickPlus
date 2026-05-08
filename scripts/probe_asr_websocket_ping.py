#!/usr/bin/env python3
"""Probe whether the ASR WebSocket server sends ping or fragmented frames.

This intentionally avoids websocket client libraries because they reassemble
continuation frames and auto-handle ping/pong before user code can observe them.
"""

from __future__ import annotations

import argparse
import base64
import os
import secrets
import socket
import ssl
import struct
import sys
import time
from pathlib import Path
from urllib.parse import urlparse


VOLCENGINE_URL = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async"
DEFAULT_RESOURCE_ID = "volc.seedasr.sauc.duration"

EVENT_START_CONNECTION = 1
EVENT_FINISH_CONNECTION = 2
EVENT_START_SESSION = 100
EVENT_FINISH_SESSION = 102

OPCODES = {
    0x0: "continuation",
    0x1: "text",
    0x2: "binary",
    0x8: "close",
    0x9: "ping",
    0xA: "pong",
}


def load_windows_config() -> dict[str, str]:
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


def json_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def session_payload(resource_id: str) -> bytes:
    payload = (
        '{"user":{"uid":"voice-stick-local"},'
        '"audio":{"format":"ogg","codec":"opus","rate":16000,"bits":16,"channel":1},'
        '"request":{"model_name":"bigmodel","enable_nonstream":true,'
        '"show_utterances":false,"result_type":"full","enable_ddc":true,'
        f'"resource_id":"{json_escape(resource_id)}"'
        "}}"
    )
    return payload.encode("utf-8")


def connection_payload(resource_id: str) -> bytes:
    return (
        b'{"namespace":"BidirectionalASR","event":0,"req_params":'
        + session_payload(resource_id)
        + b"}"
    )


def make_binary_frame(message_type: int, flags: int, serialization: int, compression: int, payload: bytes) -> bytes:
    return bytes([0x11, (message_type << 4) | flags, (serialization << 4) | compression, 0x00]) + struct.pack(
        ">I", len(payload)
    ) + payload


def make_event_frame(message_type: int, event: int, session_id: str, serialization: int, payload: bytes) -> bytes:
    out = bytearray([0x11, (message_type << 4) | 0x04, serialization << 4, 0x00])
    out += struct.pack(">I", event)
    if session_id:
        session_bytes = session_id.encode("utf-8")
        out += struct.pack(">I", len(session_bytes)) + session_bytes
    out += struct.pack(">I", len(payload)) + payload
    return bytes(out)


def make_start_connection_frame(resource_id: str) -> bytes:
    return make_event_frame(0x01, EVENT_START_CONNECTION, "", 0x01, connection_payload(resource_id))


def make_finish_connection_frame(resource_id: str) -> bytes:
    return make_event_frame(0x01, EVENT_FINISH_CONNECTION, "", 0x01, connection_payload(resource_id))


def make_start_session_frame(resource_id: str, session_id: str) -> bytes:
    return make_event_frame(0x01, EVENT_START_SESSION, session_id, 0x01, session_payload(resource_id))


def make_finish_session_frame(resource_id: str, session_id: str) -> bytes:
    return make_event_frame(0x01, EVENT_FINISH_SESSION, session_id, 0x01, connection_payload(resource_id))


def make_legacy_client_request_frame(resource_id: str) -> bytes:
    return make_binary_frame(0x01, 0x00, 0x01, 0x00, session_payload(resource_id))


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise EOFError("socket closed")
        chunks += chunk
    return bytes(chunks)


def send_ws_frame(sock: socket.socket, opcode: int, payload: bytes, fin: bool = True) -> None:
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


def recv_ws_frame(sock: socket.socket) -> tuple[bool, int, bytes]:
    first, second = read_exact(sock, 2)
    fin = bool(first & 0x80)
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
    return fin, opcode, payload


def websocket_handshake(url: str, api_key: str, resource_id: str, timeout: float) -> socket.socket:
    parsed = urlparse(url)
    if parsed.scheme not in {"ws", "wss"}:
        raise ValueError(f"unsupported URL scheme: {parsed.scheme}")
    host = parsed.hostname
    if not host:
        raise ValueError("URL has no host")
    port = parsed.port or (443 if parsed.scheme == "wss" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query

    raw = socket.create_connection((host, port), timeout=timeout)
    raw.settimeout(timeout)
    sock: socket.socket
    if parsed.scheme == "wss":
        sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host)
    else:
        sock = raw

    key = base64.b64encode(secrets.token_bytes(16)).decode("ascii")
    headers = [
        f"GET {path} HTTP/1.1",
        f"Host: {host}:{port}" if parsed.port else f"Host: {host}",
        "Upgrade: websocket",
        "Connection: Upgrade",
        f"Sec-WebSocket-Key: {key}",
        "Sec-WebSocket-Version: 13",
        "User-Agent: VoiceStick-ASR-Fragment-Probe/1.0",
        f"X-Api-Key: {api_key}",
        f"X-Api-Resource-Id: {resource_id}",
        f"X-Api-Request-Id: voice-stick-fragment-probe-{secrets.token_hex(8)}",
        "X-Api-Sequence: -1",
        "",
        "",
    ]
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


def parse_event_id(payload: bytes) -> str:
    if len(payload) < 8:
        return ""
    message_type = payload[1] >> 4
    flags = payload[1] & 0x0F
    compression = payload[2] & 0x0F
    offset = (payload[0] & 0x0F) * 4
    if message_type in {0x09, 0x0B} and flags == 0x04 and compression == 0 and len(payload) >= offset + 4:
        event_id = struct.unpack(">I", payload[offset : offset + 4])[0]
        return f" event={event_id}"
    if message_type == 0x0F:
        return " error"
    return f" message_type=0x{message_type:x}"


def main() -> int:
    config = load_windows_config()
    provider = config.get("asr_provider", "volcengine")
    default_url = config.get("voicestick_cloud_url") if provider == "voicestick_cloud" else VOLCENGINE_URL
    default_key = config.get("voicestick_api_key") if provider == "voicestick_cloud" else config.get("volcengine_api_key")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=os.environ.get("VOICESTICK_ASR_URL") or default_url or VOLCENGINE_URL)
    parser.add_argument("--api-key", default=os.environ.get("VOICESTICK_ASR_API_KEY") or default_key)
    parser.add_argument("--resource-id", default=os.environ.get("VOICESTICK_ASR_RESOURCE_ID") or config.get("resource_id") or DEFAULT_RESOURCE_ID)
    parser.add_argument("--seconds", type=float, default=30.0, help="How long to observe frames after connecting.")
    parser.add_argument("--timeout", type=float, default=15.0, help="Socket read/connect timeout.")
    parser.add_argument("--legacy", action="store_true", help="Send the non-reusable ASR client request frame.")
    parser.add_argument("--start-session", action="store_true", help="Also start and finish an empty reusable ASR session.")
    parser.add_argument("--no-auto-pong", action="store_true", help="Do not reply to server ping frames.")
    args = parser.parse_args()

    if not args.api_key:
        print("Missing ASR API key. Set VOICESTICK_ASR_API_KEY or configure VoiceStick first.", file=sys.stderr)
        return 2

    print(f"Connecting to {args.url}")
    sock = websocket_handshake(args.url, args.api_key, args.resource_id, args.timeout)
    fragmented_frames = 0
    continuation_frames = 0
    fragmented_messages = 0
    ping_frames = 0
    pong_frames = 0
    current_message = bytearray()
    session_id = "voice-stick-fragment-probe-" + secrets.token_hex(8)

    try:
        if args.legacy:
            send_ws_frame(sock, 0x2, make_legacy_client_request_frame(args.resource_id))
            print("sent legacy client request")
        else:
            send_ws_frame(sock, 0x2, make_start_connection_frame(args.resource_id))
            print("sent reusable start_connection")
            if args.start_session:
                send_ws_frame(sock, 0x2, make_start_session_frame(args.resource_id, session_id))
                send_ws_frame(sock, 0x2, make_finish_session_frame(args.resource_id, session_id))
                print("sent empty reusable session")

        deadline = time.monotonic() + args.seconds
        index = 0
        while time.monotonic() < deadline:
            remaining = max(0.1, min(args.timeout, deadline - time.monotonic()))
            sock.settimeout(remaining)
            try:
                fin, opcode, payload = recv_ws_frame(sock)
            except socket.timeout:
                break
            index += 1
            is_fragment = (not fin) or opcode == 0x0
            if not fin:
                fragmented_frames += 1
            if opcode == 0x0:
                continuation_frames += 1
            if opcode == 0x9:
                ping_frames += 1
                if not args.no_auto_pong:
                    send_ws_frame(sock, 0xA, payload)
            if opcode == 0xA:
                pong_frames += 1
            if is_fragment:
                fragmented_messages += 1

            detail = ""
            if opcode in {0x1, 0x2} and fin:
                detail = parse_event_id(payload)
            elif opcode in {0x1, 0x2} and not fin:
                current_message = bytearray(payload)
            elif opcode == 0x0:
                current_message += payload
                if fin:
                    detail = parse_event_id(bytes(current_message))
                    current_message.clear()

            print(
                f"#{index:03d} fin={int(fin)} opcode=0x{opcode:x}({OPCODES.get(opcode, 'unknown')}) "
                f"payload_len={len(payload)} fragmented={int(is_fragment)}{detail}"
            )
            if opcode == 0x8:
                break

        if not args.legacy:
            try:
                send_ws_frame(sock, 0x2, make_finish_connection_frame(args.resource_id))
            except OSError:
                pass
    finally:
        sock.close()

    print(
        "summary: "
        f"ping_frames={ping_frames} "
        f"pong_frames={pong_frames} "
        f"fragmented_frames={fragmented_frames} "
        f"continuation_frames={continuation_frames} "
        f"fragmented_messages={fragmented_messages}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
