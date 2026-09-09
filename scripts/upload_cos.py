#!/usr/bin/env python3
"""按指定文件对上传腾讯 COS（release workflow 固件发布用）。

用法（CI）：
    TENCENT_COS_SECRET_ID=... TENCENT_COS_SECRET_KEY=... \
    python scripts/upload_cos.py --bucket <name> --region <region> \
        --upload dist/a.bin firmware/v2.3.10/a.bin \
        --upload dist/manifest.json firmware/latest/manifest.json
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cos_uploader import upload_files


def main() -> None:
    parser = argparse.ArgumentParser(description="Upload files to Tencent COS.")
    parser.add_argument("--bucket", required=True, help="COS bucket 名（含 APPID 后缀）")
    parser.add_argument("--region", required=True, help="COS 地域，如 ap-shanghai")
    parser.add_argument("--upload", nargs=2, action="append", default=[],
                        metavar=("LOCAL", "KEY"), help="本地路径与 COS 对象键，可多次")
    parser.add_argument("--sync-dir", nargs=2, action="append", default=[],
                        metavar=("DIR", "KEY_PREFIX"),
                        help="本地目录与 COS 键前缀，递归同步，可多次")
    args = parser.parse_args()

    files = []
    for local, key in args.upload:
        path = Path(local)
        if not path.is_file():
            sys.exit(f"Error: upload source not found: {local}")
        files.append((str(path), key))
    for local_dir, key_prefix in args.sync_dir:
        if not Path(local_dir).is_dir():
            sys.exit(f"Error: sync dir not found: {local_dir}")
        files.extend(collect_dir(local_dir, key_prefix))
    if not files:
        sys.exit("Error: nothing to upload (use --upload or --sync-dir).")
    upload_files(args.bucket, args.region, files)


if __name__ == "__main__":
    main()
