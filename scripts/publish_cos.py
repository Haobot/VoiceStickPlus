#!/usr/bin/env python3
"""本机发布 COS 直传（签名机运行，替代 CI 跨境镜像链路）。

背景：GitHub Actions 海外 runner -> 上海 COS 跨境上行 ~8KB/s 且 ~130s 断连，
大文件分块上传必死（Doc/Ref/cos-distribution.md 待办 3 定案）。固件/软件的
COS 同步移交本机发布流程（国内上行，1.34GB 模型已验证此路径）。

用法（Windows 签名机）：
    python scripts/publish_cos.py firmware --dist dist --version 2.4.0
    python scripts/publish_cos.py software --msi-dir <dir> --version 2.4.0
    python scripts/publish_cos.py pages-mirror --pages-base https://<owner>.github.io/<repo>

凭据：优先 TENCENT_COS_SECRET_ID / TENCENT_COS_SECRET_KEY（与 CI 同名），
回退 TENCENTCLOUD_SECRET_ID / TENCENTCLOUD_SECRET_KEY（本机常见命名）。
"""
import argparse
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cos_uploader import upload_files

DEFAULT_BUCKET = "voicestick-dl-1329978361"
DEFAULT_REGION = "ap-shanghai"
DEFAULT_DIST_DOMAIN = "https://dl.davenger.cloud"


def build_firmware_manifest(version, min_version, tag, dist_domain, download_base,
                            ota_name, merged_name, ota_sha, ota_size,
                            merged_sha, merged_size):
    """生成固件 OTA manifest（字段名/顺序与 release.yml 旧版一致，客户端校验依赖）。"""
    return {
        "hardware": "stick_s3",
        "version": version,
        "min_version": min_version,
        "ota_url": f"{dist_domain}/firmware/{tag}/{ota_name}",
        "ota_url_fallback": f"{download_base}/{ota_name}",
        "ota_sha256": ota_sha,
        "ota_size": int(ota_size),
        "merged_url": f"{dist_domain}/firmware/{tag}/{merged_name}",
        "merged_url_fallback": f"{download_base}/{merged_name}",
        "merged_sha256": merged_sha,
        "merged_size": int(merged_size),
    }


def firmware_upload_plan(files, tag):
    """dist 文件列表 -> [(local_path, cos_key)]；manifest 追加 firmware/latest 稳定别名。

    COS 键即产物文件名（与 CI 镜像规则 mirror_urls.py 一致）。
    """
    plan = []
    for local, kind in files:
        key = f"firmware/{tag}/{Path(local).name}"
        plan.append((str(local), key))
        if kind == "manifest":
            plan.append((str(local), "firmware/latest/manifest.json"))
    return plan


def software_upload_plan(files, tag):
    """MSI 及其校验文件 -> [(local_path, cos_key)]。"""
    return [(str(local), f"software/windows/{tag}/{Path(local).name}")
            for local in files]


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_sha256_file(path):
    """生成 sha256sum 格式校验文件（<hash>  <name>\\n），返回哈希十六进制。"""
    name = Path(path).name
    digest = sha256_file(path)
    Path(str(path) + ".sha256").write_text(f"{digest}  {name}\n", encoding="utf-8")
    return digest


def _require_local(path):
    if not Path(path).is_file():
        sys.exit(f"Error: 发布产物缺失: {path}")
    return path


