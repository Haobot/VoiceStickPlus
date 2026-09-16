// gateway_hogp.c — HOGP 服务与 input report 输出实现
//
// Report Map 自定义两个报告（与 gateway_keymap 的两类输出动作一一对应）：
//   Report ID 1：Consumer Control，2×LE16 usage 数组（数组式=按下集合，系统栈翻译标准 Consumer 键）
//   Report ID 2：标准键盘（modifier 位图 + 保留 + 6 键槽）
#include "gateway_hogp.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"

static const char *TAG = "gw_hogp";

// ---- HID Report Map（自定义键盘 + Consumer 双报告） ----
static const uint8_t kReportMap[] = {
    // Report ID 1: Consumer Control（2 槽 16 位 usage 数组）
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x05, 0x0C,       //   Usage Page (Consumer)
    0x15, 0x00,       //   Logical Min (0)
    0x26, 0xFF, 0x0F, //   Logical Max (0x0FFF)
    0x19, 0x00,       //   Usage Min (0)
    0x2A, 0xFF, 0x0F, //   Usage Max (0x0FFF)
    0x95, 0x02,       //   Report Count (2)
    0x75, 0x10,       //   Report Size (16)
    0x81, 0x00,       //   Input (Data, Array, Abs)
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
    0x19, 0x00,       //   Usage Min (0)
    0x2A, 0xFF, 0x00, //   Usage Max (0xFF)
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x00,       //   Input (Data, Array, Abs) —— 6 键槽数组
    0xC0,             // End Collection
};

// HID Information：bcdHID 1.11 / country 0 / flags 0（ Normally Connectable 不设，
// 广播常在即可）
static const uint8_t kHidInfo[4] = {0x11, 0x01, 0x00, 0x00};

// 两个 Report 特征的当前报文状态（read 访问与 notify 发送同源）
static uint8_t s_report_consumer[5];  // [ID, usage_lo, usage_hi, 0, 0]
static uint8_t s_report_keyboard[9];  // [ID, mods, resv, 6 键槽]

// Report Reference 描述符：reportID + Input 类型
static const uint8_t kRefConsumer[2] = {0x01, 0x01};  // Report ID 1, Input
static const uint8_t kRefKeyboard[2] = {0x02, 0x01};  // Report ID 2, Input

static uint16_t s_chr_consumer;  // 注册后回填的特征值句柄
static uint16_t s_chr_keyboard;

// ---- GATT 访问回调 ----

static int access_info(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, kHidInfo, sizeof(kHidInfo)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_report_map(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, kReportMap, sizeof(kReportMap)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_control_point(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    // 写入（suspend/resume）在网关场景无意义，静默接受
    return ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR ? 0 : BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int access_report_consumer(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, s_report_consumer, sizeof(s_report_consumer)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_report_keyboard(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, s_report_keyboard, sizeof(s_report_keyboard)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_ref_consumer(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, kRefConsumer, sizeof(kRefConsumer)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_ref_keyboard(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, kRefKeyboard, sizeof(kRefKeyboard)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ---- 服务定义 ----
// 注意：带 notify 的特征 NimBLE 自动生成 CCCD(0x2902)，描述符表只声明 Report Reference

static const struct ble_gatt_dsc_def s_report_consumer_dscs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),  // Report Reference: Report ID 1, Input
        .access_cb = access_ref_consumer,
    },
    {0},
};

static const struct ble_gatt_dsc_def s_report_keyboard_dscs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),  // Report Reference: Report ID 2, Input
        .access_cb = access_ref_keyboard,
    },
    {0},
};

static const struct ble_gatt_chr_def s_hid_chars[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4A),  // HID Information
        .access_cb = access_info,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4B),  // Report Map
        .access_cb = access_report_map,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),  // Report（Consumer）
        .access_cb = access_report_consumer,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_consumer,
        .descriptors = s_report_consumer_dscs,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),  // Report（Keyboard）
        .access_cb = access_report_keyboard,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_keyboard,
        .descriptors = s_report_keyboard_dscs,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4C),  // HID Control Point
        .access_cb = access_control_point,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {0},
};

static const struct ble_gatt_svc_def s_hogp_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),  // HID Service
        .characteristics = s_hid_chars,
    },
    {0},
};

const struct ble_gatt_svc_def *gateway_hogp_services(size_t *count) {
    if (count != NULL) {
        *count = sizeof(s_hogp_svcs) / sizeof(s_hogp_svcs[0]) - 1;
    }
    return s_hogp_svcs;
}

// ---- 发送 ----

// 找第一条已建立的外设连接（网关为单目标，Phase 3 多目标时由切换器指定）
static uint16_t first_connected_handle(void) {
    for (uint16_t h = 0; h < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; h++) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(h, &desc) == 0) {
            return h;
        }
    }
    return BLE_HS_CONN_HANDLE_NONE;
}

static int send_report(uint16_t chr_handle, const uint8_t *report, size_t len) {
    uint16_t conn = first_connected_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gatts_notify_custom(conn, chr_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify 失败 rc=%d（未订阅属正常）", rc);
    }
    return rc;
}

int gateway_hogp_send_keyboard(uint8_t keycode, bool pressed) {
    s_report_keyboard[0] = 0x02;
    memset(s_report_keyboard + 1, 0, sizeof(s_report_keyboard) - 1);
    if (pressed) {
        s_report_keyboard[3] = keycode;  // 6 键槽首槽
    }
    return send_report(s_chr_keyboard, s_report_keyboard, sizeof(s_report_keyboard));
}

int gateway_hogp_send_consumer(uint16_t usage, bool pressed) {
    s_report_consumer[0] = 0x01;
    memset(s_report_consumer + 1, 0, sizeof(s_report_consumer) - 1);
    if (pressed) {
        s_report_consumer[1] = (uint8_t)(usage & 0xFF);
        s_report_consumer[2] = (uint8_t)(usage >> 8);
    }
    return send_report(s_chr_consumer, s_report_consumer, sizeof(s_report_consumer));
}
