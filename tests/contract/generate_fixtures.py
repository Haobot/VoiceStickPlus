#!/usr/bin/env python3
"""tests/contract fixtures 生成器——黄金字节独立于实现手搓。

依据 Doc/Ref/protocol.md 的帧结构与事件表构造跨端契约样本，产出
fixtures/manifest.json 供三端契约测试消费：

  - Windows: desktop/windows/tests/core_tests.cc::TestContractFixtures（CI ctest）
  - macOS:   desktop/macos/Tests/VoiceStickTests/ContractFixtureTests.swift（CI swift run）
  - 固件端:  control_cmd_contract_test.c（run_tests.py 目标 firmware_control_cmd，
            CI host-tests）✅ 三端齐。

约定：
  - JSON 类帧（state/power_mgmt/ota_state/control）expect 用**线上字段名**（snake_case），
    各端测试自行映射到本端结构体字段；control_payloads 的 expect 为完整对象语义
    （字段集合+值，键序无关——macOS JSONSerialization 键序不保证）。
  - 二进制帧（audio/motion/ota control）比对整帧字节。

运行：python3 tests/contract/generate_fixtures.py
"""

import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "fixtures" / "manifest.json"


def frame(ftype: int, payload: bytes) -> bytes:
    """state/power_mgmt/ota_state 共用 4 字节帧头：version=1, type, u16 payload_len。"""
    return bytes([1, ftype]) + struct.pack("<H", len(payload)) + payload


def state_frame(js: str) -> bytes:
    return frame(0x10, js.encode("utf-8"))


def ota_state_frame(js: str) -> bytes:
    return frame(0x30, js.encode("utf-8"))


def audio_frame(session_id: int, seq: int, flags: int, payload: bytes) -> bytes:
    # Doc/Ref/protocol.md「Audio Frame」：16 字节头。
    return (
        bytes([1, 0x01])
        + struct.pack("<H", 16)
        + struct.pack("<II", session_id, seq)
        + bytes([flags, 0])
        + struct.pack("<H", len(payload))
        + payload
    )


def motion_frame(dx: int, dy: int) -> bytes:
    return bytes([1, 0x11]) + struct.pack("<hh", dx, dy)


def ota_begin(image_size: int, transfer_id: int) -> bytes:
    return bytes([1, 0x20, 12, 0]) + struct.pack("<II", image_size, transfer_id)


def ota_data(transfer_id: int, offset: int, chunk: bytes) -> bytes:
    return bytes([1, 0x21, 12, 0]) + struct.pack("<II", transfer_id, offset) + chunk


def ota_end(transfer_id: int, image_size: int) -> bytes:
    return bytes([1, 0x22, 12, 0]) + struct.pack("<II", transfer_id, image_size)


def ota_abort(transfer_id: int) -> bytes:
    return bytes([1, 0x23, 8, 0]) + struct.pack("<I", transfer_id)


def hx(b: bytes) -> str:
    return b.hex()


# ---------------------------------------------------------------- state 事件
# JSON 文本逐字取自 Doc/Ref/protocol.md「State Event」示例（协议即规格）。
STATE = [
    (
        "device_info",
        '{"event":"device_info","hardware":"stick_s3","firmware_version":"0.2.2",'
        '"buttons":["primary","secondary"],"interaction_modes":["hold_to_talk","click_to_talk"],'
        '"ui_states":["ready","recording","thinking","pending_confirmation","error","air_mouse"]}',
        {"event": "device_info", "hardware": "stick_s3", "firmware_version": "0.2.2"},
        # 数组字段仅 macOS 结构体暴露（Windows StateEvent 无 buttons/ui_states 字段），
        # 期望集只取两端公共字段；数组解析各端另有覆盖（macOS 侧经 Decodable 自然校验）。
    ),
    ("encoder_status", '{"event":"encoder_status","present":true}',
     {"event": "encoder_status", "present": True}),
    ("battery_status", '{"event":"battery_status","level":87,"charging":false,"usb_powered":true}',
     {"event": "battery_status", "level": 87, "charging": False, "usb_powered": True}),
    ("button_down_session", '{"event":"button_down","button":"primary","session_id":1234}',
     {"event": "button_down", "button": "primary", "session_id": 1234}),
    ("button_up_session",
     '{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}',
     {"event": "button_up", "button": "primary", "duration_ms": 620, "session_id": 1234}),
    ("button_down_secondary", '{"event":"button_down","button":"secondary"}',
     {"event": "button_down", "button": "secondary"}),
    ("button_up_secondary", '{"event":"button_up","button":"secondary","duration_ms":90}',
     {"event": "button_up", "button": "secondary", "duration_ms": 90}),
    ("button_double_click", '{"event":"button_double_click","button":"primary"}',
     {"event": "button_double_click", "button": "primary"}),
    ("button_click_encoder",
     '{"event":"button_click","button":"primary","duration_ms":131,"source":"encoder"}',
     {"event": "button_click", "button": "primary", "duration_ms": 131, "source": "encoder"}),
    ("tap", '{"event":"tap","button":"double"}',
     {"event": "tap", "button": "double"}),
    ("encoder_rotate", '{"event":"encoder_rotate","direction":"cw","steps":2}',
     {"event": "encoder_rotate", "direction": "cw", "steps": 2}),
    ("gateway_key", '{"event":"gateway_key","key":"back","pressed":true}',
     {"event": "gateway_key", "key": "back", "pressed": True}),
    # routes 数组 Windows 暂无解析（backlog D10），期望集只取 event；键序两端各自校验。
    ("gateway_keymap", '{"event":"gateway_keymap","routes":[{"key":"back","route":"software"}]}',
     {"event": "gateway_keymap"}),
    ("gateway_status", '{"event":"gateway_status","mode":"gateway"}',
     {"event": "gateway_status", "mode": "gateway"}),
]

