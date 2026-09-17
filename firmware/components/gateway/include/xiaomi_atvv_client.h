// xiaomi_atvv_client.h — 小米遥控器 ATVV 语音链路（NimBLE 薄壳）
//
// 承担纯逻辑状态机 gateway_atvv_session 与 BLE 的接线：ATVV 服务发现/特征
// 订阅/写 TX/notify 分发/周期 tick。会话语义（caps 握手/会话沿/尾包宽限）
// 全在 gateway_atvv_session；按键沿与 PCM 帧经回调交 main.c 主键链与
// audio_pipeline（方案 Doc/Plan/xiaomi-remote-stick-gateway.md §5.4.1）。
//
// 生命周期：网关模式启动时 xiaomi_atvv_client_start 注册回调；HID 链路就绪后
// main.c 调 xiaomi_atvv_client_on_link_ready（延迟串行在 HID 发现之后，NimBLE
// 每连接同时仅一个 GATT 过程）；断开调 on_link_down 复位会话。
// 线程契约：所有入口都在 NimBLE host 任务上下文调用（含回调）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 语音键按下/松开沿（main.c 注入 APP_INPUT_SOURCE_XIAOMI 主键链）
typedef void (*xiaomi_atvv_press_cb_t)(bool pressed);
// 40ms PCM 帧（audio_pipeline 外部源馈送）
typedef void (*xiaomi_atvv_pcm_cb_t)(const int16_t *pcm, size_t samples);

esp_err_t xiaomi_atvv_client_start(xiaomi_atvv_press_cb_t on_press,
                                   xiaomi_atvv_pcm_cb_t on_pcm);
void xiaomi_atvv_client_stop(void);
// HID 链路就绪（加密+订阅完成）后调用：conn 为小米连接句柄
void xiaomi_atvv_client_on_link_ready(uint16_t conn);
// 小米链路断开/网关模式退出时调用：复位会话（连接已断，MIC_CLOSE 不再可发）
void xiaomi_atvv_client_on_link_down(void);
// notify 路由入口（注册给 gateway_hid_host_set_notify_router，勿直接调用）
void xiaomi_atvv_client_notify_router(uint16_t attr_handle, const uint8_t *data,
                                      size_t len);
// 语音键当前按住态（ATVV 会话 STREAMING 中）：无本地 GPIO，以此代替电平判定
bool xiaomi_atvv_client_voice_pressed(void);
