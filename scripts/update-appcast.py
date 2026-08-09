#!/usr/bin/env python3
import argparse
import email.utils
import html
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


def existing_item(path: Path, sparkle_os: str) -> str:
    if not path.exists():
        return ""
    content = path.read_text(encoding="utf-8")
    match = re.search(
        rf'    <item>\s*.*?sparkle:os="{re.escape(sparkle_os)}".*?    </item>\r?\n?',
        content,
        flags=re.DOTALL,
    )
    return match.group(0) if match else ""


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
    parser.add_argument("--release-notes", default="VoiceStick release.")
    args = parser.parse_args()

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

    notes = "".join(f"<li>{html.escape(line)}</li>" for line in args.release_notes.splitlines() if line.strip())
    if not notes:
        notes = "<li>VoiceStick release.</li>"

    pub_date = email.utils.format_datetime(datetime.now(timezone.utc))
    output_path = Path(args.output)
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
