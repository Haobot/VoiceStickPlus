// main.c — 小米遥控器 StickS3 网关 Phase 0 spike 主程序
//
// 验证目标（Doc/Plan/xiaomi-remote-stick-gateway.md §6，逐项带 [spike#N] 日志标记）：
//   #1 小米进配对模式后被 ESP32 扫描发现（名称白名单 / ATVV UUID 双通道过滤）
//   #2 连接 + Just Works 配对 + bond 持久化（NVS），观察小米对 NoInputNoOutput 主机的接受度
//   #3 断电重启后 ESP32 主动 direct connect 回连成功（对端身份地址持久化）
//   #4/#5/#6 由 gatt_explore 完成（Report Map / 三键 notify / ATVV 三特征）
//
// 本文件为一次性验证程序：状态机刻意线性，不做生产级健壮性（退避简单、无并发保护扩展）。
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "gatt_explore.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

// 官方 blecent 例程同款前置声明（store/config 头文件未导出该符号）
void ble_store_config_init(void);

static const char *TAG = "spike";

#define NVS_NS "gw_poc"
#define NVS_KEY_PEER "peer"
#define RETRY_DELAY_US (3 * 1000 * 1000)  // 断开后 3s 重试回连

static uint8_t s_own_addr_type;
static bool s_peer_known;                   // NVS 中已有对端地址
static uint8_t s_peer_addr[6];              // 对端身份地址
static uint8_t s_peer_addr_type;            // 对端地址类型
static esp_timer_handle_t s_retry_timer;

// 名称白名单（trim+小写子串匹配；来源：桌面端 pair_device_dialog 实测白名单）
// "u-rfrc"：2026-09-16 spike 取证发现——正常态广播名为 U-RFRC478（5C:24:1F 小米 OUI +
// Flags 0x06 纯 BLE 可连接 + Service Data 0xFF01），配对模式名才是 MI RC 等白名单形态
static const char *kNameWhitelist[] = {
    "mi rc", "xiaomi bluetooth remote 2 pro", "小米蓝牙语音遥控器", "rc001", "rc003", "u-rfrc",
};
static const ble_uuid128_t kAtvvSvcUuid = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x01, 0x00, 0x5E, 0xAB);

static void start_scan(void);
static void try_connect_known(void);

static void to_lower_str(char *s) {
  for (; *s; s++) {
    if (*s >= 'A' && *s <= 'Z') *s += 32;
  }
}

static bool name_matches(const uint8_t *name, uint8_t name_len) {
  char buf[64];
  if (name_len == 0 || name_len >= sizeof(buf)) return false;
  memcpy(buf, name, name_len);
  buf[name_len] = '\0';
  to_lower_str(buf);
  for (size_t i = 0; i < sizeof(kNameWhitelist) / sizeof(kNameWhitelist[0]); i++) {
    if (strstr(buf, kNameWhitelist[i]) != NULL) return true;
  }
  return false;
}

static bool adv_has_atvv_uuid(const struct ble_hs_adv_fields *fields) {
  if (fields->num_uuids128 == 0) return false;
  for (int i = 0; i < fields->num_uuids128; i++) {
    if (ble_uuid_cmp(&fields->uuids128[i].u, &kAtvvSvcUuid.u) == 0) return true;
  }
  return false;
}

static void save_peer(const uint8_t *addr, uint8_t type) {
  memcpy(s_peer_addr, addr, 6);
  s_peer_addr_type = type;
  s_peer_known = true;
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    uint8_t blob[7] = {type, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]};
    nvs_set_blob(h, NVS_KEY_PEER, blob, sizeof(blob));
    nvs_commit(h);
    nvs_close(h);
  }
}

static bool load_peer(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  uint8_t blob[7];
  size_t len = sizeof(blob);
  bool ok = nvs_get_blob(h, NVS_KEY_PEER, blob, &len) == ESP_OK && len == 7;
  nvs_close(h);
  if (ok) {
    s_peer_addr_type = blob[0];
    memcpy(s_peer_addr, blob + 1, 6);
    s_peer_known = true;
  }
  return ok;
}

static void log_addr(const char *prefix, const uint8_t *addr, uint8_t type) {
  // 地址类型只有 public(0)/random(1) 两值；RPA 属 random 的一种
  ESP_LOGI(TAG, "%s %02x:%02x:%02x:%02x:%02x:%02x (type=%s)", prefix, addr[5], addr[4], addr[3],
           addr[2], addr[1], addr[0], type == BLE_ADDR_PUBLIC ? "public" : "random");
}

static void connect_or_scan_after_retry(void *arg) {
  if (s_peer_known) {
    ESP_LOGI(TAG, "[spike#3] 重试：direct connect 已知对端…");
    try_connect_known();
  } else {
    start_scan();
  }
}

