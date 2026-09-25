#!/usr/bin/env python3
"""scripts/update-appcast.py 的单元测试（纯逻辑，不联网）。

运行：python scripts/test_update_appcast.py
"""
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("update-appcast.py")


def load_module():
    spec = importlib.util.spec_from_file_location("update_appcast", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


appcast = load_module()

ITEM = """    <item>
      <title>Version {version}</title>
      <enclosure
        url="https://example.com/{os}-{version}.bin"
        sparkle:os="{os}"
        sparkle:version="{version}"
        length="123"
        type="application/octet-stream"
      />
    </item>
"""

APCAST_TMPL = """<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>VoiceStick</title>
    <link>https://example.com/appcast.xml</link>
{items}  </channel>
</rss>
"""


class UpdateAppcastTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def seed(self, *os_versions):
        path = self.root / "appcast.xml"
        items = "".join(ITEM.format(os=os_name, version=version) for os_name, version in os_versions)
        path.write_text(APCAST_TMPL.format(items=items), encoding="utf-8")
        return path

    def run_main(self, path, version, extra):
        argv = ["--version", version, "--output", str(path),
                "--appcast-url", "https://example.com/appcast.xml"] + extra
        old_argv = sys.argv
        sys.argv = ["update-appcast.py"] + argv
        try:
            appcast.main()
        finally:
            sys.argv = old_argv

    def test_existing_item_isolated_per_platform(self):
        path = self.seed(("macos", "3.0.0"), ("windows", "3.0.0"))
        windows = appcast.existing_item(path, "windows")
        macos = appcast.existing_item(path, "macos")
        self.assertEqual(windows.count('sparkle:os="windows"'), 1)
        self.assertNotIn('sparkle:os="macos"', windows)
        self.assertEqual(macos.count('sparkle:os="macos"'), 1)
        self.assertNotIn('sparkle:os="windows"', macos)
        self.assertEqual(appcast.existing_item(path, "linux"), "")

    def test_no_duplicate_items_when_other_platform_item_exists(self):
        path = self.seed(("windows", "2.3.9"))
        self.run_main(path, "2.3.9", [
            "--zip-url", "https://example.com/VS-2.3.9.zip",
            "--signature", "c2lnbmF0dXJl",
            "--length", "1234",
        ])
        content = path.read_text(encoding="utf-8")
        self.assertEqual(content.count('sparkle:os="windows"'), 1, content)
        self.assertEqual(content.count('sparkle:os="macos"'), 1, content)

    def test_version_downgrade_refused(self):
        path = self.seed(("windows", "3.0.0"))
        before = path.read_text(encoding="utf-8")
        with self.assertRaises(SystemExit) as ctx:
            self.run_main(path, "2.9.9", ["--msi-url", "https://example.com/a.msi", "--msi-length", "10"])
        self.assertIn("downgrade", str(ctx.exception))
        self.assertEqual(path.read_text(encoding="utf-8"), before)
        self.run_main(path, "2.9.9", [
            "--msi-url", "https://example.com/a.msi", "--msi-length", "10",
            "--allow-version-downgrade",
        ])
        self.assertIn('sparkle:version="2.9.9"', path.read_text(encoding="utf-8"))

    def test_version_helpers(self):
        self.assertEqual(appcast._version_key("2.3.10"), (2, 3, 10))
        self.assertGreater(appcast._version_key("2.3.10"), appcast._version_key("2.3.9"))
        self.assertEqual(appcast._version_key("v2.3.9"), ())
        path = self.seed(("macos", "2.3.9"), ("windows", "2.4.0"))
        self.assertEqual(appcast.newest_version_in_appcast(path), (2, 4, 0))


if __name__ == "__main__":
    unittest.main(verbosity=2)
