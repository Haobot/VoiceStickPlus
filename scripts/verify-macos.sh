#!/bin/bash
# 本机闭环验证：macOS 全 target 构建 + VoiceStickTests + 固件 host 单测。
#
# 一条命令跑完本机可执行的全部验证面，任一环节红即整体红（闭环判定）：
#   1. preflight  环境预检（CLT / arm64 约束 / python3 / 仓库洁净度提示）
#   2. build      VOICESTICK_ARCHS=arm64 swift build（全 target——App 模块错误
#                 只有全量构建能测出，VoiceStickTests 只链 Core）
#   3. tests      swift run VoiceStickTests（无框架 runner，失败 exit 1）
#   4. firmware   固件 host 单测（gateway/voice_ble 纯逻辑，cc 直编；覆盖
#                 BLE 连接参数/conn_table 等跨端契约改动）
#   5. summary    汇总各环节耗时与红绿，日志留档 build/verify-logs/
#
# 用法：
#   scripts/verify-macos.sh                # 全流程
#   scripts/verify-macos.sh --skip-build   # 复用已有构建产物只跑测试
#   scripts/verify-macos.sh --only tests   # 只跑单环节（preflight/build/tests/firmware）
#
# 退出码：全绿 0；任一环节失败 1；参数错误 2。

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
MACOS_DIR="$ROOT_DIR/desktop/macos"
LOG_DIR="$ROOT_DIR/build/verify-logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
ONLY=""
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --only) ONLY="${2:?--only 需要环节名}"; shift 2 ;;
        *) echo "未知参数: $1（支持 --skip-build / --only <stage>）" >&2; exit 2 ;;
    esac
done

RED='\033[0;31m'; GREEN='\033[0;32m'; BOLD='\033[1m'; OFF='\033[0m'
declare -a STAGE_NAMES=()
declare -a STAGE_RESULTS=()

run_stage() {
    local name="$1"
    local log="$LOG_DIR/${STAMP}-${name}.log"
    shift
    local t0=$(date +%s)
    echo -e "${BOLD}==> [${name}] 开始${OFF}"
    if "$@" > >(tee "$log") 2>&1; then
        STAGE_RESULTS+=("PASS")
    else
        STAGE_RESULTS+=("FAIL")
    fi
    local t1=$(date +%s)
    STAGE_NAMES+=("${name} ($((t1 - t0))s)")
    echo -e "${BOLD}==> [${name}] 结束，日志: ${log}${OFF}"
}

want() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }

mkdir -p "$LOG_DIR"

# ---------- 1. preflight ----------
stage_preflight() {
    echo "== 仓库 HEAD: $(git -C "$ROOT_DIR" rev-parse --short HEAD) $(git -C "$ROOT_DIR" branch --show-current)"
    local dirty
    dirty=$(git -C "$ROOT_DIR" status --short | wc -l | tr -d ' ')
    echo "== 工作区改动条目: ${dirty}（非 0 时结果对应该工作区状态）"
    command -v xcrun >/dev/null || { echo "FAIL: 无 xcrun（需 Xcode Command Line Tools）"; return 1; }
    echo "== CLT: $(xcrun --find clang 2>/dev/null | head -1)"
    command -v python3 >/dev/null || { echo "FAIL: 无 python3（固件 host 单测需要）"; return 1; }
    # 本机 CLT-only（macOS 27 SDK）约束：Swift 兼容静态库无 x86_64 slice。
    if [[ "$(uname -m)" == "arm64" ]]; then
        echo "== arm64 主机：将以 VOICESTICK_ARCHS=arm64 构建（CLT-only 约束）"
    else
        echo "WARN: 非 arm64 主机，universal 构建约束需自行确认"
    fi
    echo "preflight OK"
}

# ---------- 2. build ----------
stage_build() {
    (cd "$MACOS_DIR" && VOICESTICK_ARCHS=arm64 swift build)
}

# ---------- 3. tests ----------
stage_tests() {
    (cd "$MACOS_DIR" && VOICESTICK_ARCHS=arm64 swift run VoiceStickTests)
}

# ---------- 4. firmware host tests ----------
stage_firmware() {
    python3 "$ROOT_DIR/firmware/components/gateway/test/run_tests.py"
}

echo -e "${BOLD}====== VoiceStick 本机闭环验证 ${STAMP} ======${OFF}"

if want preflight; then run_stage preflight stage_preflight; fi
if want build && [[ $SKIP_BUILD -eq 0 ]]; then run_stage build stage_build; fi
if want tests; then run_stage tests stage_tests; fi
if want firmware; then run_stage firmware stage_firmware; fi

echo
echo -e "${BOLD}====== 汇总 ======${OFF}"
OVERALL=0
# 自检：环节名/结果数组必须成对且非空，否则视为脚本自身故障强制红（防误报全绿）。
if [[ ${#STAGE_NAMES[@]} -eq 0 || ${#STAGE_NAMES[@]} -ne ${#STAGE_RESULTS[@]} ]]; then
    echo -e "  ${RED}✘ 汇总自检失败：run_stage 记录异常（names=${#STAGE_NAMES[@]} results=${#STAGE_RESULTS[@]}），视为整体失败${OFF}"
    exit 1
fi
for i in "${!STAGE_NAMES[@]}"; do
    if [[ "${STAGE_RESULTS[$i]}" == "PASS" ]]; then
        echo -e "  ${GREEN}✔ ${STAGE_NAMES[$i]}${OFF}"
    else
        echo -e "  ${RED}✘ ${STAGE_NAMES[$i]}${OFF}"
        OVERALL=1
    fi
done
[[ $OVERALL -eq 0 ]] && echo -e "${GREEN}本机闭环验证全部通过${OFF}" || echo -e "${RED}本机闭环验证存在失败环节，详见 ${LOG_DIR}/${STAMP}-*.log${OFF}"
exit $OVERALL
