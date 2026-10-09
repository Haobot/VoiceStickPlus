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
    (root / 'Doc' / 'Plan').mkdir(parents=True, exist_ok=True)
    (root / 'Doc' / 'Plan' / 'backlog.md').write_text(
        '> 状态：closed（样例载体）' + chr(10) + chr(10) + '# backlog' + chr(10)
        + '| ID | 事项 | 状态 | 证据 |' + chr(10)
        + '|---|---|---|---|' + chr(10)
        + '| T1 | 样例 | closed | 全过 |' + chr(10)
        + '| T2 | 样例 | open | 待办 |' + chr(10)
        + '| 2026-10-08 | 闭环记录样例 | 证据... |' + chr(10),
        encoding='utf-8',
    )
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
    # d11-append: event literals for protocol-events fixture
    with (root / "firmware" / "components" / "voice_ble" / "voice_ble.c").open(
            "a", encoding="utf-8") as fh:
        fh.write("const char* ev_state = " + chr(34) + "device_info" + chr(34) + "; const char* ev_ctrl = " + chr(34) + "gateway_keymap_set" + chr(34) + ";" + chr(10))
    # d11-append-2: 最小 control schema（与 protocol 示例对齐）
    import json as _j
    (root / "Doc" / "Ref" / "control-frame-schema.json").write_text(
        _j.dumps({"events": {"gateway_keymap_set": {"type": "object", "properties": {}, "required": [], "additionalProperties": False}}}) + chr(10), encoding="utf-8",
    )
    (root / "firmware" / "components" / "audio_pipeline" / "audio_pipeline.c").write_text(
        "#define AUDIO_FRAME_MS 40\n", encoding="utf-8"
    )
    (root / "Doc" / "Ref" / "protocol.md").write_text(
        f"service {SERVICE_UUID}\n"
        "The firmware currently encodes 40 ms of 16 kHz mono audio per packet.\n",
        encoding="utf-8",
    )
    # d11-append: State/Control 章节（protocol-events 切向）
    with (root / "Doc" / "Ref" / "protocol.md").open("a", encoding="utf-8") as fh:
        fh.write(chr(10) + "## State Event" + chr(10) + chr(96) * 3 + chr(10)
                 + '{"event":"device_info"}' + chr(10) + chr(96) * 3 + chr(10)
                 + chr(10) + "## Control Event" + chr(10) + chr(96) * 3 + chr(10)
                 + '{"event":"gateway_keymap_set"}' + chr(10) + chr(96) * 3 + chr(10))
    (root / "desktop" / "windows" / "src" / "ble_protocol.h").write_text(
        f'static constexpr const wchar_t* service_uuid = L"{SERVICE_UUID}";\n',
        encoding="utf-8",
    )
    # d11-append: event literals
    with (root / "desktop" / "windows" / "src" / "ble_protocol.h").open(
            "a", encoding="utf-8") as fh:
        fh.write("const char* ev_state = " + chr(34) + "device_info" + chr(34) + "; const char* ev_ctrl = " + chr(34) + "gateway_keymap_set" + chr(34) + ";" + chr(10))
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "BleProtocol.swift").write_text(
        f'public static let serviceUUID = "{SERVICE_UUID.upper()}"\n', encoding="utf-8"
    )
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "EventNames.swift").write_text(
        "let evState = " + chr(34) + "device_info" + chr(34)
        + "; let evCtrl = " + chr(34) + "gateway_keymap_set" + chr(34) + chr(10),
        encoding="utf-8",
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
    # N1 第二刀：OggOpusMuxer 已下沉 VoiceStickCore，夹具随迁
    (root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "OggOpusMuxer.swift").write_text(
        "granulePosition += UInt64(AudioOpusEncoder.frameSamples * 48_000 / sampleRate)\n",
        encoding="utf-8",
    )

    # N9 桌面本地化奇偶守护的夹具（枚举⇄EN⇄ZH 三集合齐全 → 基线绿）。
    (root / "desktop" / "windows" / "src" / "localization.h").write_text(
        "enum class StringId {\n    kOk,\n    kCancel,\n};\n", encoding="utf-8"
    )
    (root / "desktop" / "windows" / "src" / "localization.cc").write_text(
        "constexpr StringTable EnglishStrings() {\n"
        "    table[Index(StringId::kOk)] = \"OK\";\n"
        "    table[Index(StringId::kCancel)] = \"Cancel\";\n"
        "    return table;\n}\n"
        "constexpr StringTable ChineseStrings() {\n"
        "    table[Index(StringId::kOk)] = \"好\";\n"
        "    table[Index(StringId::kCancel)] = \"取消\";\n"
        "    return table;\n}\n"
        "constexpr StringTable kEnglish = EnglishStrings();\n"
        "constexpr StringTable kChinese = ChineseStrings();\n",
        encoding="utf-8",
    )
    (root / "desktop" / "macos" / "Sources" / "VoiceStickApp" / "Localization.swift").write_text(
        "enum L10nKey: String, CaseIterable {\n    case ok\n    case cancel\n}\n"
        "enum Localization {\n"
        "    private static let english: [L10nKey: String] = [\n"
        "        .ok: \"OK\",\n        .cancel: \"Cancel\",\n    ]\n"
        "    private static let chinese: [L10nKey: String] = [\n"
        "        .ok: \"好\",\n        .cancel: \"取消\",\n    ]\n}\n",
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

    def test_desktop_i18n_baseline_green(self):
        # 夹具三集合齐全 → 必须通过（与 test_baseline_all_green 互为呼应）。
        self.assertEqual(self.failures(), [])

    def test_desktop_i18n_windows_missing_zh_detected(self):
        p = self.root / "desktop" / "windows" / "src" / "localization.cc"
        p.write_text(
            "constexpr StringTable EnglishStrings() {\n"
            "    table[Index(StringId::kOk)] = \"OK\";\n"
            "    table[Index(StringId::kCancel)] = \"Cancel\";\n"
            "    return table;\n}\n"
            "constexpr StringTable ChineseStrings() {\n"
            "    table[Index(StringId::kOk)] = \"好\";\n"
            "    return table;\n}\n"
            "constexpr StringTable kEnglish = EnglishStrings();\n"
            "constexpr StringTable kChinese = ChineseStrings();\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("i18n-desktop") and "windows ZH" in f for f in fails), fails
        )

    def test_desktop_i18n_macos_missing_chinese_detected(self):
        p = self.root / "desktop" / "macos" / "Sources" / "VoiceStickApp" / "Localization.swift"
        p.write_text(
            "enum L10nKey: String, CaseIterable {\n    case ok\n    case cancel\n}\n"
            "enum Localization {\n"
            "    private static let english: [L10nKey: String] = [\n"
            "        .ok: \"OK\",\n        .cancel: \"Cancel\",\n    ]\n"
            "    private static let chinese: [L10nKey: String] = [\n"
            "        .ok: \"好\",\n    ]\n}\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("i18n-desktop") and "macos chinese" in f for f in fails), fails
        )

    def test_desktop_i18n_ghost_entry_detected(self):
        # 表里有、枚举已无（改名残留）→ 幽灵项必须被抓。
        p = self.root / "desktop" / "windows" / "src" / "localization.h"
        p.write_text(
            "enum class StringId {\n    kOk,\n    kCancel,\n    kLegacy,\n};\n",
            encoding="utf-8",
        )
        (self.root / "desktop" / "windows" / "src" / "localization.cc").write_text(
            "constexpr StringTable EnglishStrings() {\n"
            "    table[Index(StringId::kOk)] = \"OK\";\n"
            "    table[Index(StringId::kCancel)] = \"Cancel\";\n"
            "    table[Index(StringId::kGone)] = \"ghost\";\n"
            "    return table;\n}\n"
            "constexpr StringTable ChineseStrings() {\n"
            "    table[Index(StringId::kOk)] = \"好\";\n"
            "    table[Index(StringId::kCancel)] = \"取消\";\n"
            "    return table;\n}\n"
            "constexpr StringTable kEnglish = EnglishStrings();\n"
            "constexpr StringTable kChinese = ChineseStrings();\n",
            encoding="utf-8",
        )
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("i18n-desktop") and "幽灵" in f for f in fails), fails
        )

    def _write_plan(self, name: str, body: str) -> Path:
        d = self.root / "Doc" / "Plan"
        d.mkdir(parents=True, exist_ok=True)
        p = d / name
        p.write_text(body, encoding="utf-8")
        return p

    def test_doc_plan_status_marked_passes(self):
        self._write_plan(
            "a-plan.md", "# 标题\n\n> 状态：closed（示例）\n\n正文\n"
        )
        self.assertEqual(self.failures(), [])

    def test_doc_plan_status_missing_detected(self):
        self._write_plan("a-plan.md", "# 标题\n\n正文\n")
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("doc-plan-status") and "缺引用式状态行" in f for f in fails),
            fails,
        )

    def _write_test_cc(self, body: str) -> None:
        d = self.root / "desktop" / "windows" / "tests"
        d.mkdir(parents=True, exist_ok=True)
        (d / "core_tests_x.cc").write_text(body, encoding="utf-8")

    def test_test_ns_balance_balanced_passes(self):
        # 约定：ns 闭合必须是 col0 的 "} // namespace" 注释式（与仓库现状一致）
        self._write_test_cc(
            "namespace {\nint a;\n} // namespace\nint main() { return 0; }\n"
        )
        self.assertEqual(self.failures(), [])

    def test_test_ns_balance_unclosed_detected(self):
        # cut13 原型：闭合随 span 迁走 → 文件尾不配平
        self._write_test_cc(
            "namespace {\nint a;\n}\n"      # 开1闭0（第二段开未闭）
            "namespace {\nint b;\nint main() { return 0; }\n"
        )
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("test-ns") and "未配平" in f for f in fails), fails
        )
        self.assertTrue(
            any("main 不在全局域" in f for f in fails), fails
        )

    def test_test_ns_balance_stray_close_detected(self):
        # cut13 五修原型：新文件吞了别处的闭合 → 深度为负
        self._write_test_cc("} // namespace\nint main() { return 0; }\n")
        fails = self.failures()
        self.assertTrue(
            any(f.startswith("test-ns") and "深度为负" in f for f in fails), fails
        )

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
        # N1 第二刀：OggOpusMuxer 随迁 VoiceStickCore，篡改路径同步
        (self.root / "desktop" / "macos" / "Sources" / "VoiceStickCore" / "OggOpusMuxer.swift").write_text(
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

    def test_protocol_events_green(self):
        self.assertIsNone(guard.check_protocol_events(self.root))

    def test_protocol_events_missing_on_mac_detected(self):
        p = self.root / "desktop/macos/Sources/VoiceStickCore/EventNames.swift"
        p.write_text("// stripped", encoding="utf-8")
        err = guard.check_protocol_events(self.root)
        self.assertIsNotNone(err)
        self.assertIn("device_info", err)


    def test_control_frame_schema_green(self):
        self.assertIsNone(guard.check_control_frame_schema(self.root))

    def test_control_frame_schema_drift_detected(self):
        p = self.root / "Doc" / "Ref" / "protocol.md"
        orig = p.read_text(encoding="utf-8")
        old = '{"event":"gateway_keymap_set"}'
        new = '{"event":"gateway_keymap_set","key":"back"}'
        self.assertIn(old, orig)
        p.write_text(orig.replace(old, new), encoding="utf-8")
        err = guard.check_control_frame_schema(self.root)
        self.assertIsNotNone(err)
        self.assertIn("gateway_keymap_set", err)

    def test_backlog_structure_green(self):
        res = guard.check_backlog_structure(self.root)
        self.assertTrue(res is None or (isinstance(res, tuple) and res[0] == 'skip'), res)

    def test_backlog_broken_row_detected(self):
        p = self.root / 'Doc' / 'Plan' / 'backlog.md'
        t = p.read_text(encoding='utf-8')
        t = t.replace("| T2 | 样例 | open | 待办 |", "| T2 | 样例 | open | 待办")
        p.write_text(t, encoding='utf-8')
        res = guard.check_backlog_structure(self.root)
        self.assertIsNotNone(res)
        self.assertIn("行尾缺竖线", str(res))

    def test_backlog_bad_status_detected(self):
        p = self.root / 'Doc' / 'Plan' / 'backlog.md'
        t = p.read_text(encoding='utf-8')
        t = t.replace("| T1 | 样例 | closed | 全过 |", "| T1 | 样例 | closd | 全过 |")
        p.write_text(t, encoding='utf-8')
        res = guard.check_backlog_structure(self.root)
        self.assertIsNotNone(res)
        self.assertIn("closd", str(res))

if __name__ == "__main__":
    unittest.main(verbosity=2)
