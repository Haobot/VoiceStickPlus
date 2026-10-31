#!/usr/bin/env python3
"""publish_cos（本机发布 COS 直传）的单元测试（纯逻辑，不联网不导 SDK）。

运行：python scripts/test_publish_cos.py
"""
import importlib.util
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent


def load_module(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pub = load_module("publish_cos")


class FirmwareManifestTests(unittest.TestCase):
    """manifest 字段结构必须与 release.yml 旧版逐字段一致（客户端校验依赖）。"""

    def test_manifest_matches_ci_schema(self):
        m = pub.build_firmware_manifest(
            version="2.4.0", min_version="0.3.0", tag="v2.4.0",
            dist_domain="https://dl.davenger.cloud",
            download_base="https://github.com/Haobot/VoiceStickPlus/releases/download/v2.4.0",
            ota_name="voicestick-firmware-sticks3-ota-2.4.0.bin",
            merged_name="voicestick-firmware-sticks3-merged-2.4.0.bin",
            ota_sha="a" * 64, ota_size=1234,
            merged_sha="b" * 64, merged_size=5678)
        # 逐字段断言：键名、顺序与 CI 旧版 manifest 完全一致
        self.assertEqual(list(m.keys()), [
            "hardware", "version", "min_version",
            "ota_url", "ota_url_fallback", "ota_sha256", "ota_size",
            "merged_url", "merged_url_fallback", "merged_sha256", "merged_size",
        ])
        self.assertEqual(m["hardware"], "stick_s3")
        self.assertEqual(m["version"], "2.4.0")
        self.assertEqual(m["min_version"], "0.3.0")
        self.assertEqual(m["ota_url"],
                         "https://dl.davenger.cloud/firmware/v2.4.0/voicestick-firmware-sticks3-ota-2.4.0.bin")
        self.assertEqual(m["ota_url_fallback"],
                         "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.4.0/voicestick-firmware-sticks3-ota-2.4.0.bin")
        self.assertEqual(m["ota_sha256"], "a" * 64)
        self.assertEqual(m["ota_size"], 1234)          # int，非字符串
        self.assertEqual(m["merged_url"],
                         "https://dl.davenger.cloud/firmware/v2.4.0/voicestick-firmware-sticks3-merged-2.4.0.bin")
        self.assertEqual(m["merged_url_fallback"],
                         "https://github.com/Haobot/VoiceStickPlus/releases/download/v2.4.0/voicestick-firmware-sticks3-merged-2.4.0.bin")
        self.assertEqual(m["merged_sha256"], "b" * 64)
        self.assertEqual(m["merged_size"], 5678)


class FirmwareAssetPlanTests(unittest.TestCase):
    """固件资产 -> COS 键的编排（含 firmware/latest/manifest.json 稳定地址别名）。"""

    def test_plan_lists_bins_sha_and_manifest_alias(self):
        dist = Path("/tmp/dist")  # 路径只参与拼接，不触碰文件系统
        files = [
            (dist / "voicestick-firmware-sticks3-ota-2.4.0.bin", "ota"),
            (dist / "voicestick-firmware-sticks3-ota-2.4.0.bin.sha256", "ota_sha"),
            (dist / "voicestick-firmware-sticks3-merged-2.4.0.bin", "merged"),
            (dist / "voicestick-firmware-sticks3-merged-2.4.0.bin.sha256", "merged_sha"),
            (dist / "manifest.json", "manifest"),
        ]
        plan = pub.firmware_upload_plan(files, tag="v2.4.0")
        keys = [key for _, key in plan]
        self.assertEqual(keys, [
            "firmware/v2.4.0/voicestick-firmware-sticks3-ota-2.4.0.bin",
            "firmware/v2.4.0/voicestick-firmware-sticks3-ota-2.4.0.bin.sha256",
            "firmware/v2.4.0/voicestick-firmware-sticks3-merged-2.4.0.bin",
            "firmware/v2.4.0/voicestick-firmware-sticks3-merged-2.4.0.bin.sha256",
            "firmware/v2.4.0/manifest.json",
            "firmware/latest/manifest.json",   # 稳定地址别名，紧跟 manifest 本体
        ])

    def test_software_upload_plan(self):
        plan = pub.software_upload_plan(
            [Path("/tmp/VoiceStick_2.4.0_zh-CN.msi"),
             Path("/tmp/VoiceStick_2.4.0_zh-CN.msi.sha256")],
            tag="v2.4.0")
        self.assertEqual([key for _, key in plan], [
            "software/windows/v2.4.0/VoiceStick_2.4.0_zh-CN.msi",
            "software/windows/v2.4.0/VoiceStick_2.4.0_zh-CN.msi.sha256",
        ])


if __name__ == "__main__":
    unittest.main(verbosity=2)
