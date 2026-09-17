// gateway_hogp_report.h — HOGP Report Map 与 input report 报文构造（纯逻辑，无 NimBLE 依赖）
//
// 与 gateway_hogp.c 的分工：
//   - 本模块：HID Report Map 字节（描述符）与 Report 特征值报文（载荷）——可在主机侧单测；
//   - gateway_hogp.c：NimBLE GATT 服务定义、访问回调与 notify 发送。
//
// 关键约定（HID over GATT Profile / HIDS 1.0）：
//   1. Report 特征值 **不含 Report ID 前缀**。Report ID 由该特征的 Report Reference
//      描述符（0x2908）承载，报文里再带一次会被主机按"超长/错位"解析（键值永远不生效）。
//   2. 报文长度必须与 Report Map 声明的 Report Size × Report Count 完全一致。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Report ID 与报文长度（与 Report Map 声明一一对应，改动必须同步 host 侧单测）
#define GATEWAY_HOGP_REPORT_ID_CONSUMER 1u
#define GATEWAY_HOGP_REPORT_ID_KEYBOARD 2u
#define GATEWAY_HOGP_CONSUMER_REPORT_LEN 1u
#define GATEWAY_HOGP_KEYBOARD_REPORT_LEN 8u  // modifier + 保留 + 6 键槽

// Consumer 页 usage → Report ID 1 位图 bit；未在本 Report Map 声明返回 -1
int gateway_hogp_consumer_usage_bit(uint16_t usage);

// 两个 Report 特征的当前值（read 访问与 notify 发送同源）
typedef struct {
    uint8_t consumer[GATEWAY_HOGP_CONSUMER_REPORT_LEN];
    uint8_t keyboard[GATEWAY_HOGP_KEYBOARD_REPORT_LEN];
} gateway_hogp_reports_t;

void gateway_hogp_reports_reset(gateway_hogp_reports_t *reports);

// 更新 Consumer 位图：pressed=置位，false=清位。usage 未声明返回 -1（报文不变）
int gateway_hogp_reports_set_consumer(gateway_hogp_reports_t *reports, uint16_t usage, bool pressed);

// 更新键盘报文：pressed 时把 keycode 放进 6 键槽首槽，false 送全零释放帧
void gateway_hogp_reports_set_keyboard(gateway_hogp_reports_t *reports, uint8_t keycode, bool pressed);

// HID Report Map 字节（长度经 out_len 返回）
const uint8_t *gateway_hogp_report_map(size_t *out_len);
