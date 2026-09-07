#!/usr/bin/env python3
"""从 GitHub Release 生成 website/public/downloads.json（下载页数据源）。

在 deploy-website.yml 中随 appcast 更新一起调用；也可本地手动运行：
    GH_TOKEN=... python scripts/update-downloads.py --repo Haobot/VoiceStickPlus

纯转换逻辑 build_downloads() 的单测见 scripts/test_update_downloads.py。
"""
import argparse
import datetime as _dt
import json
import subprocess
import sys
from pathlib import Path

WINDOWS_PORTABLE_PREFIX = "VoiceStick_Portable"
MACOS_PREFIX = "VoiceStick-"
FIRMWARE_PREFIX = "voicestick-firmware-"


def classify_asset(name: str):
    """按资产名判断平台分类；附属文件（.sha256 / manifest.json）与未知资产返回 None。"""
    if name.endswith(".sha256") or name == "manifest.json":
        return None
    if name.endswith(".msi") or name.startswith(WINDOWS_PORTABLE_PREFIX):
        return "windows"
    if name.startswith(MACOS_PREFIX) and name.endswith((".zip", ".dmg")):
        return "macos"
    if name.startswith(FIRMWARE_PREFIX):
        return "firmware"
    return None


def build_entry(release: dict) -> dict:
    assets_by_name = {a["name"]: a for a in release["assets"]}
    assets = []
    for asset in release["assets"]:
        platform = classify_asset(asset["name"])
        if platform is None:
            continue
        checksum = assets_by_name.get(asset["name"] + ".sha256")
        assets.append({
            "name": asset["name"],
            "platform": platform,
            "url": asset["url"],
            "size": asset["size"],
            "sha256": checksum["url"] if checksum else None,
        })
    return {
        "version": release["tagName"].lstrip("v"),
        "date": release["publishedAt"],
        "notes": release["body"] or "",
        "prerelease": release["isPrerelease"],
        "assets": assets,
    }


def build_downloads(releases: list, min_version: str, generated_at: str) -> dict:
    """纯转换：gh release JSON 列表 -> downloads.json 结构。"""
    entries = [build_entry(r) for r in releases]
    if not entries:
        sys.exit("Error: no releases returned; refusing to write an empty downloads.json.")
    latest_index = next(
        (i for i, r in enumerate(releases) if r.get("isLatest")), 0)
    latest = dict(entries[latest_index])
    latest["min_firmware_version"] = min_version
    return {
        "generated_at": generated_at,
        "latest": latest,
        "releases": entries,
    }


def gh_json(args: list) -> list | dict:
    result = subprocess.run(["gh", *args], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def fetch_releases(repo: str, limit: int) -> list:
    listed = gh_json([
        "release", "list", "--repo", repo, "--limit", str(limit),
        "--json", "tagName,isPrerelease,isLatest,publishedAt",
    ])
    releases = []
    for item in listed:
        # isLatest 仅 release list --json 支持（view 不提供），从列表阶段合并
        detail = gh_json([
            "release", "view", item["tagName"], "--repo", repo,
            "--json", "assets,body,isPrerelease,publishedAt,tagName",
        ])
        detail["isLatest"] = item["isLatest"]
        releases.append(detail)
    return releases


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate website/public/downloads.json from GitHub Releases.")
    parser.add_argument("--repo", required=True, help="owner/name，例如 Haobot/VoiceStickPlus")
    parser.add_argument("--limit", type=int, default=10, help="收录最近多少个版本")
    parser.add_argument("--output", default="website/public/downloads.json")
    parser.add_argument("--min-version-file", default="FIRMWARE_MIN_VERSION",
                        help="最低兼容固件版本文件（单行纯文本）")
    args = parser.parse_args()

    min_version = Path(args.min_version_file).read_text(encoding="utf-8").strip()
    releases = fetch_releases(args.repo, args.limit)
    downloads = build_downloads(
        releases, min_version,
        _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with open(output, "w", encoding="utf-8") as f:
        json.dump(downloads, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"Wrote {output} ({len(downloads['releases'])} releases, "
          f"latest {downloads['latest']['version']}).")


if __name__ == "__main__":
    main()
