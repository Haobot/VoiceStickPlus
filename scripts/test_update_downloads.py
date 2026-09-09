#!/usr/bin/env python3
"""update-downloads.py 的单元测试（纯转换逻辑，不联网）。

运行：python scripts/test_update_downloads.py
"""
import importlib.util
import json
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("update-downloads.py")


def load_module():
    spec = importlib.util.spec_from_file_location("update_downloads", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mod = load_module()


def make_release(tag, assets, body="说明", is_prerelease=False, is_latest=False,
                 published_at="2026-09-07T10:00:00Z"):
    return {
        "tagName": tag,
        "publishedAt": published_at,
        "isPrerelease": is_prerelease,
        "isLatest": is_latest,
        "body": body,
        "assets": [
            {
                "name": name,
                "size": size,
                "url": f"https://github.com/Haobot/VoiceStickPlus/releases/download/{tag}/{name}",
            }
            for name, size in assets
        ],
    }


LATEST = make_release(
    "v2.3.8",
    [
        ("VoiceStick_2.3.8_zh-CN.msi", 100),
        ("VoiceStick_2.3.8_en-US.msi", 101),
        ("voicestick-firmware-sticks3-ota-2.3.8.bin", 200),
        ("voicestick-firmware-sticks3-ota-2.3.8.bin.sha256", 1),
        ("voicestick-firmware-sticks3-merged-2.3.8.bin", 300),
        ("voicestick-firmware-sticks3-merged-2.3.8.bin.sha256", 1),
        ("manifest.json", 2),
    ],
    body="## 2.3.8\n- 修复若干问题",
    is_latest=True,
)
OLDER = make_release(
    "v2.3.7",
    [("voicestick-firmware-sticks3-ota-2.3.7.bin", 200)],
    published_at="2026-08-01T10:00:00Z",
)
DOWNLOADS = mod.build_downloads([LATEST, OLDER], "2.3.0", "2026-09-07T12:00:00Z")
MIRRORED = mod.build_downloads(
    [LATEST, OLDER], "2.3.0", "2026-09-07T12:00:00Z",
    mirror_base="https://dl.davenger.cloud",
)


class ClassifyAssetTests(unittest.TestCase):
    def test_windows_assets(self):
        self.assertEqual(mod.classify_asset("VoiceStick_2.3.8_zh-CN.msi"), "windows")
        self.assertEqual(mod.classify_asset("VoiceStick_Portable_v2.3.8.zip"), "windows")

    def test_macos_assets(self):
        self.assertEqual(mod.classify_asset("VoiceStick-2.3.8.zip"), "macos")
        self.assertEqual(mod.classify_asset("VoiceStick-2.3.8.dmg"), "macos")

    def test_firmware_assets(self):
        self.assertEqual(
            mod.classify_asset("voicestick-firmware-sticks3-ota-2.3.8.bin"), "firmware")

    def test_auxiliary_assets_are_not_listed(self):
        # 校验和附属文件与 manifest.json 不作为独立下载项
        self.assertIsNone(mod.classify_asset("voicestick-firmware-sticks3-ota-2.3.8.bin.sha256"))
        self.assertIsNone(mod.classify_asset("manifest.json"))

    def test_unknown_asset(self):
        self.assertIsNone(mod.classify_asset("notes.txt"))


class BuildDownloadsTests(unittest.TestCase):
    def test_latest_picks_is_latest_flag(self):
        self.assertEqual(DOWNLOADS["latest"]["version"], "2.3.8")

    def test_latest_carries_min_firmware_version_only(self):
        self.assertEqual(DOWNLOADS["latest"]["min_firmware_version"], "2.3.0")
        for entry in DOWNLOADS["releases"]:
            self.assertNotIn("min_firmware_version", entry)

    def test_manifest_and_checksum_files_excluded_from_assets(self):
        names = [a["name"] for a in DOWNLOADS["latest"]["assets"]]
        self.assertNotIn("manifest.json", names)
        self.assertFalse(any(n.endswith(".sha256") for n in names))

    def test_firmware_asset_links_checksum_file(self):
        fw = [a for a in DOWNLOADS["latest"]["assets"] if a["platform"] == "firmware"]
        by_name = {a["name"]: a for a in fw}
        self.assertEqual(
            by_name["voicestick-firmware-sticks3-ota-2.3.8.bin"]["sha256"],
            "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
            "voicestick-firmware-sticks3-ota-2.3.8.bin.sha256",
        )
        self.assertEqual(by_name["voicestick-firmware-sticks3-merged-2.3.8.bin"]["sha256"],
                         "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                         "voicestick-firmware-sticks3-merged-2.3.8.bin.sha256")

    def test_msi_without_checksum_file_is_none(self):
        msi = [a for a in DOWNLOADS["latest"]["assets"] if a["platform"] == "windows"]
        by_name = {a["name"]: a for a in msi}
        self.assertIsNone(by_name["VoiceStick_2.3.8_zh-CN.msi"]["sha256"])

    def test_size_and_url_preserved(self):
        msi = next(a for a in DOWNLOADS["latest"]["assets"]
                   if a["name"] == "VoiceStick_2.3.8_en-US.msi")
        self.assertEqual(msi["size"], 101)
        self.assertTrue(msi["url"].endswith("VoiceStick_2.3.8_en-US.msi"))

    def test_notes_and_date_and_prerelease_flags(self):
        self.assertIn("2.3.8", DOWNLOADS["latest"]["notes"])
        self.assertEqual(DOWNLOADS["latest"]["date"], "2026-09-07T10:00:00Z")
        self.assertFalse(DOWNLOADS["latest"]["prerelease"])
        pre = mod.build_downloads(
            [make_release("v2.4.0-beta", [("VoiceStick_2.4.0-beta_en-US.msi", 100)],
                          is_prerelease=True)],
            "2.3.0", "2026-09-07T12:00:00Z",
        )
        self.assertTrue(pre["latest"]["prerelease"])

    def test_all_releases_listed_in_order(self):
        self.assertEqual([r["version"] for r in DOWNLOADS["releases"]], ["2.3.8", "2.3.7"])

    def test_empty_assets_release(self):
        data = mod.build_downloads([make_release("v1.0.0", [])], "0.3.0", "now")
        self.assertEqual(data["latest"]["assets"], [])
        self.assertEqual(data["releases"][0]["assets"], [])

    def test_generated_at_recorded(self):
        self.assertEqual(DOWNLOADS["generated_at"], "2026-09-07T12:00:00Z")

    def test_output_is_json_serializable(self):
        json.dumps(DOWNLOADS)


class MirrorBaseTests(unittest.TestCase):
    """mirror_base（COS 国内分发面）下的 URL 改写。"""

    def test_assets_use_mirror_urls(self):
        assets = {a["name"]: a for a in MIRRORED["latest"]["assets"]}
        self.assertEqual(
            assets["VoiceStick_2.3.8_en-US.msi"]["url"],
            "https://dl.davenger.cloud/windows/v2.3.8/VoiceStick_2.3.8_en-US.msi")
        self.assertEqual(
            assets["voicestick-firmware-sticks3-ota-2.3.8.bin"]["url"],
            "https://dl.davenger.cloud/firmware/v2.3.8/"
            "voicestick-firmware-sticks3-ota-2.3.8.bin")

    def test_checksum_links_use_mirror_urls(self):
        fw = {a["name"]: a for a in MIRRORED["latest"]["assets"]
              if a["platform"] == "firmware"}
        self.assertEqual(
            fw["voicestick-firmware-sticks3-ota-2.3.8.bin"]["sha256"],
            "https://dl.davenger.cloud/firmware/v2.3.8/"
            "voicestick-firmware-sticks3-ota-2.3.8.bin.sha256")

    def test_mirror_applies_to_all_releases(self):
        older = MIRRORED["releases"][1]["assets"][0]
        self.assertEqual(
            older["url"],
            "https://dl.davenger.cloud/firmware/v2.3.7/"
            "voicestick-firmware-sticks3-ota-2.3.7.bin")

    def test_without_mirror_urls_unchanged(self):
        # 不传 mirror_base 时行为与现状一致（GitHub 直链）
        assets = {a["name"]: a for a in DOWNLOADS["latest"]["assets"]}
        self.assertEqual(
            assets["VoiceStick_2.3.8_en-US.msi"]["url"],
            "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
            "VoiceStick_2.3.8_en-US.msi")


if __name__ == "__main__":
    unittest.main(verbosity=2)