POWER_MGMT = [
    ("power_mgmt_on", '{"event":"power_mgmt","usb_auto_off":true}',
     {"event": "power_mgmt", "usb_auto_off": True}),
    ("power_mgmt_off", '{"event":"power_mgmt","usb_auto_off":false}',
     {"event": "power_mgmt", "usb_auto_off": False}),
]

OTA_STATE = [
    ("ota_ready", '{"event":"ready","transfer_id":1,"size":1385760,"partition":"ota_1"}',
     {"event": "ready", "transfer_id": 1, "size": 1385760}),
    ("ota_progress", '{"event":"progress","transfer_id":1,"written":32768,"size":1385760}',
     {"event": "progress", "transfer_id": 1, "written": 32768, "size": 1385760}),
    ("ota_done", '{"event":"done","transfer_id":1,"reboot_ms":500}',
     {"event": "done", "transfer_id": 1, "reboot_ms": 500}),
    ("ota_error", '{"event":"error","code":"bad_offset","esp_err":258}',
     {"event": "error", "code": "bad_offset", "esp_err": 258}),
    ("ota_aborted", '{"event":"aborted"}', {"event": "aborted"}),
    # 注：partition 字段仅部分端暴露（Windows FirmwareOtaStateEvent 无此字段），
    # 保留在帧内以同时校验两端解码对未知字段的容忍。
]

# ---------------------------------------------------------------- 二进制帧
SESSION = 305419896  # 0x12345678
BINARY = [
    ("audio_start", "audio", audio_frame(SESSION, 1, 0x01, b"\xde\xad\xbe\xef"),
     {"session_id": SESSION, "seq": 1, "flags": 1, "payload_hex": "deadbeef"}),
    ("audio_data", "audio", audio_frame(SESSION, 2, 0x00, b"\x01\x02\x03"),
     {"session_id": SESSION, "seq": 2, "flags": 0, "payload_hex": "010203"}),
    ("audio_end", "audio", audio_frame(SESSION, 3, 0x02, b""),
     {"session_id": SESSION, "seq": 3, "flags": 2, "payload_hex": ""}),
    ("motion", "motion", motion_frame(-2000, 1234),
     {"dx": -2000, "dy": 1234}),
    ("motion_clamp_bound", "motion", motion_frame(8000, -8000),
     {"dx": 8000, "dy": -8000}),  # AIR_MOUSE_MAX_DELTA 边界
]

