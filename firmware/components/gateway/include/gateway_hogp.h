// gateway_hogp.h — HOGP 外设输出（HID over GATT 按键直通通道）
//
// 服务定义由 gateway_hogp_services() 导出，经 voice_ble 的 extra_svcs 钩子在
// NimBLE 注册窗口（nimble_port_freertos_init 之前）注册；普通模式下服务同样存在
// 但无按键流，行为零回归。目标设备（Windows/安卓/macOS）需在系统蓝牙设置里配对
// StickS3 一次，之后系统 HID 栈直接消费这里的 input report（三键全平台识别的落点）。
//
// Report Map 与报文长度见 gateway_hogp_report.h；服务的 Report Reference 描述符
// 必须带 READ 权限（Code 10 根因，见 gateway_hogp.c 文件头）。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ble_gatt_svc_def;

// 返回 HOGP 服务数组（HID 0x1812：Information/Report Map/Control Point/两个 Report；
// BAS 0x180F：电量）
const struct ble_gatt_svc_def *gateway_hogp_services(size_t *count);

// 发送键盘 input report（Report ID 2，8 字节：modifier + 保留 + 6 键槽，不含 Report ID 前缀）
// keycode 为 HID 键盘页键值；pressed=false 发全零释放帧。无连接返回非 0。
int gateway_hogp_send_keyboard(uint8_t keycode, bool pressed);

// 发送 Consumer input report（Report ID 1，1 字节位图，不含 Report ID 前缀）
// usage 为 Consumer 页 16 位标准 usage；pressed=false 清对应位。usage 未声明返回非 0。
int gateway_hogp_send_consumer(uint16_t usage, bool pressed);
