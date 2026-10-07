#!/usr/bin/env python3
"""scripts/release_guard.py 的单元测试（纯逻辑，不联网）。

运行：python3 scripts/test_release_guard.py
"""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("release_guard.py")


def load_module():
    spec = importlib.util.spec_from_file_location("release_guard", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


guard = load_module()

SERVICE_UUID = guard.SERVICE_UUID
# 与固件 voice_ble.c 的 BLE_UUID128_INIT 小端字节序一致（16 字节）
SERVICE_BYTES_LE = [0x00, 0x51, 0xFC, 0xEA, 0x3C, 0x3A, 0xF7, 0x88,
                    0x23, 0x4B, 0x6F, 0x6E, 0x84, 0x0B, 0x2F, 0x8F]

HUB_BODY = """
本文件是面向 AI 编码助手的**核心枢纽（Hub）**。

{self_ref}是本文的同源副本，三者内容一致，修改整体性内容时同步更新三份，避免漂移。

## 项目概览

- 红线一
- 红线二
"""

AGENTS_HUB = "# VoiceStick — Agent 工作指南\n\n" + HUB_BODY.format(
    self_ref="`CLAUDE.md`（Claude Code）与 `CODEBUDDY.md`（CodeBuddy）"
)
CLAUDE_HUB = (
    "# CLAUDE.md\n\n"
    "This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.\n\n"
    + HUB_BODY.format(self_ref="`AGENTS.md`（通用 AI 编码助手）与 `CODEBUDDY.md`（CodeBuddy）")
)
CODEBUDDY_HUB = (
    "# CODEBUDDY.md\n\n"
    "This file provides guidance to CodeBuddy Code when working with code in this repository.\n\n"
    + HUB_BODY.format(self_ref="`AGENTS.md`（通用 AI 编码助手）与 `CLAUDE.md`（Claude Code）")
)


def build_repo(root: Path) -> None:
    """构造一个全绿的最小仓库。"""
    (root / "firmware" / "components" / "audio_pipeline").mkdir(parents=True)
    (root / "firmware" / "components" / "voice_ble").mkdir(parents=True)
    (root / "Doc" / "Ref").mkdir(parents=True)
    (root / "desktop" / "windows" / "src").mkdir(parents=True)
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore").mkdir(parents=True)
    (root / "desktop" / "macos" / "Sources" / "VoiceStickApp").mkdir(parents=True)
    (root / "website" / "src" / "i18n").mkdir(parents=True)
    (root / "website" / "public").mkdir(parents=True)

    (root / "VERSION").write_text("2.4.9", encoding="utf-8")
    (root / "firmware" / "version.txt").write_text("2.4.9", encoding="utf-8")
    (root / "CHANGELOG.md").write_text(
        "# CHANGELOG\n\n## v2.4.9\n\n- fix\n\n## v2.4.8\n", encoding="utf-8"
    )
    (root / "AGENTS.md").write_text(AGENTS_HUB, encoding="utf-8")
    (root / "CLAUDE.md").write_text(CLAUDE_HUB, encoding="utf-8")
    (root / "CODEBUDDY.md").write_text(CODEBUDDY_HUB, encoding="utf-8")

    (root / "website" / "src" / "i18n" / "zh-CN.json").write_text(
        json.dumps({"a": {"b": "你好"}, "c": "世界"}, ensure_ascii=False), encoding="utf-8"
    )
    (root / "website" / "src" / "i18n" / "en-US.json").write_text(
        json.dumps({"a": {"b": "hi"}, "c": "world"}, ensure_ascii=False), encoding="utf-8"
    )
    (root / "website" / "public" / "appcast.xml").write_text(
        '<rss><sparkle:version="2.4.9"/></rss>', encoding="utf-8"
    )

    hex_bytes = ", ".join(f"0x{x:02X}" for x in SERVICE_BYTES_LE)
    (root / "firmware" / "components" / "voice_ble" / "voice_ble.c").write_text(
        "static const ble_uuid128_t s_service_uuid =\n"
        f"    BLE_UUID128_INIT({hex_bytes});\n",
        encoding="utf-8",
    )
    (root / "firmware" / "components" / "audio_pipeline" / "audio_pipeline.c").write_text(
        "#define AUDIO_FRAME_MS 40\n", encoding="utf-8"
    )
    (root / "Doc" / "Ref" / "protocol.md").write_text(
        f"service {SERVICE_UUID}\n"
        "The firmware currently encodes 40 ms of 16 kHz mono audio per packet.\n",
        encoding="utf-8",
    )
    (root / "desktop" / "windows" / "src" / "ble_protocol.h").write_text(
        f'static constexpr const wchar_t* service_uuid = L"{SERVICE_UUID}";\n',
        encoding="utf-8",
    )
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "BleProtocol.swift").write_text(
        f'public static let serviceUUID = "{SERVICE_UUID.upper()}"\n', encoding="utf-8"
    )
    (root / "desktop" / "windows" / "src" / "audio_opus_encoder.h").write_text(
        "static constexpr int kFrameSamples = 640;  // 40 ms\n", encoding="utf-8"
    )
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "AudioOpusEncoder.swift").write_text(
        "public static let frameSamples = 640  // 40 ms\n", encoding="utf-8"
    )
    (root / "desktop" / "windows" / "src" / "ogg_opus_muxer.cc").write_text(
        "granule_position_ += static_cast<std::uint64_t>(AudioOpusEncoder::kFrameSamples)"
        " * 48000 / sample_rate_;\n",
        encoding="utf-8",
    )
    (root / "desktop" / "macos" / "Sources" / "VoiceStickApp" / "OggOpusMuxer.swift").write_text(
        "granulePosition += UInt64(AudioOpusEncoder.frameSamples * 48_000 / sampleRate)\n",
        encoding="utf-8",
    )


class ReleaseGuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        build_repo(self.root)
        self.addCleanup(self.tmp.cleanup)

    def failures(self):
        return guard.run(self.root, verbose=False)

    def test_baseline_all_green(self):
        self.assertEqual(self.failures(), [])

    def test_version_mismatch_detected(self):
        (self.root / "firmware" / "version.txt").write_text("2.4.8", encoding="utf-8")
        fails = self.failures()
        self.assertTrue(any(f.startswith("versions") for f in fails), fails)

    def test_changelog_lag_detected(self):
        (self.root / "CHANGELOG.md").write_text("## v2.4.8\n", encoding="utf-8")
        fails = self.failures()
        self.assertTrue(any("CHANGELOG" in f for f in fails), fails)

    def test_hub_drift_detected(self):
        drifted = CLAUDE_HUB.replace("- 红线二", "- 红线二\n- 只有 CLAUDE 有的行")
        (self.root / "CLAUDE.md").write_text(drifted, encoding="utf-8")
        fails = self.failures()
        self.assertTrue(any(f.startswith("hub") for f in fails), fails)

    def test_hub_blank_line_drift_detected(self):
        # 在两条 bullet 之间插入空行（与既有空行不相邻，不会被折叠规则吞掉）
        drifted = CLAUDE_HUB.replace("- 红线一", "- 红线一\n")
        (self.root / "CLAUDE.md").write_text(drifted, encoding="utf-8")
        fails = self.failures()
        self.assertTrue(any(f.startswith("hub") for f in fails), fails)

    def test_hub_identical_modulo_self_reference_passes(self):
        # 三份自指句不同但正文一致 → 必须通过
        self.assertEqual(self.failures(), [])

    def test_i18n_missing_key_detected(self):
        (self.root / "website" / "src" / "i18n" / "en-US.json").write_text(
            json.dumps({"a": {"b": "hi"}}, ensure_ascii=False), encoding="utf-8"
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("i18n") for f in fails), fails)

    def test_uuid_byte_order_mismatch_detected(self):
        bad = SERVICE_BYTES_LE[::-1]  # 端序反了
        hex_bytes = ", ".join(f"0x{x:02X}" for x in bad)
        (self.root / "firmware" / "components" / "voice_ble" / "voice_ble.c").write_text(
            f"static const ble_uuid128_t s_service_uuid =\n    BLE_UUID128_INIT({hex_bytes});\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("uuid") for f in fails), fails)

    def test_protocol_frame_ms_drift_detected(self):
        (self.root / "Doc" / "Ref" / "protocol.md").write_text(
            f"service {SERVICE_UUID}\nencodes 60 ms of 16 kHz mono audio per packet.\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("frame-ms") for f in fails), fails)

    def test_stale_granule_960_detected(self):
        # 历史缺陷回归用例：960 采样（60ms 口径）必须被拦截
        (self.root / "desktop" / "windows" / "src" / "ogg_opus_muxer.cc").write_text(
            "granule_position_ += static_cast<std::uint64_t>(960 * 48000 / sample_rate_);\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("frame-ms") for f in fails), fails)

    def test_mac_stale_granule_detected(self):
        (self.root / "desktop" / "macos" / "Sources" / "VoiceStickApp" / "OggOpusMuxer.swift").write_text(
            "granulePosition += UInt64(960 * 48_000 / sampleRate)\n", encoding="utf-8"
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("frame-ms") for f in fails), fails)

    def test_appcast_ahead_detected(self):
        (self.root / "website" / "public" / "appcast.xml").write_text(
            '<rss><sparkle:version="9.9.9"/></rss>', encoding="utf-8"
        )
        fails = self.failures()
        self.assertTrue(any(f.startswith("appcast") for f in fails), fails)

    def test_appcast_lag_is_tolerated(self):
        # 发布流程中 appcast 晚于 VERSION 属正常窗口，只拦“超前”
        (self.root / "website" / "public" / "appcast.xml").write_text(
            '<rss><sparkle:version="2.4.6"/></rss>', encoding="utf-8"
        )
        self.assertEqual(self.failures(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
