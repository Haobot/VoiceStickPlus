#!/usr/bin/env python3
"""把 GitHub Release 资产镜像到腾讯 COS（deploy-website workflow 用）。

用法（CI）：
    TENCENT_COS_SECRET_ID=... TENCENT_COS_SECRET_KEY=... \
    python scripts/mirror_release_to_cos.py --repo <owner/name> \
        --bucket <name> --region <region> --limit 10

镜像路径规则与 scripts/mirror_urls.py 一致（windows/macos/firmware 三类，
.sha256 与 manifest.json 跟随主资产）；最新版 manifest 额外落一份到
firmware/latest/manifest.json 稳定地址。未知资产跳过不镜像。
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cos_uploader import upload_files
from mirror_urls import mirror_class


def plan_mirror_assets(release: dict):
    """release JSON -> [(download_name, cos_key)]；未知资产跳过。

    download_name 为 Release 资产名（下载阶段用），最新版 manifest 追加
    latest 别名条目（同一资产上传到第二个 key）。
    """
    tag = release["tagName"]
    plan = []
    for asset in release["assets"]:
        name = asset["name"]
        asset_class = mirror_class(name)
        if asset_class is None:
            continue
        plan.append((name, f"{asset_class}/{tag}/{name}"))
    if release.get("isLatest"):
        tag_manifest = f"firmware/{tag}/manifest.json"
        for name, key in list(plan):
            if key == tag_manifest:
                plan.append((name, "firmware/latest/manifest.json"))
    return plan


def gh_json(args):
    result = subprocess.run(["gh", *args], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def fetch_releases(repo: str, limit: int):
    listed = gh_json(["release", "list", "--repo", repo, "--limit", str(limit),
                      "--json", "tagName,isLatest"])
    releases = []
    for item in listed:
        detail = gh_json(["release", "view", item["tagName"], "--repo", repo,
                          "--json", "assets"])
        detail["isLatest"] = item["isLatest"]
        releases.append(detail)
    return releases


def main() -> None:
    parser = argparse.ArgumentParser(description="Mirror GitHub Release assets to COS.")
    parser.add_argument("--repo", required=True)
    parser.add_argument("--bucket", required=True)
    parser.add_argument("--region", required=True)
    parser.add_argument("--limit", type=int, default=10,
                        help="镜像最近多少个版本（与 update-downloads --limit 一致）")
    args = parser.parse_args()

    releases = fetch_releases(args.repo, args.limit)
    with tempfile.TemporaryDirectory() as tmp:
        for release in releases:
            plan = plan_mirror_assets(release)
            if not plan:
                continue
            tag = release["tagName"]
            names = sorted({name for name, _ in plan})
            subprocess.run(
                ["gh", "release", "download", tag, "--repo", args.repo,
                 "--dir", tmp, *sum((["--pattern", n] for n in names), [])],
                check=True)
            files = [(str(Path(tmp) / name), key) for name, key in plan]
            upload_files(args.bucket, args.region, files)
            print(f"mirrored {tag}: {len(files)} objects")


if __name__ == "__main__":
    main()
