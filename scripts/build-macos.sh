#!/bin/bash
# Build VoiceStick for macOS as a universal app bundle.
#
# Produces:
#   build/VoiceStick-<version>.app
#   build/VoiceStick-<version>.zip
#   build/VoiceStick-<version>.signature  (when Sparkle sign_update is available)
#
# Optional environment:
#   VOICESTICK_APPCAST_URL=https://haobot.github.io/VoiceStickPlus/appcast.xml
#   SPARKLE_PUBLIC_ED_KEY=<public key from Sparkle generate_keys>
#   SPARKLE_PRIVATE_ED_KEY=<private key exported by Sparkle generate_keys -x>
#   SPARKLE_KEY_ACCOUNT=voicestick
#   VOICESTICK_DEV_IDENTITY=<本地自签名证书 CN，默认 "VoiceStick Local Code Signing">
#   VOICESTICK_ARCHS=<目标架构列表，默认 "arm64 x86_64"；本机 CLT（macOS 27 SDK）
#     的 Swift 兼容静态库无 x86_64 slice，本机开发构建需 VOICESTICK_ARCHS=arm64>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
DESKTOP_DIR="$ROOT_DIR/desktop/macos"
BUILD_DIR="$ROOT_DIR/build"
PLIST="$DESKTOP_DIR/Sources/VoiceStickApp/Info.plist"
VERSION="$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")"
CONFIG="${1:---release}"
TARGET_ARCHS="${VOICESTICK_ARCHS:-arm64 x86_64}"
SPARKLE_KEY_ACCOUNT="${SPARKLE_KEY_ACCOUNT:-voicestick}"
VOICESTICK_DEV_IDENTITY="${VOICESTICK_DEV_IDENTITY:-VoiceStick Local Code Signing}"

case "$CONFIG" in
    --release)
        SWIFT_CONFIG="release"
        ;;
    --debug)
        SWIFT_CONFIG="debug"
        ;;
    *)
        echo "Usage: $0 [--release|--debug]"
        exit 1
        ;;
esac

if [ -z "$VERSION" ]; then
    echo "Error: VERSION is empty"
    exit 1
fi

mkdir -p "$BUILD_DIR"

echo "===================================="
echo " VoiceStick macOS Build v$VERSION"
echo " Universal Binary: $TARGET_ARCHS"
echo "===================================="

/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$PLIST"

if [ -n "${VOICESTICK_APPCAST_URL:-}" ]; then
    /usr/libexec/PlistBuddy -c "Set :SUFeedURL $VOICESTICK_APPCAST_URL" "$PLIST"
fi

if [ -n "${SPARKLE_PUBLIC_ED_KEY:-}" ]; then
    /usr/libexec/PlistBuddy -c "Set :SUPublicEDKey $SPARKLE_PUBLIC_ED_KEY" "$PLIST"
elif /usr/libexec/PlistBuddy -c "Print :SUPublicEDKey" "$PLIST" | grep -q "REPLACE_WITH"; then
    echo "WARNING: SUPublicEDKey is still a placeholder."
    echo "         Generate Sparkle keys before shipping a public release."
fi

for ARCH in $TARGET_ARCHS; do
    echo ""
    echo "Building VoiceStickApp for $ARCH..."
    SCRATCH="$DESKTOP_DIR/.build-$ARCH"
    rm -rf "$SCRATCH"
    swift build \
        --package-path "$DESKTOP_DIR" \
        -c "$SWIFT_CONFIG" \
        --arch "$ARCH" \
        --scratch-path "$SCRATCH"
done

APP_DIR="$BUILD_DIR/VoiceStick-${VERSION}.app"
rm -rf "$APP_DIR"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources" "$APP_DIR/Contents/Frameworks"

