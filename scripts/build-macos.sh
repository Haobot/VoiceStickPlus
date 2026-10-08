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
    # E3-①：release 构建带占位公钥 = 签出来的更新永远无法被验证（Sparkle 静默拒收，
    # 用户拿不到修复）→ fail-hard；debug 构建保留告警（本地迭代不应被卡住）。
    if [ "$SWIFT_CONFIG" = "release" ]; then
        echo "Error: SUPublicEDKey is still a placeholder in a release build."
        echo "       Shipped updates could never be verified by clients."
        echo "       Set SPARKLE_PUBLIC_ED_KEY (and SPARKLE_PRIVATE_ED_KEY for sign_update)."
        exit 1
    fi
    echo "WARNING: SUPublicEDKey is still a placeholder."
    echo "         Generate Sparkle keys before shipping a public release."
fi

# ---- 内置凭据注入（对齐 Windows build-msi.bat 内部测试模式）----
# 源：本机 config.toml（VOICESTICK_BUILTIN_CONFIG_SOURCE 可覆盖）；提取 7 项字段
# 重写 Sources/VoiceStickApp/BuiltinSecrets.swift（仓库内占位模板），构建完成后
# trap 恢复占位（凭据不进 git）。VOICESTICK_EMBED_BUILTIN_KEYS=0 显式构建无凭据
# 公开包。门禁日志（发布 skill 铁律）：见 "Injecting built-in credentials into
# VoiceStickApp" 即内测包；见 "building WITHOUT built-in credentials" 即公开包。
BUILTIN_SECRETS_SWIFT="$DESKTOP_DIR/Sources/VoiceStickApp/BuiltinSecrets.swift"
BUILTIN_SECRETS_RESTORED=0
restore_builtin_secrets() {
    if [ "$BUILTIN_SECRETS_RESTORED" -eq 0 ] && [ -n "${BUILTIN_SECRETS_DIRTY:-}" ]; then
        git -C "$ROOT_DIR" checkout -- "$BUILTIN_SECRETS_SWIFT" 2>/dev/null || true
        BUILTIN_SECRETS_RESTORED=1
    fi
}
trap restore_builtin_secrets EXIT

if [ "${VOICESTICK_EMBED_BUILTIN_KEYS:-1}" != "0" ]; then
    BUILTIN_CONFIG_SOURCE="${VOICESTICK_BUILTIN_CONFIG_SOURCE:-$HOME/Library/Application Support/VoiceStick/config.toml}"
    if [ -f "$BUILTIN_CONFIG_SOURCE" ]; then
        if INJECT_STATUS="$(python3 - "$BUILTIN_CONFIG_SOURCE" "$BUILTIN_SECRETS_SWIFT" <<'PYEOF'
import re, sys

src_path, out_path = sys.argv[1], sys.argv[2]
content = open(src_path, encoding="utf-8").read()

FIELDS = [
    ("volcengineAPIKey", "volcengine_api_key"),
    ("tencentSecretID", "tencent_secret_id"),
    ("tencentSecretKey", "tencent_secret_key"),
    ("tencentAppid", "tencent_appid"),
    ("llmAPIKey", "llm_api_key"),
    ("llmBaseURL", "llm_base_url"),
    ("llmModel", "llm_model"),
]

def esc(v: str) -> str:
    return v.replace("\\", "\\\\").replace('"', '\\"')

values = {}
missing = []
for swift_name, toml_name in FIELDS:
    m = re.search(rf'^\s*{toml_name}\s*=\s*"([^"]*)"', content, re.M)
    value = (m.group(1).strip() if m else "")
    values[swift_name] = value
    if not value:
        missing.append(toml_name)

# 逐字段替换占位空值（保持文件其余注释/结构不变，git diff 最小）
out = open(out_path, encoding="utf-8").read()
for swift_name, _ in FIELDS:
    out, n = re.subn(
        rf'(static let {swift_name} = ")[^"]*(")',
        lambda m, v=values[swift_name]: m.group(1) + v.replace("\\", "\\\\").replace('"', '\\"') + m.group(2),
        out, count=1)
    if n != 1:
        sys.exit(f"Error: BuiltinSecrets.swift 缺少 {swift_name} 占位行")
