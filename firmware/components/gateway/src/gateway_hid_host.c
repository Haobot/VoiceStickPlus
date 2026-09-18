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
#define UUID_REPORT_MAP 0x2A4B
#define CHR_PROP_NOTIFY 0x10

// 名称白名单（trim+小写子串匹配；u-rfrc 为 Phase 0 取证发现的正常态广播名）
static const char *kNameWhitelist[] = {
    "mi rc", "xiaomi bluetooth remote 2 pro", "小米蓝牙语音遥控器", "rc001", "rc003", "u-rfrc",
};

static gateway_hid_key_cb_t s_on_key;
static gateway_hid_link_cb_t s_on_link;
static gateway_hid_notify_router_t s_notify_router;
// latency 修正计次（每连接；防止对端反复强推省电参数造成循环）
static uint8_t s_latency_fix_count;
// Report CCCD 订阅状态与重试：CCCD 枚举/写是 ATT 事务，可能被小米 ~4.9s 的
// L2CAP 参数请求 0x212 窗口吞掉（真机 Phase 2 定案——与 ATVV 发现同机制），
// 订阅不成则小米永不推 HID 按键。写确认回调置位，3s 未确认重试。
static volatile bool s_report_cccd_written;
static uint8_t s_cccd_retry_count;
static struct ble_npl_callout s_cccd_retry_callout;
static bool s_cccd_retry_callout_inited;
static bool s_running;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;  // 小米侧 Report 特征值句柄（notify 源）
static uint16_t s_report_map_handle;  // Report Map(0x2A4B) 句柄（读取以解锁按键推送）
static uint8_t s_own_addr_type;
static gateway_report_parser_t s_parser;
static struct ble_npl_callout s_retry_callout;
static bool s_retry_callout_inited;
static struct ble_npl_callout s_sec_callout;
static bool s_sec_callout_inited;

static void start_scan(void);
static void try_connect_known(void);

// Report Map 长读回调：内容丢弃。读取本身是目的——spike 流程实证小米在主机
// 读过 ReportMap 后才开始推按键 notify（未读则只响应请求不主动推送，真机
// Phase 2 排查期对照发现）。
static int on_read_report_map(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)arg;
  if (error->status == BLE_HS_EDONE) {
    ESP_LOGI(TAG, "Report Map 读取完成（按键推送解锁条件）");
  }
  return 0;
}

// Report CCCD 订阅写完成回调：成功置订阅标志；Invalid Handle 等失败由
// retry callout 重试（同句柄）。
static int on_cccd_write_done(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (error->status == 0) {
    s_report_cccd_written = true;
    ESP_LOGI(TAG, "Report CCCD 订阅确认（按键推送通道开通）");
    if (s_report_map_handle != 0) {
      int rc = ble_gattc_read_long(s_conn, s_report_map_handle, 0, on_read_report_map,
                                   NULL);
      ESP_LOGI(TAG, "读 Report Map(0x%04x) rc=%d", s_report_map_handle, rc);
    }
  } else {
    ESP_LOGW(TAG, "Report CCCD 写失败 status=%d", error->status);
  }
  return 0;
}

// 直接写 Report 特征句柄 +1 的 CCCD：小米对描述符枚举（disc_all_dscs，
// Read By Type 0x2902）真机无响应（四连发全挂，与 svcs/chrs 枚举通形成对照），
// 改按 GATT 布局惯例直写（spike 实证 CCCD 句柄 = Report+1 = 0x0065）。
static void try_write_report_cccd(void) {
  static const uint16_t enable_notify = 0x0001;  // 静态生命周期：write_flat 异步引用
  uint16_t handle = (uint16_t)(s_report_handle + 1);
  int rc = ble_gattc_write_flat(s_conn, handle, &enable_notify, sizeof(enable_notify),
                                on_cccd_write_done, NULL);
  ESP_LOGI(TAG, "Report CCCD(0x%04x) 直写 0x0001 rc=%d", handle, rc);
}

static void cccd_retry_cb(struct ble_npl_event *ev) {
  (void)ev;
  if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE || s_report_handle == 0) {
    return;
  }
  if (s_report_cccd_written || s_cccd_retry_count >= 3) {
    return;
  }
  s_cccd_retry_count++;
  ESP_LOGW(TAG, "Report CCCD 订阅未确认，直写重试第 %u 次", s_cccd_retry_count);
  try_write_report_cccd();
  // 无论发起成败都续 arm：写再次被吞时下一次重试继续（written 置位后停）
  ble_npl_callout_reset(&s_cccd_retry_callout, pdMS_TO_TICKS(3000));
}

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

