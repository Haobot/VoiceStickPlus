// gateway_mode.h — 网关/普通双模式状态（NVS 持久化）
//
// 普通模式：行为与现状完全一致（网关模块不启动，零回归）。
// 网关模式：启动小米 central 链路 + HOGP 按键直通输出。
// 切换入口（Phase 1）：按住编码器按键上电翻转模式（boot 窗口采样，不占用运行期按键语义）；
// Phase 3 屏幕菜单就绪后替换为正式 UI 入口。
#pragma once

#include "esp_err.h"

typedef enum {
    GATEWAY_MODE_NORMAL = 0,   // 普通语音棒（现状行为）
    GATEWAY_MODE_GATEWAY = 1,  // 网关（小米中转）
} gateway_mode_t;

esp_err_t gateway_mode_init(void);            // 读取 NVS 持久化模式
gateway_mode_t gateway_mode_get(void);
esp_err_t gateway_mode_toggle(void);          // 翻转并持久化
const char *gateway_mode_name(gateway_mode_t mode);  // 中文显示名