# 定位指定架构的产物目录。新版工具链（CLT/macOS 27）构建产物在
# out/Products/<Config>/，旧版在 <triple>/<config>/，两处都探测。
bin_dir_for_arch() {
    local arch="$1"
    local scratch="$DESKTOP_DIR/.build-$arch"
    local config_cap
    config_cap="$(echo "$SWIFT_CONFIG" | awk '{print toupper(substr($0,1,1)) tolower(substr($0,2))}')"
    if [ -f "$scratch/$arch-apple-macosx/$SWIFT_CONFIG/VoiceStickApp" ]; then
        echo "$scratch/$arch-apple-macosx/$SWIFT_CONFIG"
    elif [ -f "$scratch/out/Products/$config_cap/VoiceStickApp" ]; then
        echo "$scratch/out/Products/$config_cap"
    else
        return 1
    fi
}

set -- $TARGET_ARCHS
BIN_DIRS=()
for ARCH in $TARGET_ARCHS; do
    if ! BIN_DIR="$(bin_dir_for_arch "$ARCH")"; then
        echo "Error: 找不到 $ARCH 的构建产物（VoiceStickApp）" >&2
        exit 1
    fi
    BIN_DIRS+=("$BIN_DIR")
done
if [ "$#" -eq 1 ]; then
    echo ""
    echo "Copying $1 executable..."
    cp "${BIN_DIRS[0]}/VoiceStickApp" "$APP_DIR/Contents/MacOS/VoiceStickApp"
else
    echo ""
    echo "Creating universal executable..."
    LIPO_INPUTS=()
    for BIN_DIR in "${BIN_DIRS[@]}"; do
        LIPO_INPUTS+=("$BIN_DIR/VoiceStickApp")
    done
    lipo -create "${LIPO_INPUTS[@]}" -output "$APP_DIR/Contents/MacOS/VoiceStickApp"
fi

cp "$PLIST" "$APP_DIR/Contents/Info.plist"

ICON_PATH="$DESKTOP_DIR/Resources/AppIcon.icns"
if [ -f "$ICON_PATH" ]; then
    cp "$ICON_PATH" "$APP_DIR/Contents/Resources/AppIcon.icns"
else
    echo "WARNING: App icon was not found: $ICON_PATH"
fi

FIRST_ARCH="${TARGET_ARCHS%% *}"
# 优先取产物目录里 SwiftPM 已铺好的 Sparkle.framework（与链接所用一致），
# 取不到再回退到 binary artifact 目录（旧工具链行为）。
SPARKLE_FRAMEWORK="${BIN_DIRS[0]}/Sparkle.framework"
if [ ! -d "$SPARKLE_FRAMEWORK" ]; then
    SPARKLE_FRAMEWORK="$(find -L "$DESKTOP_DIR/.build-$FIRST_ARCH/artifacts" -name Sparkle.framework -type d 2>/dev/null | head -1 || true)"
fi
if [ -n "$SPARKLE_FRAMEWORK" ]; then
    cp -R "$SPARKLE_FRAMEWORK" "$APP_DIR/Contents/Frameworks/"
    install_name_tool -add_rpath "@loader_path/../Frameworks" "$APP_DIR/Contents/MacOS/VoiceStickApp" 2>/dev/null || true
else
    echo "WARNING: Sparkle.framework was not found in SwiftPM artifacts."
fi

# 签名身份优先级：Developer ID（发布）> 本地自签证书（开发）> ad-hoc。
# TCC 权限（蓝牙/输入监控/辅助功能）按签名身份记账：ad-hoc 的 designated
# requirement 锚定二进制 cdhash，每次重编译即失效，权限反复弹窗；用固定证书
# 签名后授权一次即可跨构建保持。自签证书 CN = VOICESTICK_DEV_IDENTITY，
# pem 源在 ~/.voicestick-sign/（生成与排障见
# Doc/Expe/xiaomi-remote-macos-hid-seize-not-permitted-2026-09-03.md）。
# 注意：新版 macOS 的 find-identity -p codesigning 不把自签证书列为 valid
# identity，但 codesign --sign 按证书名仍可正常解析签名，故用 find-certificate
# + find-key 探测证书与私钥是否存在。
CODESIGN_IDENTITY="-"
CODESIGN_EXTRA_FLAGS=(--deep --force --options runtime)
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk -F'"' '{print $2}')"
elif security find-certificate -c "$VOICESTICK_DEV_IDENTITY" >/dev/null 2>&1 \
    && security find-key -l "$VOICESTICK_DEV_IDENTITY" >/dev/null 2>&1; then
    CODESIGN_IDENTITY="$VOICESTICK_DEV_IDENTITY"
    # 自签证书没有 Apple Team ID，同 ad-hoc 一样不能带 --options runtime。
    CODESIGN_EXTRA_FLAGS=(--deep --force)
