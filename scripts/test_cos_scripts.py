#!/usr/bin/env python3
"""cos_uploader / mirror_release_to_cos 的单元测试（纯逻辑，不联网不导 SDK）。

运行：python scripts/test_cos_scripts.py
"""
import importlib.util
import os
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent


def load_module(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


uploader = load_module("cos_uploader")
mirror = load_module("mirror_release_to_cos")


class CacheControlTests(unittest.TestCase):
    def test_html_json_xml_no_cache(self):
        for key in ("index.html", "appcast.xml", "downloads.json",
                    "firmware/latest/manifest.json"):
            self.assertEqual(uploader.cache_control_for(key), "no-cache")

    def test_binary_assets_long_cache(self):
        # 版本化路径的 bin/msi/zip 内容不可变，长缓存安全
        for key in ("firmware/v2.3.10/ota.bin", "windows/v2.3.10/a.msi",
                    "macos/v2.3.10/VoiceStick-2.3.10.zip", "assets/app.1a2b3c.js"):
            self.assertEqual(uploader.cache_control_for(key), "max-age=31536000")


class RequireCredentialsTests(unittest.TestCase):
    def test_missing_credentials_exit_with_error(self):
        saved = {k: os.environ.pop(k, None) for k in
                 ("TENCENT_COS_SECRET_ID", "TENCENT_COS_SECRET_KEY")}
        try:
            with self.assertRaises(SystemExit) as ctx:
                uploader.require_credentials()
            self.assertIn("TENCENT_COS_SECRET_ID", str(ctx.exception))
        finally:
            for key, value in saved.items():
                if value is not None:
                    os.environ[key] = value

    def test_credentials_present(self):
        old_id = os.environ.get("TENCENT_COS_SECRET_ID")
        old_key = os.environ.get("TENCENT_COS_SECRET_KEY")
        os.environ["TENCENT_COS_SECRET_ID"] = "id"
        os.environ["TENCENT_COS_SECRET_KEY"] = "key"
        try:
            self.assertEqual(uploader.require_credentials(), ("id", "key"))
        finally:
            for key, old in (("TENCENT_COS_SECRET_ID", old_id),
                             ("TENCENT_COS_SECRET_KEY", old_key)):
                if old is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = old


class PlanMirrorAssetsTests(unittest.TestCase):
    def test_assets_mapped_and_skipped(self):
        release = {
            "tagName": "v2.3.8",
            "assets": [
                {"name": "VoiceStick_2.3.8_zh-CN.msi"},
                {"name": "VoiceStick_2.3.8_en-US.msi.sha256"},
                {"name": "voicestick-firmware-sticks3-ota-2.3.8.bin"},
                {"name": "manifest.json"},
                {"name": "notes.txt"},  # 未知资产：跳过不镜像
            ],
        }
        plan = mirror.plan_mirror_assets(release)
        keys = [key for _, key in plan]
        self.assertEqual(keys, [
            "software/windows/v2.3.8/VoiceStick_2.3.8_zh-CN.msi",
            "software/windows/v2.3.8/VoiceStick_2.3.8_en-US.msi.sha256",
            "firmware/v2.3.8/voicestick-firmware-sticks3-ota-2.3.8.bin",
            "firmware/v2.3.8/manifest.json",
        ])

    def test_latest_manifest_alias(self):
        release = {
            "tagName": "v2.3.8",
            "isLatest": True,
            "assets": [{"name": "manifest.json"}],
        }
        plan = mirror.plan_mirror_assets(release)
        # 最新版的 manifest 同步落一份到 firmware/latest/ 稳定地址
        self.assertIn(("manifest.json", "firmware/latest/manifest.json"), plan)

    def test_non_latest_no_alias(self):
        release = {
            "tagName": "v2.3.7",
            "isLatest": False,
            "assets": [{"name": "manifest.json"}],
        }
        plan = mirror.plan_mirror_assets(release)
        self.assertEqual([key for _, key in plan], ["firmware/v2.3.7/manifest.json"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
