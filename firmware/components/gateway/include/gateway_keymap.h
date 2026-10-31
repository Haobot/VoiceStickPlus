// gateway_keymap.h — 小米 usage → 输出动作翻译表（含软件路由，纯逻辑无 ESP-IDF 依赖）
//
// 四类动作（Doc/Plan/xiaomi-remote-stick-gateway.md §5.3）：
//   KEYBOARD：小米报的键盘页 usage 值恰与 HID keycode 一致（方向/Enter/菜单/Home），直接透传
//   CONSUMER：键盘页非标 usage（三键/mute）翻译为 Consumer 页标准 usage，全平台原生识别（R1）
//   INTERCEPT：语音/电源/tv/未知 usage 截留在网关内，不向目标设备输出
//   SOFTWARE：软件路由（P1 隧道融合）——不出 HOGP，改经 state_tx gateway_key 事件
//             上报桌面端，由桌面端 keymap 自定义动作（value 携带原 usage）
//
// 路由表（内存态，NVS 持久化由 main.c 负责）：除语音键（固定 ATVV 会话语义，不可
// 路由）与未知 usage 外的 13 键均可设 SOFTWARE；默认全部 PASSTHROUGH（Phase 1
// 行为零回归）。线程契约：路由表设置与查询须在同一线程序列化（app_event 任务）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GATEWAY_KEY_KEYBOARD = 0,  // value = HID 键盘页 keycode（Report ID 2）
    GATEWAY_KEY_CONSUMER,      // value = Consumer 页 16 位 usage（Report ID 1）
    GATEWAY_KEY_INTERCEPT,     // 截留（语音键会话/电源防误关机/tv/未知）
    GATEWAY_KEY_SOFTWARE,      // 软件路由（value = 原小米 usage，键名查 key_name）
} gateway_key_kind_t;

typedef struct {
    gateway_key_kind_t kind;
    uint16_t value;  // 按 kind 解释：keycode / Consumer usage / SOFTWARE 时为原 usage
} gateway_key_action_t;

// 路由模式
typedef enum {
    GATEWAY_ROUTE_PASSTHROUGH = 0,  // 直通（默认，Phase 1 行为）
    GATEWAY_ROUTE_SOFTWARE,         // 软件路由（gateway_key 事件上报桌面端）
} gateway_route_t;

gateway_key_action_t gateway_keymap_translate(uint16_t xiaomi_usage);

// ---- 软件路由表 ----

// 设置单键路由：语音键（0x003E）与未知 usage 返回非 0（不可路由），其余返回 0。
int gateway_keymap_set_route(uint16_t xiaomi_usage, gateway_route_t route);
gateway_route_t gateway_keymap_get_route(uint16_t xiaomi_usage);
// 恢复全部默认（直通）
void gateway_keymap_reset_routes(void);

// usage → 协议 key 字符串（gateway_key 事件 key 字段 / 配置枚举）；语音键与未知
// usage 返回 NULL。
const char *gateway_keymap_key_name(uint16_t xiaomi_usage);
// 可软件路由的键数（13）
#define GATEWAY_KEYMAP_ROUTABLE_COUNT 13
size_t gateway_keymap_routable_key_count(void);
// 按序号遍历可路由键（i ∈ [0, GATEWAY_KEYMAP_ROUTABLE_COUNT)）：返回 usage，越界返回 0
uint16_t gateway_keymap_routable_usage_at(size_t index);