fi

echo ""
echo "Signing app..."
xattr -cr "$APP_DIR" 2>/dev/null || true
if [ "$CODESIGN_IDENTITY" = "-" ]; then
    # ad-hoc 本地测试包不能带 --options runtime：hardened runtime 的库校验会拒绝
    # 内嵌的 Sparkle.framework（dyld 报 "different Team IDs"，app 启动即崩）。
    # hardened runtime 只为公证（notarization）服务，正式签名分支保留。
    echo "Using ad-hoc signature."
else
    echo "Using: $CODESIGN_IDENTITY"
fi
codesign "${CODESIGN_EXTRA_FLAGS[@]}" --sign "$CODESIGN_IDENTITY" "$APP_DIR"

echo "Verifying app signature..."
codesign --verify --deep --strict --verbose=2 "$APP_DIR"

ZIP_PATH="$BUILD_DIR/VoiceStick-${VERSION}.zip"
SIGNATURE_PATH="${ZIP_PATH%.zip}.signature"
STAGING_DIR="$BUILD_DIR/.sparkle-staging"
rm -rf "$STAGING_DIR" "$ZIP_PATH" "$SIGNATURE_PATH"
mkdir -p "$STAGING_DIR"
ditto --norsrc --noextattr "$APP_DIR" "$STAGING_DIR/VoiceStick.app"

echo ""
echo "Creating Sparkle ZIP..."
ditto -c -k --norsrc --noextattr --keepParent "$STAGING_DIR/VoiceStick.app" "$ZIP_PATH"
rm -rf "$STAGING_DIR"

SIGN_TOOL="$(find -L "$DESKTOP_DIR/.build-$FIRST_ARCH/artifacts" -name sign_update -type f 2>/dev/null | head -1 || true)"
if [ -n "$SIGN_TOOL" ] && [ -x "$SIGN_TOOL" ]; then
    echo "Signing Sparkle ZIP..."
    if [ -n "${SPARKLE_PRIVATE_ED_KEY:-}" ]; then
        SIGN_OUTPUT="$(printf '%s' "$SPARKLE_PRIVATE_ED_KEY" | "$SIGN_TOOL" --ed-key-file - "$ZIP_PATH" 2>&1 || true)"
    else
        SIGN_OUTPUT="$("$SIGN_TOOL" --account "$SPARKLE_KEY_ACCOUNT" "$ZIP_PATH" 2>&1 || true)"
    fi
    echo "$SIGN_OUTPUT"
    ED_SIGNATURE="$(printf '%s\n' "$SIGN_OUTPUT" | sed -nE 's/.*sparkle:edSignature="([^"]+)".*/\1/p' | head -1)"
    if [ -n "$ED_SIGNATURE" ]; then
        printf '%s\n' "$ED_SIGNATURE" > "$SIGNATURE_PATH"
    else
        printf '%s\n' "$SIGN_OUTPUT" > "$SIGNATURE_PATH"
    fi
else
    echo "WARNING: Sparkle sign_update tool was not found."
fi

echo ""
echo "Build complete:"
echo "  App: $APP_DIR"
echo "  ZIP: $ZIP_PATH"
if [ -f "$SIGNATURE_PATH" ]; then
    echo "  Sig: $SIGNATURE_PATH"
fi
echo ""
echo "Next: $SCRIPT_DIR/make-dmg.sh $APP_DIR"
