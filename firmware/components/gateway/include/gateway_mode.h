// gateway_mode.h — 网关模式状态（恒定网关）
//
// 网关模式向下兼容普通模式：桌面 app 直连链路（语音输入/OTA/状态上报）行为不变，
// 只是额外启动小米 central 链路 + HOGP 按键直通输出。历史上曾支持「冷启动按住
// 主键翻转普通/网关双模式」，因入口隐蔽（用户感知为"功能丢失"）且网关模式已
// 完全覆盖普通模式行为，已移除切换：设备恒定工作在网关模式。
// GATEWAY_MODE_NORMAL 枚举与 main.c 的模式判断分支保留（作为代码路径存在，
// gateway_status 协议字段语义不变，桌面端无需感知此变更）。
#pragma once

#include "esp_err.h"

typedef enum {
    GATEWAY_MODE_NORMAL = 0,   // 普通语音棒（历史语义，现不再进入）
    GATEWAY_MODE_GATEWAY = 1,  // 网关（小米中转），唯一运行模式
} gateway_mode_t;

// 强制进入网关模式并把 NVS 持久值覆写为网关（旧版本固件回滚时读到 1 仍是网关，
// 不会出现「升级后配对功能消失」的状态回退）。需在 voice_ble_init 之前调用。
esp_err_t gateway_mode_init(void);
gateway_mode_t gateway_mode_get(void);
const char *gateway_mode_name(gateway_mode_t mode);  // 中文显示名
