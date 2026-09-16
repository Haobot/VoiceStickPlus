// gateway_hid_host.c — 小米遥控器 central 链路实现
#include "gateway_hid_host.h"

#include <string.h>

#include "esp_log.h"
#include "gateway_report_parser.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nvs.h"

static const char *TAG = "gw_hid";

#define NVS_NS "gateway"
#define NVS_KEY_PEER "xiaomi_peer"
// 重试间隔用 FreeRTOS tick 换算（与 voice_ble 的 callout 用法一致）
#define RETRY_DELAY_TICKS pdMS_TO_TICKS(3000)
#define START_DELAY_TICKS pdMS_TO_TICKS(1000)

#define UUID_HID_SVC 0x1812
#define UUID_REPORT 0x2A4D
#define CHR_PROP_NOTIFY 0x10

// 名称白名单（trim+小写子串匹配；u-rfrc 为 Phase 0 取证发现的正常态广播名）
static const char *kNameWhitelist[] = {
    "mi rc", "xiaomi bluetooth remote 2 pro", "小米蓝牙语音遥控器", "rc001", "rc003", "u-rfrc",
};

static gateway_hid_key_cb_t s_on_key;
static gateway_hid_link_cb_t s_on_link;
static bool s_running;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;  // 小米侧 Report 特征值句柄（notify 源）
static uint8_t s_own_addr_type;
static gateway_report_parser_t s_parser;
static struct ble_npl_callout s_retry_callout;
static bool s_retry_callout_inited;

static void start_scan(void);
static void try_connect_known(void);

// ---- 对端地址持久化 ----

static void save_peer(const uint8_t *addr, uint8_t type) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  uint8_t blob[7] = {type, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]};
  (void)nvs_set_blob(h, NVS_KEY_PEER, blob, sizeof(blob));
  (void)nvs_commit(h);
  nvs_close(h);
}

static bool load_peer(uint8_t *addr, uint8_t *type) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
    return false;
  }
  uint8_t blob[7];
  size_t len = sizeof(blob);
  bool ok = nvs_get_blob(h, NVS_KEY_PEER, blob, &len) == ESP_OK && len == 7;
  nvs_close(h);
  if (ok) {
    *type = blob[0];
    memcpy(addr, blob + 1, 6);
  }
  return ok;
}

// ---- GATT 发现（定位 notify Report 特征并订阅） ----

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg);
static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

struct hid_range {
  uint16_t start;
  uint16_t end;
};

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg) {
  struct hid_range *range = arg;
  if (error->status == BLE_HS_EDONE) {
    if (range->start == 0) {
      ESP_LOGW(TAG, "小米侧未发现 HID 服务");
      return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, range->start, range->end, on_chr, range);
    if (rc != 0) {
      ESP_LOGE(TAG, "disc_all_chrs 失败 rc=%d", rc);
    }
    return 0;
  }
  if (error->status != 0) {
    ESP_LOGE(TAG, "服务枚举失败 rc=%d", error->status);
    return 0;
  }
  if (ble_uuid_u16(&svc->uuid.u) == UUID_HID_SVC) {
    range->start = svc->start_handle;
    range->end = svc->end_handle;
  }
  return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
  struct hid_range *range = arg;
  if (error->status == BLE_HS_EDONE) {
    if (s_report_handle == 0) {
      ESP_LOGW(TAG, "HID 服务内无带 notify 的 Report 特征");
      return 0;
    }
    // 订阅：标准做法写 CCCD（小米实测无需 CCCD 也会推送，写失败不视为错误）
    int rc = ble_gattc_disc_all_dscs(conn, s_report_handle + 1, s_report_handle + 5,
                                     on_dsc, NULL);
    if (rc != 0) {
      ESP_LOGI(TAG, "CCCD 枚举启动失败 rc=%d（小米可免 CCCD，继续）", rc);
    }
    return 0;
  }
  if (error->status != 0) {
    return 0;
  }
  if (ble_uuid_u16(&chr->uuid.u) == UUID_REPORT &&
      (chr->properties & CHR_PROP_NOTIFY) &&
      chr->val_handle >= range->start && chr->val_handle <= range->end &&
      s_report_handle == 0) {
    s_report_handle = chr->val_handle;
    ESP_LOGI(TAG, "锁定小米 Report 特征 0x%04x，链路就绪", s_report_handle);
    if (s_on_link != NULL) {
      s_on_link(true);
    }
  }
  return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  (void)chr_val_handle; (void)arg;
  if (error->status == BLE_HS_EDONE) {
    return 0;
  }
  if (error->status != 0) {
    return 0;
  }
  if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
    // 静态生命周期：write_flat 异步引用
    static const uint16_t enable_notify = 0x0001;
    (void)ble_gattc_write_flat(conn, dsc->handle, &enable_notify, sizeof(enable_notify),
                               NULL, NULL);
  }
  return 0;
}