struct hid_range {
  uint16_t start;
  uint16_t end;
};

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg);

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
    // 订阅：直写 Report+1 的 CCCD（枚举路径小米无响应，见 try_write_report_cccd
    // 注释）；写是 ATT 事务，可能被 ~4.9s 参数窗口吞——retry callout 兜底重试。
    if (!s_cccd_retry_callout_inited) {
      ble_npl_callout_init(&s_cccd_retry_callout, nimble_port_get_dflt_eventq(),
                           cccd_retry_cb, NULL);
      s_cccd_retry_callout_inited = true;
    }
    s_report_cccd_written = false;
    s_cccd_retry_count = 0;
    try_write_report_cccd();
    ble_npl_callout_reset(&s_cccd_retry_callout, pdMS_TO_TICKS(3000));
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
  } else if (ble_uuid_u16(&chr->uuid.u) == UUID_REPORT_MAP &&
             chr->val_handle >= range->start && chr->val_handle <= range->end) {
    s_report_map_handle = chr->val_handle;
  }
  return 0;
}

static void explore_hid(uint16_t conn) {
  s_report_handle = 0;
  s_report_map_handle = 0;
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
  // 防堆积：连接仍在（含未断开的悬挂态）时不发起新连接尝试
  if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) == 0) {
      return;  // 连接真实存在，等它断开事件再重试
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
  }
  // 此处 host 已完成同步（callout 首启延迟 1s），推断本机地址类型供扫描/连接使用
  if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
    ESP_LOGE(TAG, "本机地址获取失败，3s 后重试");
    schedule_retry();
    return;
  }
  try_connect_known();
}