static void schedule_retry(void) {
  ESP_LOGI(TAG, "%lus 后重试", (unsigned long)(RETRY_DELAY_US / 1000000));
  esp_timer_stop(s_retry_timer);
  esp_timer_start_once(s_retry_timer, RETRY_DELAY_US);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) {
        ESP_LOGE(TAG, "[spike#2] 连接失败 status=%d", event->connect.status);
        schedule_retry();
        return 0;
      }
      ESP_LOGI(TAG, "[spike#2] 已连接 conn_handle=%d（连接间隔协商见后续 CONN_UPDATE）",
               event->connect.conn_handle);
      // 先交换 MTU（Report Map 可能超默认 23），再发起配对/加密
      ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
      int rc = ble_gap_security_initiate(event->connect.conn_handle);
      if (rc != 0) {
        // 已 bond 时直接恢复加密；错误码即配对兼容性观察点
        ESP_LOGE(TAG, "[spike#2] security_initiate 失败 rc=%d（配对兼容性风险信号）", rc);
      }
      return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
      int rc2 = event->enc_change.status;
      if (rc2 == 0) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
          // 持久化对端身份地址（bond 回连依据，spike#3）
          save_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);
          log_addr("[spike#2] 配对/加密成功，对端身份地址已存 NVS：", desc.peer_id_addr.val,
                   desc.peer_id_addr.type);
        }
        gatt_explore_start(event->enc_change.conn_handle);
      } else {
        // 失败码即 spike 风险 1（小米不接受 ESP32 配对）的直接证据
        ESP_LOGE(TAG, "[spike#2] 配对/加密失败 status=%d —— 对照 BLE_HS_HCI_ERR 判因", rc2);
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGW(TAG, "断开 conn=%d reason=%d", event->disconnect.conn.conn_handle,
               event->disconnect.reason);
      schedule_retry();
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      // 订阅特征的 notify 统一入口（HID Report / ATVV Control 等都从这里来）
      struct os_mbuf *om = event->notify_rx.om;
      size_t len = om ? OS_MBUF_PKTLEN(om) : 0;
      if (len > 0 && len <= 64) {
        uint8_t buf[64];
        ble_hs_mbuf_to_flat(om, buf, len, NULL);
        gatt_dump_notify(event->notify_rx.attr_handle, buf, len);
      }
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // 反复测试场景：对端密钥与本地 bond 不一致时，删旧 bond 重配，避免卡死
      ESP_LOGW(TAG, "REPEAT_PAIRING：删除旧 bond 重新配对");
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
      ESP_LOGI(TAG, "连接参数更新 conn=%d status=%d", event->conn_update.conn_handle,
               event->conn_update.status);
      return 0;

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(TAG, "MTU 协商完成 conn=%d mtu=%u", event->mtu.conn_handle, event->mtu.value);
      return 0;

    default:
      return 0;
  }
}

// 旁路设备取证打印去重：同一地址只打一次（缓存满则视为已打印）
#define BYPASS_CACHE 12
static uint8_t s_bypass_seen[BYPASS_CACHE][6];
static int s_bypass_next;

static bool bypass_seen_once(const uint8_t *addr) {
  for (int i = 0; i < BYPASS_CACHE; i++) {
    if (memcmp(s_bypass_seen[i], addr, 6) == 0) return true;
  }
  memcpy(s_bypass_seen[s_bypass_next], addr, 6);
  s_bypass_next = (s_bypass_next + 1) % BYPASS_CACHE;
  return false;
}