static void explore_hid(uint16_t conn) {
  s_report_handle = 0;
  static struct hid_range range;  // 静态：异步回调链携带
  range.start = range.end = 0;
  int rc = ble_gattc_disc_all_svcs(conn, on_svc, &range);
  if (rc != 0) {
    ESP_LOGE(TAG, "disc_all_svcs 失败 rc=%d", rc);
  }
}

// ---- GAP 事件 ----

static void schedule_retry(void) {
  if (!s_running) {
    return;
  }
  ble_npl_callout_reset(&s_retry_callout, RETRY_DELAY_TICKS);
}

static void retry_cb(struct ble_npl_event *ev) {
  (void)ev;
  if (!s_running) {
    return;
  }
  // 此处 host 已完成同步（callout 首启延迟 1s），推断本机地址类型供扫描/连接使用
  if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
    ESP_LOGE(TAG, "本机地址获取失败，3s 后重试");
    schedule_retry();
    return;
  }
  try_connect_known();
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) {
        ESP_LOGW(TAG, "连接小米失败 status=%d，3s 后重试", event->connect.status);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        schedule_retry();
        return 0;
      }
      s_conn = event->connect.conn_handle;
      ble_gattc_exchange_mtu(s_conn, NULL, NULL);
      if (ble_gap_security_initiate(s_conn) != 0) {
        // 已 bond 时此处即用 LTK 恢复；失败走重试
        ESP_LOGW(TAG, "security_initiate 失败，3s 后重试");
        schedule_retry();
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
      if (event->enc_change.status != 0) {
        // 非配对模式下的拒绝属预期（Phase 0 结论）：静默重试等待遥控器可配对状态
        ESP_LOGI(TAG, "加密失败 status=%d（遥控器可能未进配对模式），3s 后重试",
                 event->enc_change.status);
        return 0;
      }
      {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
          save_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);
        }
      }
      gateway_report_parser_reset(&s_parser);
      explore_hid(event->enc_change.conn_handle);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle != s_conn) {
        return 0;
      }
      ESP_LOGW(TAG, "小米断开 reason=%d，3s 后重试", event->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_report_handle = 0;
      if (s_on_link != NULL) {
        s_on_link(false);
      }
      schedule_retry();
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      struct os_mbuf *om = event->notify_rx.om;
      if (event->notify_rx.attr_handle != s_report_handle || om == NULL) {
        return 0;
      }
      size_t len = OS_MBUF_PKTLEN(om);
      if (len < GATEWAY_REPORT_LEN) {
        return 0;
      }
      uint8_t data[GATEWAY_REPORT_LEN];
      ble_hs_mbuf_to_flat(om, data, sizeof(data), NULL);
      uint16_t pressed[GATEWAY_REPORT_SLOTS], released[GATEWAY_REPORT_SLOTS];
      size_t pc, rc_count;
      if (gateway_report_parser_feed(&s_parser, data, sizeof(data), pressed, &pc,
                                     released, &rc_count) != 0) {
        return 0;
      }
      if (s_on_key != NULL) {
        for (size_t i = 0; i < pc; i++) {
          s_on_key(pressed[i], true);
        }
        for (size_t i = 0; i < rc_count; i++) {
          s_on_key(released[i], false);
        }
      }
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
      // 反复测试场景：删旧 bond 重配避免卡死
      return BLE_GAP_REPEAT_PAIRING_RETRY;

    default:
      return 0;
  }
}

// ---- 扫描 ----

static void to_lower_str(char *s) {
  for (; *s; s++) {
    if (*s >= 'A' && *s <= 'Z') {
      *s += 32;
    }
  }
}

