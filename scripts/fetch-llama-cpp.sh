#!/bin/bash
# 拉取并预编译 llama.cpp（pin b10868，与 desktop/windows/CMakeLists.txt FetchContent
# 同 pin），把 llama+ggml 静态库**合成单个 dylib** 安装到用户目录，供
# LlamaCppEngine 运行时 dlopen（与 sherpa-onnx dylib 同模式——C 头已入库
# Sources/CLlamaCpp，仓库克隆后无任何二进制即可 swift build）。
#
# 安装位（LlamaCppEngine.librarySearchPaths 对应）：
#   ~/Library/Application Support/VoiceStick/models/llama/lib/libvoicestick-llama.dylib
# 幂等：dylib 已存在即跳过（VOICESTICK_LLAMA_CPP_FORCE=1 强制重建）。
#
# 环境变量：
#   VOICESTICK_LLAMA_CPP_URL     源码 zip 覆盖（默认 ghfast.top 镜像 → GitHub 直连回退）
#   VOICESTICK_LLAMA_CPP_FORCE=1 强制重建
# 依赖：cmake + Xcode clang（无 ninja 时用 Unix Makefiles）。
# 网络注意：GitHub 走代理（外网直连不通时 export https_proxy=...）。

set -euo pipefail

PIN="b10868"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_ROOT="${HOME}/Library/Application Support/VoiceStick/models/llama/lib"
DEST="${DEST_ROOT}/libvoicestick-llama.dylib"
PRIMARY_URL="https://ghfast.top/https://github.com/ggml-org/llama.cpp/archive/refs/tags/${PIN}.zip"
FALLBACK_URL="https://github.com/ggml-org/llama.cpp/archive/refs/tags/${PIN}.zip"

if [[ -f "${DEST}" && "${VOICESTICK_LLAMA_CPP_FORCE:-0}" != "1" ]]; then
    echo "libvoicestick-llama.dylib 已就位（$(cat "${DEST_ROOT}/BUILD-INFO.txt" 2>/dev/null | head -1)），跳过"
    exit 0
fi

command -v cmake >/dev/null || { echo "需要 cmake（brew install cmake）" >&2; exit 1; }

URL="${VOICESTICK_LLAMA_CPP_URL:-$PRIMARY_URL}"
TMP=$(mktemp -d)
trap 'rm -rf "${TMP}"' EXIT

echo "下载 ${URL} ..."
if ! curl -fL --retry 3 -o "${TMP}/llama.zip" "${URL}"; then
    if [[ -n "${VOICESTICK_LLAMA_CPP_URL:-}" ]]; then exit 1; fi
    echo "镜像失败，回退 GitHub 直连（国内需代理）..."
    curl -fL --retry 3 -o "${TMP}/llama.zip" "${FALLBACK_URL}"
fi
unzip -q "${TMP}/llama.zip" -d "${TMP}/src"
SRC_DIR=$(echo "${TMP}/src/llama.cpp-"*)
echo "源码：${SRC_DIR}"

# 只编 ggml(CPU) + llama 静态库——开关与 Windows FetchContent 完全一致，
# 差异仅两处：GGML_METAL 本阶段先关（先对齐 Windows CPU 语义跑通，Metal 后续
# 再开）、Apple 平台开 Accelerate（ggml 官方 macOS 默认，提升矩阵吞吐）。
cmake -S "${SRC_DIR}" -B "${TMP}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLAMA_STANDALONE=OFF \
    -DLLAMA_BUILD_COMMON=OFF \
    -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_TOOLS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_BUILD_APP=OFF \
    -DLLAMA_CURL=OFF \
    -DGGML_BACKEND_DL=OFF \
    -DGGML_NATIVE=ON \
    -DGGML_OPENMP=OFF \
    -DGGML_METAL=OFF \
    -DGGML_CUDA=OFF \
    -DGGML_VULKAN=OFF \
    -DGGML_RPC=OFF \
    -DGGML_ACCELERATE=ON
cmake --build "${TMP}/build" --target llama ggml -- -j"$(sysctl -n hw.ncpu)"

# 合成单 dylib：-force_load 强制拉入全部归档成员（纯归档无根对象时 ld 会整个
# 丢弃），-lc++ 补 C++ 运行库，-dead_strip 裁未引用符号。
LIBS=$(find "${TMP}/build" -name 'libggml*.a' -not -name '*.dylib' | sort)
[[ -n "${LIBS}" ]] || { echo "未找到 libggml*.a" >&2; exit 1; }
DYLIB_TMP="${TMP}/libvoicestick-llama.dylib"
FORCE_LOADS=(-force_load "${TMP}/build/src/libllama.a")
for lib in ${LIBS}; do
    FORCE_LOADS+=(-force_load "${lib}")
done
# shellcheck disable=SC2086
clang -dynamiclib -o "${DYLIB_TMP}" \
    "${FORCE_LOADS[@]}" \
    -lc++ -framework Accelerate \
    -install_name @rpath/libvoicestick-llama.dylib \
    -Wl,-dead_strip

mkdir -p "${DEST_ROOT}"
cp "${DYLIB_TMP}" "${DEST}.tmp"
mv "${DEST}.tmp" "${DEST}"
printf '%s\n' "pin=${PIN}" "built=$(date '+%Y-%m-%d %H:%M:%S')" "url=${URL}" \
    "merged=$(echo "${LIBS}" | xargs -n1 basename | tr '\n' ' ')libllama.a" \
    > "${DEST_ROOT}/BUILD-INFO.txt"

echo "完成：${DEST}"
ls -la "${DEST_ROOT}"
