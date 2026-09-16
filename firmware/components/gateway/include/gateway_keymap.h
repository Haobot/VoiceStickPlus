// gateway_keymap.h — 小米 usage → 标准 HID 输出动作翻译表
//
// 三类动作（设计依据 Doc/Plan/xiaomi-remote-stick-gateway.md §5.3 P2 路由）：
//   KEYBOARD：小米报的键盘页 usage 值恰与 HID keycode 一致（方向/Enter/菜单/Home），直接透传
//   CONSUMER：键盘页非标 usage（三键/mute）翻译为 Consumer 页标准 usage，全平台原生识别（R1）
//   INTERCEPT：语音/电源/tv/未知 usage 截留在网关内，不向目标设备输出
#pragma once

#include <stdint.h>

typedef enum {
    GATEWAY_KEY_KEYBOARD = 0,  // value = HID 键盘页 keycode（Report ID 2）
    GATEWAY_KEY_CONSUMER,      // value = Consumer 页 16 位 usage（Report ID 1）
    GATEWAY_KEY_INTERCEPT,     // 截留（语音键会话/电源防误关机/tv/未知）
} gateway_key_kind_t;

typedef struct {
    gateway_key_kind_t kind;
    uint16_t value;  // 按 kind 解释：keycode 或 Consumer usage
} gateway_key_action_t;

gateway_key_action_t gateway_keymap_translate(uint16_t xiaomi_usage);