static bool name_matches(const uint8_t *name, uint8_t name_len) {
  char buf[64];
  if (name_len == 0 || name_len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, name, name_len);
  buf[name_len] = '\0';
  to_lower_str(buf);
  for (size_t i = 0; i < sizeof(kNameWhitelist) / sizeof(kNameWhitelist[0]); i++) {
    if (strstr(buf, kNameWhitelist[i]) != NULL) {
      return true;
    }
  }
  return false;
}

static int disc_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  if (event->type != BLE_GAP_EVENT_DISC) {
    return 0;
  }
  struct ble_hs_adv_fields fields;
  if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
    return 0;
  }
  if (!name_matches(fields.name, fields.name_len)) {
    return 0;  // 未带名称的报文等 scan rsp 再判
  }
  char name[64] = {0};
  memcpy(name, fields.name, fields.name_len < sizeof(name) - 1 ? fields.name_len
                                                                : sizeof(name) - 1);
  ESP_LOGI(TAG, "扫描命中小米：\"%s\" rssi=%d", name, event->disc.rssi);
  ble_gap_disc_cancel();
  static const struct ble_gap_conn_params params = {
      .scan_itvl = 0x0010, .scan_window = 0x0010,
      .itvl_min = 24, .itvl_max = 40,
      .latency = 0, .supervision_timeout = 400,
      .min_ce_len = 0, .max_ce_len = 0,
  };
  int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 10000, &params,
                           gap_event, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "发起连接失败 rc=%d，3s 后重试", rc);
    schedule_retry();
  }
  return 0;
}

static void start_scan(void) {
  struct ble_gap_disc_params params = {
      .passive = 0, .itvl = 0, .window = 0, .filter_duplicates = 1,
  };
  int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, disc_event, NULL);
  if (rc != 0) {
    ESP_LOGW(TAG, "扫描启动失败 rc=%d，3s 后重试", rc);
    schedule_retry();
  }
}

static void try_connect_known(void) {
  uint8_t addr[6], type;
  if (!load_peer(addr, &type)) {
    start_scan();
    return;
  }
  ble_addr_t peer = {.type = type};
  memcpy(peer.val, addr, 6);
  static const struct ble_gap_conn_params params = {
      .scan_itvl = 0x0010, .scan_window = 0x0010,
      .itvl_min = 24, .itvl_max = 40,
      .latency = 0, .supervision_timeout = 400,
      .min_ce_len = 0, .max_ce_len = 0,
  };
  int rc = ble_gap_connect(s_own_addr_type, &peer, 15000, &params, gap_event, NULL);
  if (rc != 0) {
    schedule_retry();
  }
}

// 说明：NimBLE host 只有一个 sync_cb，已被 voice_ble 占用；本模块不重复注册，
// 改由 start 时的延时 callout 在 host 完成同步后启动首次连接尝试（失败有重试兜底）。

// ---- 对外接口 ----

esp_err_t gateway_hid_host_start(gateway_hid_key_cb_t on_key,
                                 gateway_hid_link_cb_t on_link) {
  if (on_key == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_on_key = on_key;
  s_on_link = on_link;
  s_running = true;
  s_conn = BLE_HS_CONN_HANDLE_NONE;
  s_report_handle = 0;
  gateway_report_parser_reset(&s_parser);
  if (!s_retry_callout_inited) {
    ble_npl_callout_init(&s_retry_callout, nimble_port_get_dflt_eventq(), retry_cb, NULL);
    s_retry_callout_inited = true;
  }
  // 延迟 1s 等 host 完成 controller 同步（sync_cb 归 voice_ble），失败由重试兜底
  ble_npl_callout_reset(&s_retry_callout, START_DELAY_TICKS);
  ESP_LOGI(TAG, "小米 central 链路启动");
  return ESP_OK;
}

void gateway_hid_host_stop(void) {
  s_running = false;
  ble_npl_callout_stop(&s_retry_callout);
  if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) == 0) {
      ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
  }
  s_report_handle = 0;
  ESP_LOGI(TAG, "小米 central 链路停止");
}

bool gateway_hid_host_connected(void) { return s_report_handle != 0; }