// 延迟发起加密：连接刚建立时 controller 状态未完全就绪（真机曾见 terminate 返回
// CMD_DISALLOWED、security_initiate 同步失败），等 MTU 交换完成后再发起
static void sec_cb(struct ble_npl_event *ev) {
  (void)ev;
  if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE) {
    return;
  }
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(s_conn, &desc) != 0) {
    return;
  }
  ESP_LOGI(TAG, "发起加密：conn=%d role=%s enc=%d bonded=%d", s_conn,
           desc.role == BLE_GAP_ROLE_MASTER ? "master" : "slave",
           desc.sec_state.encrypted, desc.sec_state.bonded);
  // 清除该对端的陈旧键再配对：NVS 跨烧录持久化，spike 时代的旧 LTK 会让对端
  // 以"已有键"姿态拒绝新配对（真机排查：连接后 ~1s 被对端掐断 HCI 0x13）
  (void)ble_store_util_delete_peer(&desc.peer_id_addr);
  int rc = ble_gap_security_initiate(s_conn);
  if (rc != 0) {
    // 失败必须终止连接再重试：悬挂连接会占满连接表（MAX_CONNECTIONS=2 含桌面
    // 端一条），后续 connect 全部 ENOMEM 死循环（真机验收实测教训）
    ESP_LOGW(TAG, "security_initiate 失败 rc=%d，断开后重试", rc);
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
  }
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
      s_latency_fix_count = 0;
      s_report_cccd_written = false;
      s_cccd_retry_count = 0;
      ble_npl_callout_stop(&s_cccd_retry_callout);
      ESP_LOGI(TAG, "已连接小米 conn=%d", s_conn);
      ble_gattc_exchange_mtu(s_conn, NULL, NULL);
      if (!s_sec_callout_inited) {
        ble_npl_callout_init(&s_sec_callout, nimble_port_get_dflt_eventq(), sec_cb, NULL);
        s_sec_callout_inited = true;
      }
      ble_npl_callout_reset(&s_sec_callout, pdMS_TO_TICKS(300));
      return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
      // 小米经 L2CAP signaling 请求省电参数（连接后 ~4.9s 一次性；不走应用的
      // CONN_UPDATE_REQ 路径，NimBLE 自动转发 HCI）。其组合（itvl=10/latency=49/
      // timeout=500）违反 BLE 规范 timeout > 2*(1+latency)*itvl_max，controller
      // 以 HCI 0x212 同步拒绝后对端仍可能经 LL 层强推同参数生效。真机 Phase 2
      // 定案：latency=49 生效后 ESP32 central（master）收不到对端任何 notify
      //（连接保持、按键/ATVV 全死）——协商成功且 latency 超阈值时主动修正为 0
      //（itvl 保持对端快参数 12.5ms，组合合法），最多修正 3 次防循环。
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
        ESP_LOGI(TAG, "conn update 完成 itvl=%u latency=%u timeout=%u",
                 desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
        if (event->conn_update.conn_handle == s_conn && desc.conn_latency > 8 &&
            s_latency_fix_count < 3) {
          s_latency_fix_count++;
          // 用连接初始健康组合（24-40/latency 0，07:04 正常时段验证过），不沿用
          // 对端 12.5ms 快参数——ESP32 controller 高频连接间隔下疑似丢事件
          //（真机：latency 修正为 0 但 itvl=10 时小米 notify 依旧全死）。
          static const struct ble_gap_upd_params fix = {
              .itvl_min = 24,  // 30ms
              .itvl_max = 40,  // 50ms
              .latency = 0,    // 修正：高 slave latency 下 ESP32 收不到 notify
              .supervision_timeout = 400,
              .min_ce_len = 0,
              .max_ce_len = 0,
          };
          int rc = ble_gap_update_params(s_conn, &fix);
          ESP_LOGW(TAG, "latency=%u 过高（notify 会死），修正为 30-50ms/lat=0 rc=%d（第 %u 次）",
                   desc.conn_latency, rc, s_latency_fix_count);
        }
      }
      return 0;
    }

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
  s_report_map_handle = 0;
      if (s_on_link != NULL) {
        s_on_link(false);
      }
      schedule_retry();
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      struct os_mbuf *om = event->notify_rx.om;
      if (om == NULL) {
        return 0;
      }
      // 非 HID Report 特征的 notify（ATVV Control/Audio 等）转发给额外消费者
      if (event->notify_rx.attr_handle != s_report_handle) {
        if (s_notify_router != NULL) {
          uint8_t buf[300];
          size_t len = OS_MBUF_PKTLEN(om);
          if (len > sizeof(buf)) {
            len = sizeof(buf);  // ATVV 单包远小于 MTU，防御截断
          }
          ble_hs_mbuf_to_flat(om, buf, len, NULL);
          s_notify_router(event->notify_rx.attr_handle, buf, len);
        }
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

    case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
      // 遥控器请求 latency=49 的省电参数；真机实测该参数生效后恰一个 supervision
      // 窗口(~5s)即被对端掐断（ESP32 central 高 slave-latency 下疑似跳过主发，
      // 遥控器醒来扑空自杀）。压平 latency 保链路存活，功耗优化留 Phase 4。
      // self_params 必须全字段填充（仅改 latency 其余留 0 会被 controller 以
      // HCI 0x212 拒绝，且失败发生在 ATT 事务进行中时链路响应断流——真机
      // Phase 2 排查定案：ATVV 特征枚举恰撞上失败窗口后 GATT 回调全灭）。
      struct ble_gap_upd_params *peer = event->conn_update_req.peer_params;
      struct ble_gap_upd_params *self = event->conn_update_req.self_params;
      self->itvl_min = peer->itvl_min;
      self->itvl_max = peer->itvl_max;
      self->latency = 0;  // 仅压平 latency
      self->supervision_timeout = peer->supervision_timeout;
      self->min_ce_len = peer->min_ce_len;
      self->max_ce_len = peer->max_ce_len;
      ESP_LOGI(TAG, "对端参数请求 itvl=%u latency=%u → 压平 latency（itvl %u-%u 保持）",
               peer->itvl_max, peer->latency, self->itvl_min, self->itvl_max);
      return 0;
    }

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
  if (rc == BLE_HS_EALREADY) {
    // 扫描已在进行（前次尝试遗留），等 disc_event 命中即可，勿再叠加重试
    return;
  }
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
    ESP_LOGW(TAG, "direct connect 启动失败 rc=%d，3s 后重试", rc);
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
  s_report_map_handle = 0;
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
  s_report_map_handle = 0;
  ESP_LOGI(TAG, "小米 central 链路停止");
}

bool gateway_hid_host_connected(void) { return s_report_handle != 0; }

void gateway_hid_host_set_notify_router(gateway_hid_notify_router_t router) {
  s_notify_router = router;
}

uint16_t gateway_hid_host_conn_handle(void) { return s_conn; }