# ---------------------------------------------------------------- control 构建
# expect 为完整对象语义（键序无关）；args 为各端构建器入参。
CONTROL = [
    ("ui_state_ready", "ui_state", {"state": "ready", "text": ""},
     {"event": "ui_state", "state": "ready", "text": ""}),
    ("ui_state_thinking", "ui_state", {"state": "thinking", "text": "partial text"},
     {"event": "ui_state", "state": "thinking", "text": "partial text"}),
    ("interaction_mode_hold", "interaction_mode", {"mode": "hold_to_talk"},
     {"event": "interaction_mode", "mode": "hold_to_talk"}),
    ("show_imu_debug", "show_imu_debug", {"enabled": True},
     {"event": "show_imu_debug", "enabled": True}),
    ("imu_wake_sensitivity", "imu_wake_sensitivity", {"threshold": 500},
     {"event": "imu_wake_sensitivity", "threshold": 500}),
    ("tap_enabled", "tap_enabled", {"enabled": True},
     {"event": "tap_enabled", "enabled": True}),
    ("tap_sensitivity", "tap_sensitivity", {"level": 5},
     {"event": "tap_sensitivity", "level": 5}),
    ("encoder_led_color", "encoder_led_color", {"color": "red"},
     {"event": "encoder_led_color", "color": "red"}),
    ("encoder_recording_gate", "encoder_recording_gate", {"enabled": True},
     {"event": "encoder_recording_gate", "enabled": True}),
    ("gateway_keymap_set", "gateway_keymap_set", {"key": "back", "route": "software"},
     {"event": "gateway_keymap_set", "key": "back", "route": "software"}),
    ("gateway_target_info", "gateway_target_info", {"name": "DESKTOP-CI"},
     {"event": "gateway_target_info", "name": "DESKTOP-CI"}),
    ("air_mouse_enabled", "air_mouse_enabled", {"enabled": True},
     {"event": "air_mouse_enabled", "enabled": True}),
    ("usb_auto_off", "usb_auto_off", {"enabled": True},
     {"event": "usb_auto_off", "enabled": True}),
    ("battery_status_request", "battery_status_request", {},
     {"event": "battery_status_request"}),
    ("remote_button_down", "remote_button",
     {"action": "down", "button": "primary", "source": "global_hotkey", "request_id": 7},
     {"event": "remote_button_down", "button": "primary", "source": "global_hotkey",
      "request_id": 7}),
    ("remote_button_up", "remote_button",
     {"action": "up", "button": "primary", "source": "global_hotkey", "request_id": 7},
     {"event": "remote_button_up", "button": "primary", "source": "global_hotkey",
      "request_id": 7}),
    ("power_log_dump", "power_log_dump", {"offset": 0, "max": 65536},
     {"power_log": {"cmd": "dump", "offset": 0, "max": 65536}}),
    ("power_log_clear", "power_log_clear", {},
     {"power_log": {"cmd": "clear"}}),
]

# ---------------------------------------------------------------- OTA 控制（二进制，字节级黄金）
CHUNK = b"\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8"
OTA_CONTROL = [
    ("ota_begin", "ota_begin", {"image_size": 1385760, "transfer_id": 1},
     ota_begin(1385760, 1)),
    ("ota_data", "ota_data", {"transfer_id": 1, "offset": 12288, "chunk_hex": hx(CHUNK)},
     ota_data(1, 12288, CHUNK)),
    ("ota_end", "ota_end", {"transfer_id": 1, "image_size": 1385760},
     ota_end(1, 1385760)),
    ("ota_abort", "ota_abort", {"transfer_id": 1}, ota_abort(1)),
]


def main() -> None:
    manifest = {
        "schema": 1,
        "generated_by": "tests/contract/generate_fixtures.py",
        "spec": "Doc/Ref/protocol.md",
        "state_frames": [
            {"name": n, "hex": hx(state_frame(js)), "expect": exp} for n, js, exp in STATE
        ],
        "power_mgmt_frames": [
            {"name": n, "hex": hx(state_frame(js)), "expect": exp}
            for n, js, exp in POWER_MGMT
        ],
        "ota_state_frames": [
            {"name": n, "hex": hx(ota_state_frame(js)), "expect": exp}
            for n, js, exp in OTA_STATE
        ],
        "binary_frames": [
            {"name": n, "kind": k, "hex": hx(b), "expect": exp}
            for n, k, b, exp in BINARY
        ],
        "control_payloads": [
            {"name": n, "kind": k, "args": args, "expect": exp}
            for n, k, args, exp in CONTROL
        ],
        "ota_control_frames": [
            {"name": n, "kind": k, "args": args, "hex": hx(b)}
            for n, k, args, b in OTA_CONTROL
        ],
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                   encoding="utf-8")
    total = sum(len(v) for k, v in manifest.items()
                if isinstance(v, list))
    print(f"wrote {OUT} ({total} fixtures)")


if __name__ == "__main__":
    main()
