// gateway_switcher.h — 网关目标切换器（P1）纯逻辑状态机。
//
// 职责：决定「当前应该由哪台桌面端连着本机」，并在切换时产出动作交给 NimBLE 薄壳执行。
// 本文件**不依赖 ESP-IDF**（无 NimBLE/NVS/LVGL 调用），时间以调用方传入的 now_ms 表达，
// 因此可在主机侧单测（test/run_tests.py）完整覆盖状态迁移与超时回退。
//
// 状态迁移（设计见 Doc/Plan/xiaomi-gateway-p1-switcher.md §3.2）：
//
//   IDLE ──选择目标──► SWITCHING ──目标连上──► CONNECTED
//     ▲                    │ 超时(默认 5s)          │
//     └────────────────────┴── 回退上一目标 + UI 报错 ◄┘
//
// IDLE 语义：**不做限制**（任何 bond 过的目标都能连），保证没用过切换器的用户行为不变。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GATEWAY_SWITCHER_MAX_ACTIONS 6

// 目标身份：对端 identity address + 地址类型（与 gateway_target_t 同口径）。
typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
} gateway_switcher_peer_t;

typedef enum {
    GATEWAY_SWITCHER_IDLE = 0,   // 未选目标：不限制连接（默认行为）
    GATEWAY_SWITCHER_SWITCHING,  // 已断开旧目标，等新目标连上
    GATEWAY_SWITCHER_CONNECTED,  // 已连上选定目标
} gateway_switcher_state_t;

typedef enum {
    GATEWAY_SWITCHER_ACTION_NONE = 0,
    GATEWAY_SWITCHER_ACTION_DISCONNECT_CURRENT,  // 断开当前目标（切换第一步）
    GATEWAY_SWITCHER_ACTION_START_ADV,           // 确保在广播（等目标连上）
    GATEWAY_SWITCHER_ACTION_ACCEPT_PEER,         // 接受该对端：它就是当前目标
    GATEWAY_SWITCHER_ACTION_REJECT_PEER,         // 拒绝非目标对端（断开它）
    GATEWAY_SWITCHER_ACTION_UI_SWITCHING,        // 屏幕：正在切换到 <目标>
    GATEWAY_SWITCHER_ACTION_UI_CONNECTED,        // 屏幕：已连接 <目标>
    GATEWAY_SWITCHER_ACTION_UI_ERROR,            // 屏幕：切换失败（已回退）
    GATEWAY_SWITCHER_ACTION_UI_IDLE,             // 屏幕：未选目标
} gateway_switcher_action_t;

typedef struct {
    gateway_switcher_state_t state;
    gateway_switcher_peer_t selected;  // 选定目标（state != IDLE 时有效）
    bool selected_valid;
    gateway_switcher_peer_t previous;  // 切换前目标，用于超时回退
    bool previous_valid;
    gateway_switcher_peer_t current;   // 当前已连上的目标
    bool current_valid;
    uint32_t switch_started_ms;        // SWITCHING 起点，用于超时判定
    uint32_t timeout_ms;               // 切换超时（默认 GATEWAY_SWITCHER_TIMEOUT_MS）
} gateway_switcher_t;

#define GATEWAY_SWITCHER_TIMEOUT_MS 5000u

// 产出的动作序列（顺序即执行顺序）。
typedef struct {
    gateway_switcher_action_t items[GATEWAY_SWITCHER_MAX_ACTIONS];
    int count;
} gateway_switcher_actions_t;

void gateway_switcher_init(gateway_switcher_t *switcher);

bool gateway_switcher_peer_equal(const gateway_switcher_peer_t *lhs,
                                 const gateway_switcher_peer_t *rhs);

// 用户选择目标（编码器菜单确认）。若已经是该目标且已连上，只回 UI_CONNECTED，不重复切换。
void gateway_switcher_select(gateway_switcher_t *switcher, const gateway_switcher_peer_t *target,
                             uint32_t now_ms, gateway_switcher_actions_t *out);

// 回到「不限制」状态（清空选定目标；用于菜单里的"取消选择/自动"）。
void gateway_switcher_clear_selection(gateway_switcher_t *switcher,
                                      gateway_switcher_actions_t *out);

// 目标侧对端连上（BLE_GAP_EVENT_CONNECT 成功后调用）。
// 命中选定目标 → CONNECTED；否则（SWITCHING/CONNECTED 期间）请求拒绝。
void gateway_switcher_peer_connected(gateway_switcher_t *switcher,
                                     const gateway_switcher_peer_t *peer, uint32_t now_ms,
                                     gateway_switcher_actions_t *out);

// 目标侧对端断开。
void gateway_switcher_peer_disconnected(gateway_switcher_t *switcher, uint32_t now_ms,
                                        gateway_switcher_actions_t *out);

// 周期调用（主循环/定时器）：处理切换超时 → 回退上一目标并报错。
void gateway_switcher_tick(gateway_switcher_t *switcher, uint32_t now_ms,
                           gateway_switcher_actions_t *out);

const char *gateway_switcher_state_name(gateway_switcher_state_t state);
const char *gateway_switcher_action_name(gateway_switcher_action_t action);
