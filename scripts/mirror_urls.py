#!/usr/bin/env python3
"""GitHub Release 资产 URL -> 腾讯 COS 镜像 URL 的确定性映射。

供 update-appcast.py / update-downloads.py 共用；路径规则必须与
deploy-website.yml 的「镜像 Release 资产到 COS」步骤保持一致
（见 Doc/Rfc/tencent-cos-domestic-distribution-2026-09-09.md）：
    https://github.com/<repo>/releases/download/<tag>/<name>
        -> <mirror>/windows/<tag>/<name>   （MSI / 便携包）
        -> <mirror>/macos/<tag>/<name>     （Sparkle ZIP / DMG）
        -> <mirror>/firmware/<tag>/<name>  （固件 bin / manifest.json）
.sha256 校验和跟随主资产同目录。未知资产与非 GitHub URL 原样返回
（宁可回源也不指到不存在的镜像路径）。
"""
import re

WINDOWS_PORTABLE_PREFIX = "VoiceStick_Portable"
MACOS_PREFIX = "VoiceStick-"
FIRMWARE_PREFIX = "voicestick-firmware-"

_GITHUB_ASSET_RE = re.compile(
    r"^https://github\.com/(?P<repo>[^/]+/[^/]+)/releases/download/"
    r"(?P<tag>[^/]+)/(?P<name>.+)$"
)


def mirror_class(asset_name: str):
    """资产名 -> 镜像目录（windows/macos/firmware）；附属文件跟随主资产。"""
    if asset_name.endswith(".sha256"):
        return mirror_class(asset_name[: -len(".sha256")])
    if asset_name == "manifest.json":
        return "firmware"
    if asset_name.endswith(".msi") or asset_name.startswith(WINDOWS_PORTABLE_PREFIX):
        return "windows"
    if asset_name.startswith(MACOS_PREFIX) and asset_name.endswith((".zip", ".dmg")):
        return "macos"
    if asset_name.startswith(FIRMWARE_PREFIX):
        return "firmware"
    return None


def mirror_asset_url(github_url: str, mirror_base: str) -> str:
    """GitHub Release 资产 URL 映射为 COS 镜像 URL；无法归类时原样返回。"""
    match = _GITHUB_ASSET_RE.match(github_url)
    if not match:
        return github_url
    asset_class = mirror_class(match.group("name"))
    if asset_class is None:
        return github_url
    base = mirror_base.rstrip("/")
    return f"{base}/{asset_class}/{match.group('tag')}/{match.group('name')}"
