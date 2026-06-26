#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 桌面端通过 BLE 下发 Wi-Fi STA 凭据后，固件用 state_tx 回报当前快照。
// 协议契约见 Doc/Ref/protocol.md "Wi-Fi Provisioning and HTTPS OTA Pull" 章节，
// 实施计划见 Doc/Plan/wifi-sta-ble-provisioning.md。

// 上层注入：把已经拼好的 wifi_status JSON 文本推到 BLE state_tx 通道。
// voice_net 不直接依赖 voice_ble，避免组件互相引用。
typedef esp_err_t (*voice_net_status_publish_fn)(const char *json);

// 初始化 Wi-Fi STA 子系统：注册事件回调、加载 NVS 凭据、有凭据则立即异步连接。
// 必须在 voice_ble_init 之后调用（依赖其完成 nvs_flash_init）。
// publish 不可为空——所有状态变化都通过它推送出去。
esp_err_t voice_net_init(voice_net_status_publish_fn publish);

// 桌面端 control_rx 收到 wifi_set 时调用：保存凭据到 NVS，800ms 后异步重连。
// ssid 长度 1..32，password 长度 0..63（空表示开放网络）。
// 超长会被截断并把 last_error 置为 "payload_too_large"。
void voice_net_apply_credentials(const char *ssid, const char *password);

// 桌面端 control_rx 收到 wifi_clear 时调用：擦除 NVS 凭据并断开 STA。
void voice_net_clear_credentials(void);

// 桌面端 control_rx 收到 wifi_status_request 时调用：立即补推一帧 wifi_status。
// BLE 重连后也应主动调一次让桌面端 UI 显示当前 IP。
void voice_net_publish_status(void);

// 在 BLE 已经稳定连上后调用。如果 NVS 里有持久化凭据，此时才真正启动 Wi-Fi 栈并发起连接，
// 避免 boot 期间 Wi-Fi 初始化与 BLE 抢资源造成 UI 反复闪 Pairing 界面。
// 没有持久化凭据时是 no-op。
void voice_net_resume_if_configured(void);

// 由 main.c 注入的"是否允许启动 OTA"查询回调。voice_net 在 ota_pull 启动前会调用，
// 返回 false 时拒绝并把 wifi_status.last_error 置为 "ota_park_required"。
// 实现应检查：!s_recording && !s_ota_updating && !voice_ble_ota_is_active()。
// 详见 Doc/Plan/wifi-sta-ble-provisioning.md §2.5 Park gate。
typedef bool (*voice_net_park_query_fn)(void);
void voice_net_set_park_query(voice_net_park_query_fn cb);

// 桌面端 control_rx 收到 ota_pull 时调用：启动 esp_https_ota 拉取固件。
// url 必须以 https:// 开头且长度 ≤256；sha256_hex 可选（暂未实施校验，预留字段）。
// 内部异步执行，进度通过 wifi_status.ota_pull 子对象上报。
void voice_net_start_ota_pull(const char *url, const char *sha256_hex);

// 显式确认新固件健康，调 esp_ota_mark_app_valid_cancel_rollback。
// 桌面端 control_rx 收到 ota_commit 时由 main.c 调用；voice_net 内部也会在
// "boot 起来 N 秒 + BLE 至少连过一次" 时自动调一次（自动签到）。
// 没有 PENDING_VERIFY 状态时是 no-op。
void voice_net_mark_app_valid(void);

// 由 main.c 在 BLE 连上时调用一次，让 voice_net 知道"健康信号"满足，
// 配合启动时长可以触发自动签到。重复调用幂等。
void voice_net_notify_ble_connected(void);

// 查询当前固件是否处于 PENDING_VERIFY 状态（新固件首次启动）。
// 用于 wifi_status.ota_pending_verify 字段。
bool voice_net_is_pending_verify(void);

// Wi-Fi STA 状态变化回调：state 为字符串，如 "disabled"/"configured"/"connecting"/"connected"/"error"。
typedef void (*voice_net_status_changed_fn)(const char *ssid, const char *ip, const char *state);
void voice_net_set_status_changed_callback(voice_net_status_changed_fn cb);

// 读取当前 Wi-Fi STA 快照。ssid/ip 缓冲区由调用方提供；state 为内部静态字符串，无需释放。
void voice_net_get_status(char *ssid, size_t ssid_size, char *ip, size_t ip_size, const char **state);

#ifdef __cplusplus
}
#endif