open(out_path, "w", encoding="utf-8").write(out)

print("PARTIAL:" + ",".join(missing) if missing else "FULL")
PYEOF
        )"; then
            BUILTIN_SECRETS_DIRTY=1
            echo "Injecting built-in credentials into VoiceStickApp (source: $BUILTIN_CONFIG_SOURCE)"
            case "$INJECT_STATUS" in
                PARTIAL:*) echo "WARNING: 部分字段缺失，留空回退：${INJECT_STATUS#PARTIAL:}" ;;
                FULL) ;;
            esac
        else
            echo "Error: 内置凭据注入失败（解析 $BUILTIN_CONFIG_SOURCE 出错）"
            restore_builtin_secrets
            exit 1
        fi
    else
        echo "building WITHOUT built-in credentials (no config source at $BUILTIN_CONFIG_SOURCE)"
    fi
else
    echo "building WITHOUT built-in credentials (VOICESTICK_EMBED_BUILTIN_KEYS=0)"
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
# 无人值守（CI/深夜自动发布）：登录钥匙串的私钥首次被 codesign 访问会弹
# SecurityAgent ACL 授权且无法非交互放行（set-key-partition-list 需要用户密码）。
# 解法 = VOICESTICK_CODESIGN_KEYCHAIN 指向自建钥匙串（密码已知，已对其
# set-key-partition-list），本脚本对 sign 追加 --keychain。2026-10-02 实测：
# 自建钥匙串 + 显式 --keychain 可正常签名/验签（旧结论「只认登录钥匙串」过时）。
#
# 2026-10-02 追加（用户实测踩坑）：自建钥匙串在睡眠/重启后回到锁定态，裸访问
# （含 security show-keychain-info）会弹 GUI 解锁窗且密码非用户开机密码，极易
# 误判死锁。两道防线：
#   1) 未显式指定 VOICESTICK_CODESIGN_KEYCHAIN 时自动采用默认路径（存在即用），
#      避免照抄旧命令静默降级 ad-hoc；
#   2) 签名前用 ~/.voicestick-sign/keychain-password 自动 unlock-keychain
#      （幂等，已解锁也安全）。
DEFAULT_CODESIGN_KEYCHAIN="$HOME/Library/Keychains/voicestick-sign.keychain-db"
if [ -z "${VOICESTICK_CODESIGN_KEYCHAIN:-}" ] && [ -f "$DEFAULT_CODESIGN_KEYCHAIN" ]; then
    VOICESTICK_CODESIGN_KEYCHAIN="$DEFAULT_CODESIGN_KEYCHAIN"
fi
# 自动解锁（幂等，已解锁同样安全）：锁定态下任何访问（含证书探测）都可能弹
# GUI 解锁窗。密码文件缺失/不符时降级为警告——不阻断构建（钥匙串恰好已解锁
# 仍可完成签名）。
if [ -n "${VOICESTICK_CODESIGN_KEYCHAIN:-}" ] && [ -f "$VOICESTICK_CODESIGN_KEYCHAIN" ]; then
    SIGN_KEYCHAIN_PASSWORD_FILE="$HOME/.voicestick-sign/keychain-password"
    if [ -f "$SIGN_KEYCHAIN_PASSWORD_FILE" ]; then
        if ! security unlock-keychain -p "$(cat "$SIGN_KEYCHAIN_PASSWORD_FILE")" \
                "$VOICESTICK_CODESIGN_KEYCHAIN" 2>/dev/null; then
            echo "WARNING: 解锁 $VOICESTICK_CODESIGN_KEYCHAIN 失败（$SIGN_KEYCHAIN_PASSWORD_FILE 与钥匙串密码不符？）；签名阶段可能弹窗。"
        fi
    else
        echo "WARNING: 缺少 $SIGN_KEYCHAIN_PASSWORD_FILE，无法自动解锁自建签名钥匙串；若弹解锁窗，密码见 ~/.voicestick-sign/README.md。"
    fi
