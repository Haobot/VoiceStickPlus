#pragma once

// control_rx 命令纯解析（0.1 固件端契约 reader，2026-10-08）：
// 桌面端写入 control_rx 的 JSON → 结构化命令。**解析与副作用分层**——本文件零
// ESP 依赖（仅 cJSON），可在宿主编译单测（test/control_cmd_test.c，消费
// tests/contract/fixtures/manifest.json 的桌面端黄金样本）；执行侧副作用留在
// main.c ble_control_cb / voice_ble handle_power_log_control 的 execute 分支。
//
// 行为契约（与原 if-else 链等价）：
//   - 返回 false = JSON 非法（调用方记警）；
//   - 返回 true 且 kind == CONTROL_CMD_NONE = 未知/不完整事件，**静默忽略**；
//   - 各事件的字段有效性门（如 remote_button_* 要求 button=="primary"）在解析侧
//     收敛——门不满足即 NONE，与原链 fallthrough 行为一致；
//   - 语义映射（枚举换算、数值钳位、颜色/键名校验、默认值与告警）留在执行侧。

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_CMD_NONE = 0,  // 未知/不完整 → 静默忽略
    CONTROL_CMD_UI_STATE,
    CONTROL_CMD_INTERACTION_MODE,
    CONTROL_CMD_SHOW_IMU_DEBUG,
    CONTROL_CMD_BATTERY_STATUS_REQUEST,
    CONTROL_CMD_USB_AUTO_OFF,
    CONTROL_CMD_USB_AUTO_OFF_GET,
    CONTROL_CMD_GATEWAY_SELECT_TARGET,
    CONTROL_CMD_GATEWAY_SIDE_SWITCH,
    CONTROL_CMD_GATEWAY_TARGET_INFO,
    CONTROL_CMD_REMOTE_BUTTON_DOWN,
    CONTROL_CMD_REMOTE_BUTTON_UP,
    CONTROL_CMD_OTA_COMMIT,
    CONTROL_CMD_IMU_WAKE_SENSITIVITY,
    CONTROL_CMD_TAP_ENABLED,
    CONTROL_CMD_TAP_SENSITIVITY,
    CONTROL_CMD_ENCODER_LED_COLOR,
    CONTROL_CMD_ENCODER_RECORDING_GATE,
    CONTROL_CMD_AIR_MOUSE_ENABLED,
    CONTROL_CMD_TEST_PLAYBACK,
    CONTROL_CMD_GATEWAY_KEYMAP_SET,
    CONTROL_CMD_GATEWAY_KEYMAP_GET,
    // D9：桌面端上报自身协议版本（协商语义见 protocol.md「Protocol version」）。
    // value = proto（uint 语义，负值即非法由执行侧忽略）。
    CONTROL_CMD_PROTO_NEGOTIATE,
    // power_log 族由 voice_ble 内部执行（main 侧收到仅忽略，保持原行为）
    CONTROL_CMD_POWER_LOG_DUMP,
    CONTROL_CMD_POWER_LOG_CLEAR,
    CONTROL_CMD_POWER_LOG_TIME_ANCHOR,
    CONTROL_CMD_POWER_LOG_UNKNOWN,  // cmd 字符串无法识别（执行侧告警）
} control_cmd_kind_t;

// 字段按 kind 复用（见各 case 注释）；尺寸覆盖 control 写入缓冲（voice_ble 512B）。
typedef struct {
    control_cmd_kind_t kind;
    char str1[256];  // state/mode/button/color/action/name/key/file/power_log.cmd
    char str2[64];   // gateway_keymap_set.route 等短串
    char text[256];  // ui_state.text
    bool enabled;    // *_enabled / *_gate 类开关
    bool flag_clear; // gateway_select_target.clear
    bool flag_self;  // gateway_select_target.self
    bool has_number; // 数字字段（level/threshold/index/request_id/epoch）是否出现
    bool has_str1;   // str1 是否来自存在的 JSON 字符串（缺失 vs 空串语义有别）
    bool has_str2;   // 同上（gateway_keymap_set.route）
    int32_t value;   // threshold / level / index / request_id / offset / epoch
    uint32_t uvalue; // power_log dump.max
} control_cmd_t;

// 解析一帧 control_rx JSON。返回 false 仅当 JSON 本身非法。
bool control_cmd_parse(const char *json, control_cmd_t *out);

#ifdef __cplusplus
}
#endif
