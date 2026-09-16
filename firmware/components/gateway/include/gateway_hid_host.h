// gateway_hid_host.h — 小米遥控器 central 链路（连接/配对/订阅/按键沿回调）
//
// 连接管理移植自 Phase 0 spike（真机六项验证通过），生产化差异：
//   - 重试调度改用 ble_npl_callout（NimBLE host 上下文，避免跨任务调 BLE API 的竞态）
//   - 服务发现只定位 HID(0x1812) 内第一个带 notify 的 Report 特征并订阅（spike 结论：
//     Report ID 1 通道；小米 notify 无需 CCCD 也会推，CCCD 写入失败不视为错误）
//   - 配对身份地址持久化 NVS，重启 direct connect 回连
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 按键沿回调：usage 为小米键盘页 usage（翻译由上层 gateway_keymap 做）
typedef void (*gateway_hid_key_cb_t)(uint16_t usage, bool pressed);
// 链路回调：connected=true 表示加密+订阅就绪的可用链路
typedef void (*gateway_hid_link_cb_t)(bool connected);

esp_err_t gateway_hid_host_start(gateway_hid_key_cb_t on_key,
                                 gateway_hid_link_cb_t on_link);
void gateway_hid_host_stop(void);
bool gateway_hid_host_connected(void);
