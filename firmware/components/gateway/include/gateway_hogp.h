// gateway_hogp.h — HOGP 外设输出（HID over GATT 按键直通通道）
//
// 服务定义由 gateway_hogp_services() 导出，经 voice_ble 的 extra_svcs 钩子在
// NimBLE 注册窗口（nimble_port_freertos_init 之前）注册；普通模式下服务同样存在
// 但无按键流，行为零回归。目标设备（Mac/Windows）需在系统蓝牙设置里配对 StickS3
// 一次，之后系统 HID 栈直接消费这里的 input report（三键全平台识别的落点）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ble_gatt_svc_def;

// 返回 HOGP 服务数组（含 HID 0x1812：Information/Report Map/Control Point/两个 Report）
const struct ble_gatt_svc_def *gateway_hogp_services(size_t *count);

// 发送键盘 input report（Report ID 2，标准 8 字节键盘报文）
// keycode 为 HID 键盘页键值；pressed=false 发全零释放帧。无连接/未订阅返回非 0。
int gateway_hogp_send_keyboard(uint8_t keycode, bool pressed);

// 发送 Consumer input report（Report ID 1，2×LE16 usage 数组）
// usage 为 Consumer 页 16 位标准 usage；pressed=false 发全零释放帧。
int gateway_hogp_send_consumer(uint16_t usage, bool pressed);
