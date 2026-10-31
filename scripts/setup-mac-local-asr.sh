#!/bin/bash
# macOS 本地 ASR 运行时安装：sherpa-onnx dylib + SenseVoice int8 模型。
# 安装位（与 SherpaSenseVoiceEngine.librarySearchPaths 对应）：
#   ~/Library/Application Support/VoiceStick/models/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib
#   ~/Library/Application Support/VoiceStick/models/sense-voice-int8-2024-07-17/{model.int8.onnx,tokens.txt}
# 幂等：文件已存在且 SHA-256 校验通过则跳过下载。
# 仅 arm64（x86_64 dylib 需换 osx-x64 资产，见 SHERPA_ASSET 变量）。
# 网络注意：GitHub 走代理（外网直连不通时 export https_proxy=...）；
#           ModelScope 国内直连。SHA-256 与 scripts/pack_local_models.py（Windows
#           分发清单）及 desktop/windows/CMakeLists sherpa-onnx 段同源锁定。

set -euo pipefail

DEST_ROOT="${HOME}/Library/Application Support/VoiceStick/models"
SHERPA_VER="v1.13.7"
SHERPA_ASSET="sherpa-onnx-${SHERPA_VER}-osx-arm64-shared-lib.tar.bz2"
SHERPA_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/${SHERPA_VER}/${SHERPA_ASSET}"
# SenseVoice int8（与 Windows 在发同款；pack_local_models.py 权威清单）
MODEL_URL_BASE="https://modelscope.cn/models/pengzhendong/sherpa-onnx-sense-voice-zh-en-ja-ko-yue/resolve/master"
MODEL_SHA256="c71f0ce00bec95b07744e116345e33d8cbbe08cef896382cf907bf4b51a2cd51"
TOKENS_SHA256="f449eb28dc567533d7fa59be34e2abca8784f771850c78a47fb731a31429a1dc"

sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }

mkdir -p "${DEST_ROOT}/sherpa-onnx/lib" "${DEST_ROOT}/sense-voice-int8-2024-07-17"

# ---- sherpa-onnx dylib（c-api + onnxruntime 两个 dylib）----
DYLIB_CAPI="${DEST_ROOT}/sherpa-onnx/lib/libsherpa-onnx-c-api.dylib"
DYLIB_ORT="${DEST_ROOT}/sherpa-onnx/lib/libonnxruntime.dylib"
if [[ -f "${DYLIB_CAPI}" && -f "${DYLIB_ORT}" ]]; then
    echo "sherpa-onnx dylib 已就位，跳过"
else
    TMP=$(mktemp -d)
    trap 'rm -rf "${TMP}"' EXIT
    echo "下载 ${SHERPA_ASSET}（GitHub，必要时请先 export https_proxy）..."
    curl -fL --retry 3 -o "${TMP}/${SHERPA_ASSET}" "${SHERPA_URL}"
    tar xjf "${TMP}/${SHERPA_ASSET}" -C "${TMP}"
    cp "${TMP}"/sherpa-onnx-*/lib/libsherpa-onnx-c-api.dylib "${DYLIB_CAPI}"
    cp "${TMP}"/sherpa-onnx-*/lib/libonnxruntime.dylib "${DYLIB_ORT}"
    echo "已安装 ${DYLIB_CAPI}"
fi

# ---- SenseVoice int8 模型 ----
MODEL_FILE="${DEST_ROOT}/sense-voice-int8-2024-07-17/model.int8.onnx"
TOKENS_FILE="${DEST_ROOT}/sense-voice-int8-2024-07-17/tokens.txt"
if [[ -f "${MODEL_FILE}" ]] && [[ "$(sha256_of "${MODEL_FILE}")" == "${MODEL_SHA256}" ]]; then
    echo "SenseVoice 模型已就位且校验通过，跳过"
else
    TMP=$(mktemp -d)
    trap 'rm -rf "${TMP}"' EXIT
    echo "下载 SenseVoice int8（ModelScope 直连）..."
    curl -fL --retry 3 -o "${TMP}/model.int8.onnx" "${MODEL_URL_BASE}/model.int8.onnx"
    curl -fL --retry 3 -o "${TMP}/tokens.txt" "${MODEL_URL_BASE}/tokens.txt"
    ACTUAL=$(sha256_of "${TMP}/model.int8.onnx")
    if [[ "${ACTUAL}" != "${MODEL_SHA256}" ]]; then
        echo "SHA-256 校验失败: model.int8.onnx" >&2
        echo "  期望 ${MODEL_SHA256}" >&2
        echo "  实际 ${ACTUAL}" >&2
        exit 1
    fi
    ACTUAL_TOKENS=$(sha256_of "${TMP}/tokens.txt")
    if [[ "${ACTUAL_TOKENS}" != "${TOKENS_SHA256}" ]]; then
        echo "SHA-256 校验失败: tokens.txt" >&2
        exit 1
    fi
    mv "${TMP}/model.int8.onnx" "${MODEL_FILE}"
    mv "${TMP}/tokens.txt" "${TOKENS_FILE}"
    echo "已安装 ${MODEL_FILE}"
fi

echo ""
echo "完成。在 config.toml 中启用："
echo "  [local_asr]"
echo "  enabled = true"
echo "（models_dir 留空即默认路径）"
