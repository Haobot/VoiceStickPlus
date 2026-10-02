#!/usr/bin/env python3
"""Mac 侧消费 CI 预签名 URL 清单直传 COS（publish-mac.yml 轨道第 4/6 步）。

清单来自 Release 资产 cos-presign-<tag>.json / cos-site-presign-<tag>.json：
  stage 格式   = [{local,key,url,content_type,cache_control}, ...]
  finalize 格式 = {"objects":[...同上...], "deletes":[{key,url},...]}

用法（finalize 整站，本地已解包 CI 下发的 cos-site-dist zip）：
    python3 scripts/upload_presigned.py cos-site-presign-v2.4.7.json \
        --local-root site-dist

local 解析顺序：清单里的 local 路径（相对 --chdir）→ --local-root/<key>。

每个对象 PUT 时必须原样回放签入的 Content-Type / Cache-Control（COS 签名
校验按请求头重建，头不一致 403；不带头则对象落 octet-stream——2026-10-02
整站白屏事故根因）。成功后对公有域名做 HEAD 校验状态码与 Content-Type。
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_DOMAIN = "https://dl.davenger.cloud"
RETRIES = 3


def curl(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(["curl", "-fsS", "--max-time", "300", *args],
                          capture_output=True, text=True, **kwargs)


def put_one(obj: dict, chdir: str | None) -> None:
    local = Path(obj["local"])
    if not local.is_file():
        raise FileNotFoundError(f"local file missing for key {obj['key']}: {local}")
    headers = []
    if obj.get("content_type"):
        headers += ["-H", f"Content-Type: {obj['content_type']}"]
    if obj.get("cache_control"):
        headers += ["-H", f"Cache-Control: {obj['cache_control']}"]
    for attempt in range(1, RETRIES + 1):
        proc = curl(["-X", "PUT", "-T", str(local), *headers, obj["url"]], cwd=chdir)
        if proc.returncode == 0:
            print(f"uploaded {obj['key']}")
            return
        sys.stderr.write(f"PUT failed (attempt {attempt}/{RETRIES}) {obj['key']}: "
                         f"{proc.stderr.strip()[:200]}\n")
        if attempt < RETRIES:
            time.sleep(5 * attempt)
    raise RuntimeError(f"PUT exhausted retries: {obj['key']}")


def delete_one(item: dict) -> None:
    proc = curl(["-X", "DELETE", item["url"]])
    if proc.returncode != 0:
        raise RuntimeError(f"DELETE failed: {item['key']}: {proc.stderr.strip()[:200]}")
    print(f"deleted {item['key']}")


def verify_one(domain: str, obj: dict) -> None:
    key = obj["key"]
    proc = subprocess.run(
        ["curl", "-fsSI", "--max-time", "60", f"{domain}/{key}"],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"HEAD not 200: {key}")
    head = proc.stdout.lower()
    if "content-type" not in head:
        raise RuntimeError(f"HEAD missing content-type: {key}")
    expect = obj.get("content_type", "").lower()
    if expect and expect.split(";")[0] not in head:
        raise RuntimeError(f"content-type mismatch for {key}: expect {expect}, got:\n{proc.stdout}")
    print(f"verified {key}")


def main():
    parser = argparse.ArgumentParser(description="消费 CI 预签名 URL 清单直传 COS。")
    parser.add_argument("manifest", help="presign 清单 JSON 路径")
    parser.add_argument("--local-root", help="local 路径失效时按 <local-root>/<key> 回退解析")
    parser.add_argument("--chdir", help="解析清单内相对 local 路径的基准目录（默认当前目录）")
    parser.add_argument("--domain", default=DEFAULT_DOMAIN, help="HEAD 校验的公有域名")
    parser.add_argument("--skip-verify", action="store_true", help="跳过上传后的 HEAD 校验")
    args = parser.parse_args()

    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    if isinstance(manifest, list):
        objects, deletes = manifest, []
    else:
        objects, deletes = manifest.get("objects", []), manifest.get("deletes", [])

    local_root = Path(args.local_root) if args.local_root else None
    base = Path(args.chdir) if args.chdir else Path(".")
    for obj in objects:
        if local_root and not (base / obj["local"]).is_file():
            obj["local"] = str(local_root / obj["key"])

    failed = []
    for obj in objects:
        try:
            put_one(obj, args.chdir)
            if not args.skip_verify:
                verify_one(args.domain, obj)
        except Exception as exc:  # noqa: BLE001 — 汇总后统一退出码
            sys.stderr.write(f"ERROR: {exc}\n")
            failed.append(obj["key"])
    for item in deletes:
        try:
            delete_one(item)
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write(f"ERROR: {exc}\n")
            failed.append(item["key"])

    if failed:
        sys.exit(f"Error: {len(failed)} object(s) failed: {failed}")
    print(f"done: {len(objects)} uploaded, {len(deletes)} deleted")


if __name__ == "__main__":
    main()
