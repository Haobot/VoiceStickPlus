#!/usr/bin/env python3
import argparse
import email.utils
import html
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

# 兼容 importlib 按路径加载与直接运行两种方式，确保同目录模块可导入
sys.path.insert(0, str(Path(__file__).resolve().parent))
from mirror_urls import mirror_asset_url


def existing_item(path: Path, sparkle_os: str) -> str:
    """返回 appcast 中匹配 sparkle:os 的单个 <item> 块（含行尾换行）。

    必须按 item 边界切分后再过滤：旧正则会跨 item 贪婪匹配，当首个 item 是另一
    平台时会把它连同目标 item 一起返回，输出里就会出现重复 item。
    """
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8")
    for block in re.findall(r"    <item>.*?    </item>\r?\n?", content, flags=re.DOTALL):
        if f'sparkle:os="{sparkle_os}"' in block:
            return block
    return ""


def _version_key(version: str) -> tuple:
    """x.y.z 转可比较元组；非数字形态返回空元组（调用方跳过单调性检查）。"""
    parts = version.strip().split(".")
    if not parts or not all(p.isdigit() for p in parts):
        return ()
    return tuple(int(p) for p in parts)


def newest_version_in_appcast(path: Path) -> tuple:
    if not path.exists():
        return ()
    versions = re.findall(r'sparkle:version="([^"]+)"', path.read_text(encoding="utf-8"))
    keys = [key for key in (_version_key(v) for v in versions) if key]
    return max(keys) if keys else ()


def main() -> None:
    parser = argparse.ArgumentParser(description="Write the Sparkle appcast for a VoiceStick release.")
    parser.add_argument("--version", required=True)
    parser.add_argument("--zip-url", help="macOS ZIP asset URL (macOS-only release).")
    parser.add_argument("--signature", help="Sparkle EdDSA signature of the macOS ZIP.")
    parser.add_argument("--length", type=int, help="Byte length of the macOS ZIP.")
    parser.add_argument("--msi-url", help="Windows MSI asset URL (Windows-only release).")
    parser.add_argument("--msi-length", type=int, help="Byte length of the Windows MSI.")
    parser.add_argument("--output", default="website/public/appcast.xml")
    parser.add_argument("--appcast-url", required=True,
                        help="Public URL where this appcast will be hosted.")
    parser.add_argument("--mirror-base", default=None,
                        help="COS 国内分发面域名（如 https://dl.davenger.cloud）；"
                             "提供时 enclosure URL 改写为镜像路径，须先完成 Release 资产镜像上传")
    parser.add_argument("--release-notes", default="VoiceStick release.")
    parser.add_argument("--allow-version-downgrade", action="store_true",
                        help="允许 appcast 广播版本回退（默认拒绝：旧线补丁发布会把高版本用户永久锁死）")
    args = parser.parse_args()

    # 长度与签名仍对资产内容本身，URL 指向镜像不影响校验链
    if args.mirror_base:
        if args.msi_url:
            args.msi_url = mirror_asset_url(args.msi_url, args.mirror_base)
        if args.zip_url:
            args.zip_url = mirror_asset_url(args.zip_url, args.mirror_base)

    has_macos = args.zip_url or args.signature or args.length is not None
    has_windows = args.msi_url or args.msi_length is not None

    if has_macos:
        if not (args.zip_url and args.signature and args.length):
            sys.exit("Error: --zip-url, --signature and --length must be provided together.")
        if args.length <= 0:
            sys.exit("Error: --length must be greater than 0 for Sparkle updates.")
        if "REPLACE_WITH" in args.signature or not args.signature.strip():
            sys.exit("Error: --signature must be a real Sparkle EdDSA signature.")
    if has_windows:
        if not (args.msi_url and args.msi_length):
            sys.exit("Error: --msi-url and --msi-length must be provided together.")
        if args.msi_length <= 0:
            sys.exit("Error: --msi-length must be greater than 0 when --msi-url is set.")
    if not (has_macos or has_windows):
        sys.exit("Error: provide at least one platform item (macOS ZIP or Windows MSI).")

    # 版本单调性：appcast 是两端共用的更新源，广播版本回退会让高版本用户永久收不到更新。
    output_path = Path(args.output)
    newest = newest_version_in_appcast(output_path)
    current = _version_key(args.version)
    if newest and current and current < newest and not args.allow_version_downgrade:
        sys.exit(f"Error: refusing to downgrade appcast from {'.'.join(map(str, newest))} "
                 f"to {args.version}; pass --allow-version-downgrade to override.")

    notes = "".join(f"<li>{html.escape(line)}</li>" for line in args.release_notes.splitlines() if line.strip())
    if not notes:
        notes = "<li>VoiceStick release.</li>"

    pub_date = email.utils.format_datetime(datetime.now(timezone.utc))
    windows_item = ""
    if has_windows:
        windows_item = f"""    <item>
      <title>Version {html.escape(args.version)}</title>
      <description><![CDATA[
        <ul>
          {notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <enclosure
        url="{html.escape(args.msi_url)}"
        sparkle:os="windows"
        sparkle:version="{html.escape(args.version)}"
        sparkle:shortVersionString="{html.escape(args.version)}"
        sparkle:installerArguments="/passive"
        length="{args.msi_length}"
        type="application/octet-stream"
      />
    </item>
"""
    else:
        windows_item = existing_item(output_path, "windows")

    macos_item = ""
    if has_macos:
        macos_item = f"""    <item>
      <title>Version {html.escape(args.version)}</title>
      <description><![CDATA[
        <ul>
          {notes}
        </ul>
      ]]></description>
      <pubDate>{pub_date}</pubDate>
      <sparkle:minimumSystemVersion>12.0</sparkle:minimumSystemVersion>
      <enclosure
        url="{html.escape(args.zip_url)}"
        sparkle:os="macos"
        sparkle:version="{html.escape(args.version)}"
        sparkle:shortVersionString="{html.escape(args.version)}"
        sparkle:edSignature="{html.escape(args.signature)}"
        length="{args.length}"
        type="application/octet-stream"
      />
    </item>
"""
    else:
        macos_item = existing_item(output_path, "macos")

    content = f"""<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>VoiceStick</title>
    <link>{html.escape(args.appcast_url)}</link>
    <description>VoiceStick app updates</description>
    <language>zh-CN</language>
{windows_item}{macos_item}  </channel>
</rss>
"""
    output_path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
