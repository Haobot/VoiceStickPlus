#!/usr/bin/env python3
"""scripts/scan_release_artifacts.py 的单元测试（纯逻辑，不联网）。

运行：python scripts/test_scan_release_artifacts.py
"""
import importlib.util
import io
import sys  # noqa: F401  (redirect_stdout 需要 io；sys 供 importlib 使用)
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

SCRIPT = Path(__file__).with_name("scan_release_artifacts.py")


def load_module():
    spec = importlib.util.spec_from_file_location("scan_release_artifacts", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


scan = load_module()

TENCENT_ID = "AKIDABCDEFGHIJKLMNOPQRSTUV"
LLM_KEY = "sk-abcdefghijklmnopqrstuvwxyz012345"
SECRET = "Zm9vYmFyLXNlY3JldC12YWx1ZS0xMjM0NTY3OA=="


class ScanTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data: bytes) -> Path:
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_load_secret_values_from_config(self):
        cfg = self.root / "config.toml"
        cfg.write_text(
            '[asr]\nvolcengine_api_key = "%s"\ntencent_secret_id = "%s"\n'
            'llm_api_key = "%s"\nllm_base_url = "https://example.com"\n' % (SECRET, TENCENT_ID, LLM_KEY),
            encoding="utf-8",
        )
        values = dict(scan.load_secret_values(cfg))
        self.assertIn("volcengine_api_key", values)
        self.assertIn("tencent_secret_id", values)
        self.assertIn("llm_api_key", values)
        self.assertNotIn("llm_base_url", values)

    def test_pattern_rules(self):
        findings = scan.scan_bytes(TENCENT_ID.encode(), "a.bin", "@raw", [])
        self.assertEqual(findings[0].rule, "tencent_secret_id")
        findings = scan.scan_bytes(LLM_KEY.encode(), "a.bin", "@raw", [])
        self.assertEqual(findings[0].rule, "llm_api_key")
        payload = b'tencent_secret_key = "' + b"a" * 32 + b'"'
        findings = scan.scan_bytes(payload, "a.toml", "@raw", [])
        self.assertEqual(findings[0].rule, "credential_assignment")

    def test_exact_value_raw_and_utf16(self):
        values = [("volcengine_api_key", SECRET.encode())]
        raw = scan.scan_bytes(b"prefix" + SECRET.encode() + b"suffix", "exe", "@raw", values)
        self.assertEqual(raw[0].rule, "config_value:volcengine_api_key")
        # UTF-16LE 编码不含裸 ASCII 值，只能由 wide 分支命中。
        wide = scan.scan_bytes(SECRET.encode("utf-16-le"), "exe", "@raw", values)
        self.assertTrue(any(f.rule.startswith("config_value_utf16") for f in wide))

    def test_clean_file_and_cli_exit_codes(self):
        clean = self.write("clean.txt", b"hello world, nothing secret here")
        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(scan.main([str(clean)]), 0)
        dirty = self.write("dirty.toml", b'api_key = "' + LLM_KEY.encode() + b'"')
        with redirect_stdout(io.StringIO()):
            self.assertEqual(scan.main([str(dirty)]), 1)
            self.assertEqual(scan.main(["--allow-builtin", str(dirty)]), 0)

    def test_zip_entry_scanned(self):
        archive_path = self.root / "bundle.zip"
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("config/config.template.toml",
                             'llm_api_key = "%s"\n' % LLM_KEY)
            archive.writestr("readme.txt", "no secrets")
        findings = scan.scan_file(archive_path, [])
        self.assertTrue(any("!config/config.template.toml" == f.location for f in findings))

    def test_zip_with_secret_value_only_in_utf16(self):
        archive_path = self.root / "wide.zip"
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("blob.bin", SECRET.encode("utf-16-le"))
        values = [("llm_api_key", SECRET.encode())]
        findings = scan.scan_file(archive_path, values)
        self.assertTrue(any(f.rule.startswith("config_value_utf16") for f in findings))

    def test_scan_file_handles_bad_zip(self):
        fake = self.write("fake.zip", b"PK\x03\x04 not really a zip")
        self.assertEqual(scan.scan_file(fake, []), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
