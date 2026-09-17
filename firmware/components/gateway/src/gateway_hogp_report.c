// gateway_hogp_report.c — HOGP Report Map 与 input report 报文构造实现
//
// 真机排查（Phase 1，Windows 11 + 安卓双侧 HID 节点 Code 10）：
//   1. Report 特征值曾带 Report ID 前缀（2/9 字节），违反 HOGP"报文不含 Report ID"
//      —— 即使 HID 节点启动成功也会导致按键始终不生效，本轮修正；
//   2. Report Map 本身经 host 侧结构校验（test_gateway_logic.c）确认合法：数组项成对
//      声明 usage 范围且 min ≤ max、各 Report ID 位宽与报文长度一致。
#include "gateway_hogp_report.h"

#include <string.h>

// ---- Report Map ----
static const uint8_t kReportMap[] = {
    // Report ID 1: Consumer Control（位字段式，每键 1 bit）
    // 数组式（Ary）16 位 usage 是罕见写法，Windows HidParse 对 Consumer 数组支持差，
    // 商用键盘均为位图式（真机排查：数组式 Report Map 导致 HID 节点 Code 10）。
    // bit 映射：bit0=Mute bit1=Vol+ bit2=Vol- bit3=AC Back，报文 = 1 字节位图
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0x01,       //   Logical Max (1)
    0x75, 0x01,       //   Report Size (1)
    0x09, 0xE2,       //   Usage (Mute)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data, Var, Abs) —— bit0
    0x09, 0xE9,       //   Usage (Volume Increment)
    0x95, 0x01,
    0x81, 0x02,       //   —— bit1
    0x09, 0xEA,       //   Usage (Volume Decrement)
    0x95, 0x01,
    0x81, 0x02,       //   —— bit2
    0x0A, 0x24, 0x02, //   Usage (AC Back，16 位编码)
    0x95, 0x01,
    0x81, 0x02,       //   —— bit3
    0x95, 0x04,       //   Report Count (4) 补齐保留位
    0x81, 0x01,       //   Input (Const)
    0xC0,             // End Collection
    // Report ID 2: 标准键盘（modifier 位图 + 保留 + 6 键槽）
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x02,       //   Report ID (2)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Min (LeftCtrl)
    0x29, 0xE7,       //   Usage Max (Right GUI)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0x01,       //   Logical Max (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Var, Abs) —— modifier 位图
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Const) —— 保留字节
    // 6 键槽数组必须自带 Usage/Logical 范围：全局项沿用 modifier 段的 0..1，
    // 键码值（如 0x4F）超出 logical range 会被 Windows HidParse 拒绝。
    // Usage Maximum 用 2 字节形式（0x2A，即 0x28|bSize2）写 0x00FF：1 字节形式
    // （0x29 0xFF）在部分解析器里按有符号 -1 处理，2 字节形式无歧义。
    0x19, 0x00,       //   Usage Min (0)
    0x2A, 0xFF, 0x00, //   Usage Max (0xFF)
    0x15, 0x00,       //   Logical Min (0)
    0x25, 0xFF,       //   Logical Max (0xFF)
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x00,       //   Input (Data, Array, Abs) —— 6 键槽数组
    0xC0,             // End Collection
};

const uint8_t *gateway_hogp_report_map(size_t *out_len) {
    if (out_len != NULL) {
        *out_len = sizeof(kReportMap);
    }
    return kReportMap;
}

// ---- Consumer 位图 ----
// 顺序与 Report Map 中 Input 条目的声明顺序严格一致（bit0 先声明）
int gateway_hogp_consumer_usage_bit(uint16_t usage) {
    switch (usage) {
        case 0x00E2: return 0;  // Mute
        case 0x00E9: return 1;  // Volume Increment
        case 0x00EA: return 2;  // Volume Decrement
        case 0x0224: return 3;  // AC Back
        default: return -1;
    }
}

// 编译期不变量：报文缓冲区长度必须与对外声明的报文长度常量一致（负长度数组=编译失败）
typedef char gateway_hogp_assert_consumer_len
    [(sizeof(((gateway_hogp_reports_t *)0)->consumer) == GATEWAY_HOGP_CONSUMER_REPORT_LEN) ? 1 : -1];
typedef char gateway_hogp_assert_keyboard_len
    [(sizeof(((gateway_hogp_reports_t *)0)->keyboard) == GATEWAY_HOGP_KEYBOARD_REPORT_LEN) ? 1 : -1];

void gateway_hogp_reports_reset(gateway_hogp_reports_t *reports) {
    if (reports == NULL) {
        return;
    }
    memset(reports, 0, sizeof(*reports));
}

int gateway_hogp_reports_set_consumer(gateway_hogp_reports_t *reports, uint16_t usage, bool pressed) {
    if (reports == NULL) {
        return -1;
    }
    int bit = gateway_hogp_consumer_usage_bit(usage);
    if (bit < 0) {
        return -1;  // 未在 Report Map 位图声明的 usage
    }
    if (pressed) {
        reports->consumer[0] |= (uint8_t)(1u << bit);
    } else {
        reports->consumer[0] &= (uint8_t)~(uint8_t)(1u << bit);
    }
    return 0;
}

void gateway_hogp_reports_set_keyboard(gateway_hogp_reports_t *reports, uint8_t keycode, bool pressed) {
    if (reports == NULL) {
        return;
    }
    // [0]=modifier 位图 [1]=保留 [2..7]=6 键槽；网关只发单键，直接放首槽
    memset(reports->keyboard, 0, sizeof(reports->keyboard));
    if (pressed) {
        reports->keyboard[2] = keycode;
    }
}
