#!/usr/bin/env python3
"""腾讯 COS 上传共用模块（GitHub Actions 内使用）。

凭据从环境变量 TENCENT_COS_SECRET_ID / TENCENT_COS_SECRET_KEY 读取
（GitHub Secrets 注入，不入仓库）；qcloud_cos SDK 延迟导入，纯逻辑
（cache_control_for / require_credentials）可在未装 SDK 的环境单测。
"""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def require_credentials():
    secret_id = os.environ.get("TENCENT_COS_SECRET_ID", "").strip()
    secret_key = os.environ.get("TENCENT_COS_SECRET_KEY", "").strip()
    if not secret_id or not secret_key:
        sys.exit("Error: TENCENT_COS_SECRET_ID / TENCENT_COS_SECRET_KEY "
                 "environment variables are required.")
    return secret_id, secret_key


def cache_control_for(key: str) -> str:
    """更新元数据类文件（appcast/manifest/页面数据）禁缓存；版本化二进制长缓存。"""
    if key.endswith((".html", ".json", ".xml")):
        return "no-cache"
    return "max-age=31536000"


def collect_dir(local_dir: str, key_prefix: str):
    """目录递归映射为 (local_path, cos_key) 列表；空前缀表示同步到 bucket 根。"""
    root = Path(local_dir)
    prefix = key_prefix.strip("/")
    return [
        (str(path), path.relative_to(root).as_posix() if not prefix
         else f"{prefix}/{path.relative_to(root).as_posix()}")
        for path in sorted(root.rglob("*"))
        if path.is_file()
    ]


def upload_files(bucket: str, region: str, files) -> None:
    """逐个上传 (local_path, cos_key)；任一失败抛异常终止（不静默跳过）。"""
    from qcloud_cos import CosConfig, CosS3Client

    secret_id, secret_key = require_credentials()
    client = CosS3Client(CosConfig(Region=region, SecretId=secret_id, SecretKey=secret_key))
    for local, key in files:
        client.upload_file(
            Bucket=bucket,
            Key=key,
            LocalFilePath=local,
            CacheControl=cache_control_for(key),
            EnableMD5=False,
        )
        print(f"uploaded {key}")
