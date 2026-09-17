// gateway_hogp.c — HOGP 服务与 input report 输出实现
//
// Report Map 与报文构造在 gateway_hogp_report.c（纯逻辑，host 侧单测覆盖）。
// 本文件只负责 NimBLE GATT 服务定义、访问回调与 notify 发送。
//
// 真机结论（Phase 1，Windows 11 + 安卓两侧 HID 节点 Code 10）：
//   主机（Windows hidbthle / 安卓 bta_hh_le）启动 HID 设备前必须读每个 Report 特征的
//   Report Reference 描述符（0x2908）才能建立"特征句柄 ↔ Report ID/类型"映射。
//   自定义描述符的 att_flags 若留 0，NimBLE 会按"无 READ 权限"登记该属性，
//   任何 Read Request 都被回 Read Not Permitted（BLE_ATT_ERR_READ_NOT_PERMITTED，
//   见 nimble/host/src/ble_att_svr.c 权限检查），主机拿不到映射即报 Code 10。
#include "gateway_hogp.h"

#include <string.h>

#include "esp_log.h"
#include "gateway_hogp_report.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include "os/os_mbuf.h"
#include "stick_s3_board.h"

static const char *TAG = "gw_hogp";

// HID Information：bcdHID 1.11 / country 0 / flags 0x02（Normally Connectable）。
// flags 必须声明 Normally Connectable，否则 Windows 配对完成后不会自动回连
// （真机排查：配对成功后蓝牙设置里永远"无法连接"）。
static const uint8_t kHidInfo[4] = {0x11, 0x01, 0x00, 0x02};

// 两个 Report 特征的当前报文（read 访问与 notify 发送同源，不含 Report ID 前缀）
static gateway_hogp_reports_t s_reports;

// Report Reference 描述符：reportID + 类型（0x01=Input）
static const uint8_t kRefConsumer[2] = {GATEWAY_HOGP_REPORT_ID_CONSUMER, 0x01};
static const uint8_t kRefKeyboard[2] = {GATEWAY_HOGP_REPORT_ID_KEYBOARD, 0x01};

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
    size_t len = 0;
    const uint8_t *map = gateway_hogp_report_map(&len);
    return os_mbuf_append(ctxt->om, map, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_control_point(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    // 写入（suspend/resume）在网关场景无意义，静默接受
    return ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR ? 0 : BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int access_report_consumer(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, s_reports.consumer, sizeof(s_reports.consumer)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_report_keyboard(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    return os_mbuf_append(ctxt->om, s_reports.keyboard, sizeof(s_reports.keyboard)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
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

// Protocol Mode (0x2A4E)：HIDS 规范要求的必选特征。网关只支持 Report Protocol，
// 读恒返回 0x01；写静默接受（Windows 仅在支持 Boot Protocol 时才会写 0）。
static int access_protocol_mode(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t mode = 0x01;
        return os_mbuf_append(ctxt->om, &mode, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// Battery Level (0x2A19)：HOGP 键盘必备的电量特征（Windows 会读并在设备页展示）
static int access_battery_level(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    int level = 100;
    if (stick_s3_board_battery_level(&level) != ESP_OK || level < 0 || level > 100) {
        level = 100;
    }
    const uint8_t v = (uint8_t)level;
    return os_mbuf_append(ctxt->om, &v, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ---- 服务定义 ----
// 注意 1：带 notify 的特征 NimBLE 自动生成 CCCD(0x2902)，描述符表只声明 Report Reference；
// 注意 2：自定义描述符必须显式给 att_flags=BLE_ATT_F_READ——留 0 等于"无读权限"，
//         主机读 Report Reference 会被回 Read Not Permitted（Code 10 根因，见文件头）。

static const struct ble_gatt_dsc_def s_report_consumer_dscs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),  // Report Reference: Report ID 1, Input
        .att_flags = BLE_ATT_F_READ,         // 必须：否则主机读不到 Report ID 映射
        .access_cb = access_ref_consumer,
    },
    {0},
};

static const struct ble_gatt_dsc_def s_report_keyboard_dscs[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2908),  // Report Reference: Report ID 2, Input
        .att_flags = BLE_ATT_F_READ,
        .access_cb = access_ref_keyboard,
    },
    {0},
};

static const struct ble_gatt_chr_def s_hid_chars[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4E),  // Protocol Mode（HIDS 必选）
        .access_cb = access_protocol_mode,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
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
        .uuid = BLE_UUID16_DECLARE(0x2A4D),  // Report（Consumer，1 字节位图）
        .access_cb = access_report_consumer,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_consumer,
        .descriptors = s_report_consumer_dscs,
    },
    {
        .uuid = BLE_UUID16_DECLARE(0x2A4D),  // Report（Keyboard，8 字节标准键盘报文）
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

static const struct ble_gatt_chr_def s_bas_chars[] = {
    {
        .uuid = BLE_UUID16_DECLARE(0x2A19),  // Battery Level
        .access_cb = access_battery_level,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {0},
};

static const struct ble_gatt_svc_def s_hogp_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),  // HID Service
        .characteristics = s_hid_chars,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),  // Battery Service
        .characteristics = s_bas_chars,
    },
    {0},
};

const struct ble_gatt_svc_def *gateway_hogp_services(size_t *count) {
    gateway_hogp_reports_reset(&s_reports);  // 注册窗口内初始化报文基线
    if (count != NULL) {
        *count = sizeof(s_hogp_svcs) / sizeof(s_hogp_svcs[0]) - 1;
    }
    return s_hogp_svcs;
}

// ---- 发送 ----

// 找外设侧连接（本机为 slave、目标设备为主机的链路）。网关模式下有两条连接：
// central 侧连小米（role=master）与 peripheral 侧连目标设备（role=slave），且实测
// conn_handle 可超过 CONFIG_BT_NIMBLE_MAX_CONNECTIONS-1（handle 从 1 起编）——
// 按句柄 0..MAX-1 扫描既可能选错小米链路，也可能扫不到 handle 2，必须按角色过滤。
// NimBLE 的 ble_gap_conn_foreach_handle 未在公共头声明，这里小范围扫句柄代替。
static uint16_t first_periph_handle(void) {
    for (uint16_t h = 0; h < 16; h++) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(h, &desc) == 0 && desc.role == BLE_GAP_ROLE_SLAVE) {
            return h;
        }
    }
    return BLE_HS_CONN_HANDLE_NONE;
}

static int send_report(uint16_t chr_handle, const uint8_t *report, size_t len) {
    uint16_t conn = first_periph_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, len);
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    int rc = ble_gatts_notify_custom(conn, chr_handle, om);
    if (rc != 0) {
        // 未订阅（主机未写 CCCD）时 NimBLE 返回成功但不下发；真失败多为无连接/未订阅
        ESP_LOGW(TAG, "hogp notify chr=0x%04x len=%u rc=%d", chr_handle, (unsigned)len, rc);
    }
    return rc;
}

int gateway_hogp_send_keyboard(uint8_t keycode, bool pressed) {
    gateway_hogp_reports_set_keyboard(&s_reports, keycode, pressed);
    return send_report(s_chr_keyboard, s_reports.keyboard, sizeof(s_reports.keyboard));
}

int gateway_hogp_send_consumer(uint16_t usage, bool pressed) {
    if (gateway_hogp_reports_set_consumer(&s_reports, usage, pressed) != 0) {
        return BLE_HS_ENOTSUP;  // 未在 Report Map 位图声明的 usage
    }
    return send_report(s_chr_consumer, s_reports.consumer, sizeof(s_reports.consumer));
}
