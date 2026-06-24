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

#ifdef __cplusplus
}
#endif