fi
CODESIGN_IDENTITY="-"
CODESIGN_EXTRA_FLAGS=(--deep --force)
CODESIGN_KEYCHAIN_ARGS=()
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID Application"; then
    CODESIGN_IDENTITY="$(security find-identity -v -p codesigning | grep "Developer ID Application" | head -1 | awk -F'"' '{print $2}')"
elif [ -n "${VOICESTICK_CODESIGN_KEYCHAIN:-}" ] && [ -f "$VOICESTICK_CODESIGN_KEYCHAIN" ] \
    && security find-certificate -c "$VOICESTICK_DEV_IDENTITY" "$VOICESTICK_CODESIGN_KEYCHAIN" >/dev/null 2>&1; then
    CODESIGN_IDENTITY="$VOICESTICK_DEV_IDENTITY"
    CODESIGN_KEYCHAIN_ARGS=(--keychain "$VOICESTICK_CODESIGN_KEYCHAIN")
    # 自签证书没有 Apple Team ID，同 ad-hoc 一样不能带 --options runtime。
    CODESIGN_EXTRA_FLAGS=(--deep --force)
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
codesign "${CODESIGN_EXTRA_FLAGS[@]}" --sign "$CODESIGN_IDENTITY" "${CODESIGN_KEYCHAIN_ARGS[@]}" "$APP_DIR"

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
SIGN_FAIL=""
if [ -n "$SIGN_TOOL" ] && [ -x "$SIGN_TOOL" ]; then
    echo "Signing Sparkle ZIP..."
    SIGN_RC=0
    if [ -n "${SPARKLE_PRIVATE_ED_KEY:-}" ]; then
        SIGN_OUTPUT="$(printf '%s' "$SPARKLE_PRIVATE_ED_KEY" | "$SIGN_TOOL" --ed-key-file - "$ZIP_PATH" 2>&1)" || SIGN_RC=$?
    else
        SIGN_OUTPUT="$("$SIGN_TOOL" --account "$SPARKLE_KEY_ACCOUNT" "$ZIP_PATH" 2>&1)" || SIGN_RC=$?
    fi
    echo "$SIGN_OUTPUT"
    ED_SIGNATURE="$(printf '%s\n' "$SIGN_OUTPUT" | sed -nE 's/.*sparkle:edSignature="([^"]+)".*/\1/p' | head -1)"
    # E3-② 格式校验：EdDSA 签名必须是 base64 且 ≥40 字符——畸形值绝不落盘。
    if [ -n "$ED_SIGNATURE" ] && ! printf '%s' "$ED_SIGNATURE" | grep -qE '^[A-Za-z0-9+/=]{40,}$'; then
        echo "Error: sign_update produced a malformed edSignature."
        ED_SIGNATURE=""
        SIGN_RC=1
    fi
    if [ -n "$ED_SIGNATURE" ]; then
        printf '%s\n' "$ED_SIGNATURE" > "$SIGNATURE_PATH"
    else
        # E3-②：原 else 分支把**错误文本**写进 .signature（不可验证还伪装成已签名）；
        # 改为删除任何陈旧签名，绝不产生"新 zip + 旧/坏签名"的组合。
        rm -f "$SIGNATURE_PATH"
        if [ "$SIGN_RC" -ne 0 ]; then
            echo "Error: sign_update failed (rc=$SIGN_RC)."
        fi
        SIGN_FAIL="sign_update did not produce a signature"
    fi
else
    SIGN_FAIL="Sparkle sign_update tool was not found"
fi
if [ -n "$SIGN_FAIL" ]; then
    # E3-②：release 构建没有签名 = 发出去的更新无法验证 → fail-hard；
    # debug 构建保留告警（本地不装 Sparkle 工具链也不该卡住）。
    if [ "$SWIFT_CONFIG" = "release" ]; then
        echo "Error: cannot ship a release build without a Sparkle signature ($SIGN_FAIL)."
        exit 1
    fi
    echo "WARNING: Sparkle signature unavailable in debug build ($SIGN_FAIL)."
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