def cmd_firmware(args):
    """收集 dist 固件产物 -> 生成 manifest -> COS 直传（含 latest 别名）。

    --skip-upload 只做本地准备（写 .sha256 与 manifest.json），供发布脚本
    在创建 GitHub Release 前先生成 manifest 资产；正式直传再完整重跑（幂等）。
    """
    tag = f"v{args.version}"
    ota_name = f"voicestick-firmware-sticks3-ota-{args.version}.bin"
    merged_name = f"voicestick-firmware-sticks3-merged-{args.version}.bin"
    dist = Path(args.dist)
    ota = _require_local(dist / ota_name)
    merged = _require_local(dist / merged_name)

    ota_sha = write_sha256_file(ota)
    merged_sha = write_sha256_file(merged)
    download_base = f"https://github.com/{args.repo}/releases/download/{tag}"
    manifest = build_firmware_manifest(
        version=args.version, min_version=args.min_version, tag=tag,
        dist_domain=args.dist_domain, download_base=download_base,
        ota_name=ota_name, merged_name=merged_name,
        ota_sha=ota_sha, ota_size=Path(ota).stat().st_size,
        merged_sha=merged_sha, merged_size=Path(merged).stat().st_size)
    manifest_path = dist / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2))

    files = [
        (ota, "ota"), (Path(str(ota) + ".sha256"), "ota_sha"),
        (merged, "merged"), (Path(str(merged) + ".sha256"), "merged_sha"),
        (manifest_path, "manifest"),
    ]
    if args.skip_upload:
        print("skip-upload：已生成 .sha256 与 manifest.json，未上传 COS")
        return
    upload_files(args.bucket, args.region, firmware_upload_plan(files, tag))


def cmd_software(args):
    """MSI -> COS 直传；缺失 .sha256 时自动按 sha256sum 格式生成。"""
    tag = f"v{args.version}"
    msi_dir = Path(args.msi_dir)
    files = []
    for lang in ("zh-CN", "en-US"):
        msi = _require_local(msi_dir / f"VoiceStick_{args.version}_{lang}.msi")
        if not Path(str(msi) + ".sha256").is_file():
            write_sha256_file(msi)
        files.append(msi)
        files.append(Path(str(msi) + ".sha256"))
    upload_files(args.bucket, args.region, software_upload_plan(files, tag))


def cmd_pages_mirror(args):
    """从 GitHub Pages 下载 appcast/downloads.json 等小文件，转传 COS 桶根。"""
    files = []
    for name in args.files:
        url = f"{args.pages_base.rstrip('/')}/{name}"
        target = Path(args.out_dir) / name
        target.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url, timeout=60) as resp, open(target, "wb") as f:
            f.write(resp.read())
        files.append((target, name))
        print(f"fetched {url}")
    upload_files(args.bucket, args.region, files)


def main():
    parser = argparse.ArgumentParser(description="本机发布 COS 直传。")
    parser.add_argument("--bucket", default=DEFAULT_BUCKET)
    parser.add_argument("--region", default=DEFAULT_REGION)
    sub = parser.add_subparsers(dest="command", required=True)

    p_fw = sub.add_parser("firmware", help="固件产物 + manifest 直传")
    p_fw.add_argument("--dist", required=True, help="dist 目录（含命名产物）")
    p_fw.add_argument("--version", required=True)
    p_fw.add_argument("--min-version", required=True, help="FIRMWARE_MIN_VERSION")
    p_fw.add_argument("--repo", default="Haobot/VoiceStickPlus")
    p_fw.add_argument("--dist-domain", default=DEFAULT_DIST_DOMAIN)
    p_fw.add_argument("--skip-upload", dest="skip_upload", action="store_true",
                      help="只生成 .sha256 与 manifest.json，不上传 COS")
    p_fw.set_defaults(func=cmd_firmware)

    p_sw = sub.add_parser("software", help="MSI + sha256 直传")
    p_sw.add_argument("--msi-dir", required=True)
    p_sw.add_argument("--version", required=True)
    p_sw.set_defaults(func=cmd_software)

    p_pm = sub.add_parser("pages-mirror", help="Pages 小文件（appcast/downloads.json）转传 COS")
    p_pm.add_argument("--pages-base", required=True)
    p_pm.add_argument("--files", nargs="+", default=["appcast.xml", "downloads.json"])
    p_pm.add_argument("--out-dir", default="dist/pages")
    p_pm.set_defaults(func=cmd_pages_mirror)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