static int disc_event(struct ble_gap_event *event, void *arg) {  if (event->type != BLE_GAP_EVENT_DISC) return 0;
  struct ble_hs_adv_fields fields;
  int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
  if (rc != 0) return 0;

  bool hit = name_matches(fields.name, fields.name_len) || adv_has_atvv_uuid(&fields);
  if (!hit && fields.name_len == 0) return 0;  // 未带名称的广告再等 scan rsp

  char name[64] = {0};
  if (fields.name_len > 0 && fields.name_len < sizeof(name)) {
    memcpy(name, fields.name, fields.name_len);
  }
  if (!hit) {
    // 有名字但白名单不匹配：按地址去重做一次全量取证打印（名字/地址/RSSI/厂商数据/原始广播）
    // —— u-rfrc478 疑似小米广播名形态，靠完整字段判定而非瞎扩白名单
    if (!bypass_seen_once(event->disc.addr.val)) {
      char addr_buf[24];
      snprintf(addr_buf, sizeof(addr_buf), "%02x:%02x:%02x:%02x:%02x:%02x",
               event->disc.addr.val[5], event->disc.addr.val[4], event->disc.addr.val[3],
               event->disc.addr.val[2], event->disc.addr.val[1], event->disc.addr.val[0]);
      char mfg_hex[3 * 16 + 1] = "";
      if (fields.mfg_data && fields.mfg_data_len > 0 && fields.mfg_data_len <= 16) {
        int p = 0;
        for (int i = 0; i < fields.mfg_data_len; i++) {
          p += snprintf(mfg_hex + p, sizeof(mfg_hex) - p, "%02x", fields.mfg_data[i]);
        }
      }
      char adv_hex[3 * 31 + 1] = "";
      int p2 = 0;
      for (int i = 0; i < event->disc.length_data && i < 31; i++) {
        p2 += snprintf(adv_hex + p2, sizeof(adv_hex) - p2, "%02x", event->disc.data[i]);
      }
      ESP_LOGI(TAG, "旁路(首见) name=\"%s\" addr=%s type=%d rssi=%d mfg=%s %s",
               name, addr_buf, event->disc.addr.type, event->disc.rssi, mfg_hex,
               event->disc.length_data > 0 ? "(adv)" : "(scan-rsp)");
      ESP_LOGI(TAG, "  raw[%u]: %s", (unsigned)event->disc.length_data, adv_hex);
    }
    return 0;
  }

  ESP_LOGI(TAG, "[spike#1] 命中小米白名单：name=\"%s\" addr_type=%d rssi=%d", name,
           event->disc.addr.type, event->disc.rssi);
  log_addr("  对端地址：", event->disc.addr.val, event->disc.addr.type);

  ble_gap_disc_cancel();
  static struct ble_gap_conn_params params = {
      .scan_itvl = 0x0010, .scan_window = 0x0010,
      .itvl_min = 24, .itvl_max = 40,   // 30~50ms（HID 常规档）
      .latency = 0, .supervision_timeout = 400,  // 4s
      .min_ce_len = 0, .max_ce_len = 0,
  };
  rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 10000, &params, gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_connect 启动失败 rc=%d", rc);
    schedule_retry();
  }
  return 0;
}

static void start_scan(void) {
  struct ble_gap_disc_params params = {
      .passive = 0,            // 主动扫描以收取 scan rsp 中的完整名称
      .itvl = 0, .window = 0,
      .filter_duplicates = 1,
  };
  int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, disc_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_disc 启动失败 rc=%d", rc);
  } else {
    ESP_LOGI(TAG, "[spike#1] 开始扫描（小米请进入配对模式：长按背部配对键/组合键）…");
  }
}

static void try_connect_known(void) {
  ble_addr_t addr;
  addr.type = s_peer_addr_type;
  memcpy(addr.val, s_peer_addr, 6);
  static struct ble_gap_conn_params params = {
      .scan_itvl = 0x0010, .scan_window = 0x0010,
      .itvl_min = 24, .itvl_max = 40,
      .latency = 0, .supervision_timeout = 400,
      .min_ce_len = 0, .max_ce_len = 0,
  };
  int rc = ble_gap_connect(s_own_addr_type, &addr, 15000, &params, gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "direct connect 启动失败 rc=%d", rc);
    schedule_retry();
  }
}

static void on_sync(void) {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(TAG, "设备地址获取失败 rc=%d", rc);
    return;
  }
  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(TAG, "地址类型推断失败 rc=%d", rc);
    return;
  }
  if (s_peer_known) {
    log_addr("[spike#3] NVS 有已配对对端，直接回连：", s_peer_addr, s_peer_addr_type);
    try_connect_known();
  } else {
    start_scan();
  }
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE host 重置 reason=%d", reason); }

static void host_task(void *param) {
  nimble_port_run();  // 阻塞直至 nimble_port_stop
  nimble_port_freertos_deinit();
}

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  s_peer_known = load_peer();

  const esp_timer_create_args_t timer_args = {
      .name = "retry", .callback = connect_or_scan_after_retry,
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

  ESP_ERROR_CHECK(nimble_port_init());
  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.reset_cb = on_reset;
  // NoInputNoOutput → Just Works；小米是否接受即为 spike 风险 1 观察点
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;   // 绑定标志（bond 落 NVS）
  ble_hs_cfg.sm_mitm = 0;      // Just Works 不提供 MITM
  ble_hs_cfg.sm_sc = 1;        // LE Secure Connections
  ble_hs_cfg.sm_our_key_dist = 0x0F;   // 分发 enc/id/sign/master 信息（尽量完整）
  ble_hs_cfg.sm_their_key_dist = 0x0F;
  ble_store_config_init();

  nimble_port_freertos_init(host_task);

  ESP_LOGI(TAG, "=== 网关 Phase 0 spike 启动（清单见 Doc/Plan/xiaomi-remote-stick-gateway.md §6）===");
}
