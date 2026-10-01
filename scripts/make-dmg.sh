#!/bin/bash
# Package VoiceStick.app into a signed and optionally notarized DMG.
#
# Usage:
#   scripts/make-dmg.sh
#   scripts/make-dmg.sh build/VoiceStick-<version>.app
#   scripts/make-dmg.sh build/VoiceStick-<version>.app build/VoiceStick-<version>.dmg

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT_DIR/build"
VERSION="$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")"
APP_PATH="${1:-$BUILD_DIR/VoiceStick-${VERSION}.app}"
OUTPUT="${2:-$BUILD_DIR/VoiceStick-${VERSION}.dmg}"
STAGING_DIR="$BUILD_DIR/.dmg-staging"
VOLUME_NAME="VoiceStick"

if [ ! -d "$APP_PATH" ]; then
    echo "Error: Application bundle not found: $APP_PATH"
    exit 1
fi

# 已带有效非 ad-hoc 签名（如 build-macos.sh 自签证书产物）则跳过重签：
# 默认分支会把身份降级为 ad-hoc 且带 --options runtime——自签证书无 Team ID，
# hardened runtime 库校验会拒载 Sparkle.framework（启动即崩，见 CHANGELOG v2.3.8）。
# VOICESTICK_RESIGN=1 可强制重签（Developer ID 公证流程保留原语义）。
if [ "${VOICESTICK_RESIGN:-0}" != "1" ] \
    && codesign --verify --deep --strict "$APP_PATH" 2>/dev/null \
    && codesign -dvv "$APP_PATH" 2>&1 | grep -q "Authority="; then
    echo "App already carries a valid non-adhoc signature; skipping re-sign."
else
    CODESIGN_IDENTITY="-"
    CODESIGN_KEYCHAIN_ARGS=()
    if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
        CODESIGN_IDENTITY="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk -F'"' '{print $2}')"
    fi
    # 无 Developer ID 时对齐 build-macos.sh：本地自签证书优先于 ad-hoc
    #（VOICESTICK_CODESIGN_KEYCHAIN 无人值守链路，说明见 build-macos.sh 注释）。
    if [ "$CODESIGN_IDENTITY" = "-" ] \
        && [ -n "${VOICESTICK_CODESIGN_KEYCHAIN:-}" ] && [ -f "$VOICESTICK_CODESIGN_KEYCHAIN" ]; then
        CODESIGN_IDENTITY="${VOICESTICK_DEV_IDENTITY:-VoiceStick Local Code Signing}"
        CODESIGN_KEYCHAIN_ARGS=(--keychain "$VOICESTICK_CODESIGN_KEYCHAIN")
        # 自签证书不能带 --options runtime（原因见上）。
        echo "Signing app before DMG packaging..."
        xattr -cr "$APP_PATH" 2>/dev/null || true
        echo "Using: $CODESIGN_IDENTITY"
        codesign --deep --force --sign "$CODESIGN_IDENTITY" "${CODESIGN_KEYCHAIN_ARGS[@]}" "$APP_PATH"
    else
        echo "Signing app before DMG packaging..."
        xattr -cr "$APP_PATH" 2>/dev/null || true
        if [ "$CODESIGN_IDENTITY" != "-" ]; then
            echo "Using: $CODESIGN_IDENTITY"
            codesign --deep --force --options runtime --sign "$CODESIGN_IDENTITY" "$APP_PATH"
        else
            echo "Using ad-hoc signature."
            codesign --deep --force --options runtime --sign - "$APP_PATH"
        fi
    fi
fi

echo "Verifying app signature..."
codesign --verify --deep --strict --verbose=2 "$APP_PATH"

rm -rf "$STAGING_DIR" "$OUTPUT"
mkdir -p "$STAGING_DIR"
ditto --norsrc --noextattr "$APP_PATH" "$STAGING_DIR/VoiceStick.app"
ln -s /Applications "$STAGING_DIR/Applications"

echo "Creating DMG..."
hdiutil create \
    -volname "$VOLUME_NAME" \
    -srcfolder "$STAGING_DIR" \
    -ov \
    -format UDZO \
    "$OUTPUT"
rm -rf "$STAGING_DIR"

if xcrun notarytool history --keychain-profile "AC_PASSWORD" >/dev/null 2>&1; then
    echo "Submitting DMG for notarization..."
    xcrun notarytool submit "$OUTPUT" --keychain-profile "AC_PASSWORD" --wait
    xcrun stapler staple "$OUTPUT"
else
    echo "Skipping notarization: keychain profile AC_PASSWORD was not found."
fi

echo "DMG complete: $OUTPUT"
