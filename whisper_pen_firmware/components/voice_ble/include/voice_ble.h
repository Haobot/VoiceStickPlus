#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define VOICE_BLE_DEVICE_NAME_PREFIX "WP"   // Whisper Pen

// 音频帧标志位（Opus 帧封装头里的 flags 字节）
#define VOICE_BLE_FLAG_START 0x01
#define VOICE_BLE_FLAG_END   0x02

// Opus 帧长（与 audio_pipeline 对齐），用于发送时计算 Timestamp
#define VOICE_BLE_AUDIO_FRAME_MS 40

// BLE GATT UUID（评估方案要求 16-bit：服务 0xFF10、音频 0xFF11 notify）
#define VOICE_BLE_SVC_UUID     0xFF10
#define VOICE_BLE_CHR_AUDIO_TX 0xFF11   // Opus 帧 notify
#define VOICE_BLE_CHR_CONTROL  0xFF12   // 主机下行 control（write）

// 初始化 NimBLE 协议栈 + 注册 GATT 服务 + 启动广播。广播在 on_sync 回调里异步开始。
esp_err_t voice_ble_init(void);

bool voice_ble_is_connected(void);

// 发送 Opus 音频帧。封装为 [SeqNum u16][Timestamp u32][Flags u8][Opus payload]。
// MTU 协商到 247 后，单帧（<=220+头）一次 Notify 发完。
esp_err_t voice_ble_send_audio(uint32_t session_id, uint32_t seq, uint8_t flags,
                                const uint8_t *opus_payload, size_t len);

// 发送按键事件。脚手架聚焦音频链路，state 通道此处仅日志，后续扩展为 notify。
esp_err_t voice_ble_send_button_event(const char *button, bool pressed, uint32_t session_id);

// 录音期强制 7.5ms 快 interval（低延迟头号参数）；空闲恢复慢 interval 省电。
esp_err_t voice_ble_request_fast_interval(void);
esp_err_t voice_ble_request_slow_interval(void);
