#!/usr/bin/env python3
"""mirror_urls.py 的单元测试（COS 镜像路径映射，纯函数不联网）。

运行：python scripts/test_mirror_urls.py
"""
import importlib.util
import sys
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("mirror_urls.py")


def load_module():
    spec = importlib.util.spec_from_file_location("mirror_urls", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mod = load_module()

MIRROR = "https://dl.davenger.cloud"


class MirrorClassTests(unittest.TestCase):
    def test_windows_assets(self):
        self.assertEqual(mod.mirror_class("VoiceStick_2.3.8_zh-CN.msi"), "windows")
        self.assertEqual(mod.mirror_class("VoiceStick_Portable_v2.3.8.zip"), "windows")

    def test_macos_assets(self):
        self.assertEqual(mod.mirror_class("VoiceStick-2.3.8.zip"), "macos")
        self.assertEqual(mod.mirror_class("VoiceStick-2.3.8.dmg"), "macos")

    def test_firmware_assets(self):
        self.assertEqual(
            mod.mirror_class("voicestick-firmware-sticks3-ota-2.3.8.bin"), "firmware")

    def test_checksum_follows_primary_asset(self):
        # .sha256 附属文件跟随主资产同目录（去掉 .sha256 后缀再分类）
        self.assertEqual(mod.mirror_class("VoiceStick_2.3.8_en-US.msi.sha256"), "windows")
        self.assertEqual(
            mod.mirror_class("voicestick-firmware-sticks3-ota-2.3.8.bin.sha256"), "firmware")

    def test_manifest_json_follows_firmware(self):
        # manifest.json 与固件资产同目录存放（firmware/<tag>/manifest.json）
        self.assertEqual(mod.mirror_class("manifest.json"), "firmware")

    def test_unknown_asset_has_no_mirror_class(self):
        self.assertIsNone(mod.mirror_class("notes.txt"))


class MirrorAssetUrlTests(unittest.TestCase):
    def test_msi_url_mapped_to_windows_dir(self):
        self.assertEqual(
            mod.mirror_asset_url(
                "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                "VoiceStick_2.3.8_en-US.msi", MIRROR),
            f"{MIRROR}/windows/v2.3.8/VoiceStick_2.3.8_en-US.msi",
        )

    def test_firmware_url_keeps_tag_dir(self):
        self.assertEqual(
            mod.mirror_asset_url(
                "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                "voicestick-firmware-sticks3-ota-2.3.8.bin", MIRROR),
            f"{MIRROR}/firmware/v2.3.8/voicestick-firmware-sticks3-ota-2.3.8.bin",
        )

    def test_macos_zip_mapped(self):
        self.assertEqual(
            mod.mirror_asset_url(
                "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                "VoiceStick-2.3.8.zip", MIRROR),
            f"{MIRROR}/macos/v2.3.8/VoiceStick-2.3.8.zip",
        )

    def test_checksum_url_follows_primary(self):
        self.assertEqual(
            mod.mirror_asset_url(
                "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                "VoiceStick_2.3.8_en-US.msi.sha256", MIRROR),
            f"{MIRROR}/windows/v2.3.8/VoiceStick_2.3.8_en-US.msi.sha256",
        )

    def test_non_github_url_returned_unchanged(self):
        # 非 GitHub Release 资产 URL（自定义注入）不做改写
        url = "https://example.test/ota.bin"
        self.assertEqual(mod.mirror_asset_url(url, MIRROR), url)

    def test_unknown_asset_url_returned_unchanged(self):
        # 未知资产名不镜像，保留源 URL（宁可回源也不指到不存在的镜像路径）
        url = "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/notes.txt"
        self.assertEqual(mod.mirror_asset_url(url, MIRROR), url)

    def test_mirror_base_trailing_slash_normalized(self):
        self.assertEqual(
            mod.mirror_asset_url(
                "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.3.8/"
                "VoiceStick_2.3.8_en-US.msi", MIRROR + "/"),
            f"{MIRROR}/windows/v2.3.8/VoiceStick_2.3.8_en-US.msi",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
