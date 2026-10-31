// gatt_explore.c — Phase 0 spike：GATT 数据库探索器实现
//
// 探索链（全部异步回调推进）：
//   disc_all_svcs → 逐服务 disc_all_chrs → 全部完成后按需：
//     HID(0x1812)：read_long Report Map(0x2A4B) → disc_all_dscs 找 CCCD(0x2902) → write 0x0001 使能 notify
//     ATVV(AB5E0001-…)：特征发现阶段顺带核对 TX/Audio/Control 三特征，末尾打印判定行
#include "gatt_explore.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"

static const char *TAG = "gatt";

// ---- 蓝牙 SIG 标准 UUID（16 位） ----
#define UUID_HID_SVC 0x1812
#define UUID_REPORT_MAP 0x2A4B
#define UUID_REPORT 0x2A4D
#define UUID_CCCD 0x2902

// ---- 小米 ATVV 服务（协议事实来自 Doc/Ref/protocol.md ATVV 档案） ----
static const ble_uuid128_t UUID_ATVV_SVC = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x01, 0x00, 0x5E, 0xAB);  // AB5E0001-5A21-4F05-BC7D-AF01F617B664
static const ble_uuid128_t UUID_ATVV_TX = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x02, 0x00, 0x5E, 0xAB);  // AB5E0002 写命令
static const ble_uuid128_t UUID_ATVV_AUDIO = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x03, 0x00, 0x5E, 0xAB);  // AB5E0003 notify
static const ble_uuid128_t UUID_ATVV_CTRL = BLE_UUID128_INIT(
    0x64, 0xB6, 0x17, 0xF6, 0x01, 0xAF, 0x7D, 0xBC,
    0x05, 0x4F, 0x21, 0x5A, 0x04, 0x00, 0x5E, 0xAB);  // AB5E0004 notify

// ---- 探索状态 ----
static uint16_t s_conn;
static uint16_t s_hid_start, s_hid_end;     // HID 服务句柄范围（0 表示未发现）
static uint16_t s_map_handle;               // Report Map 特征值句柄
static uint16_t s_report_handle;            // Report 特征值句柄（对外结果）
static uint16_t s_cccd_handle;              // Report 特征的 CCCD 句柄
static bool s_atvv_seen, s_atvv_tx, s_atvv_audio, s_atvv_ctrl;
static struct ble_gatt_svc s_svcs[24];      // 服务快照（小米全库规模有限）
static int s_svc_count;
static int s_svc_cursor;                    // 当前正在枚举特征的服务下标

static uint8_t s_map_buf[512];              // Report Map 累积缓冲
static size_t s_map_len;

static void explore_next_svc(void);
static void read_report_map(void);
static void find_cccd(void);

static void uuid_str(const ble_uuid_t *uuid, char *out) {
  // NimBLE 两参数版本：调用方保证 out 容量 ≥ BLE_UUID_STR_LEN
  ble_uuid_to_str(uuid, out);
}

// 服务枚举回调：收集快照并打印
static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *svc, void *arg) {
  if (error->status == BLE_HS_EDONE) {
    ESP_LOGI(TAG, "=== 服务枚举完成，共 %d 个，逐服务枚举特征 ===", s_svc_count);
    s_svc_cursor = 0;
    explore_next_svc();
    return 0;
  }
  if (error->status != 0) {
    ESP_LOGE(TAG, "服务枚举失败 rc=%d", error->status);
    return 0;
  }
  if (s_svc_count < (int)(sizeof(s_svcs) / sizeof(s_svcs[0]))) {
    s_svcs[s_svc_count] = *svc;
  }
  char buf[BLE_UUID_STR_LEN];
  uuid_str(&svc->uuid.u, buf);
  ESP_LOGI(TAG, "svc[%d] handle 0x%04x-0x%04x uuid %s", s_svc_count, svc->start_handle,
           svc->end_handle, buf);
  s_svc_count++;
  return 0;
}

// 特征枚举回调：打印 + 记录 HID/ATVV 关键特征句柄
static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
  if (error->status == BLE_HS_EDONE) {
    s_svc_cursor++;
    explore_next_svc();
    return 0;
  }
  if (error->status != 0) {
    ESP_LOGE(TAG, "特征枚举失败 rc=%d", error->status);
    return 0;
  }
  char buf[BLE_UUID_STR_LEN];
  uuid_str(&chr->uuid.u, buf);
  ESP_LOGI(TAG, "  chr handle 0x%04x uuid %s props=0x%02x", chr->val_handle, buf, chr->properties);

  uint16_t u16 = ble_uuid_u16(&chr->uuid.u);
  if (u16 == UUID_REPORT_MAP && chr->val_handle >= s_hid_start && chr->val_handle <= s_hid_end) {
    s_map_handle = chr->val_handle;
  } else if (u16 == UUID_REPORT && chr->val_handle >= s_hid_start && chr->val_handle <= s_hid_end) {
    // 只认带 NOTIFY(0x10) 的 Report 特征（0x2A4D 有轮询形态 props=0x0a 无 CCCD）；
    // 取第一个（真机枚举顺序 0x0064 即 Report ID 1 的键盘 usage 集合通道）
    if ((chr->properties & 0x10) && s_report_handle == 0) {
      s_report_handle = chr->val_handle;
    }
  }
  if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_TX.u) == 0) s_atvv_tx = true;
  if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_AUDIO.u) == 0) s_atvv_audio = true;
  if (ble_uuid_cmp(&chr->uuid.u, &UUID_ATVV_CTRL.u) == 0) s_atvv_ctrl = true;
  return 0;
}

static void explore_next_svc(void) {
  while (s_svc_cursor < s_svc_count) {
    const struct ble_gatt_svc *svc = &s_svcs[s_svc_cursor];
    uint16_t u16 = ble_uuid_u16(&svc->uuid.u);
    if (u16 == UUID_HID_SVC) {
      s_hid_start = svc->start_handle;
      s_hid_end = svc->end_handle;
      ESP_LOGI(TAG, ">>> 命中 HID 服务(0x1812) 范围 0x%04x-0x%04x", s_hid_start, s_hid_end);
    }
    if (ble_uuid_cmp(&svc->uuid.u, &UUID_ATVV_SVC.u) == 0) {
      s_atvv_seen = true;
      ESP_LOGI(TAG, ">>> 命中 ATVV 服务(AB5E0001-…)，核对三特征…");
    }
    int rc = ble_gattc_disc_all_chrs(s_conn, svc->start_handle, svc->end_handle, on_chr, NULL);
    if (rc != 0) {
      ESP_LOGE(TAG, "disc_all_chrs 启动失败 rc=%d，跳过该服务", rc);
      continue;  // 失败则继续下一个服务
    }
    return;  // 等回调推进
  }
  // 全部服务枚举完毕 → 后处理
  ESP_LOGI(TAG, "=== 特征枚举完成 ===");
  if (s_atvv_seen) {
    ESP_LOGI(TAG, "[spike#6] ATVV 特征核对：service=%s TX(AB5E0002)=%s Audio(AB5E0003)=%s "
                  "Control(AB5E0004)=%s",
             s_atvv_seen ? "有" : "无", s_atvv_tx ? "有" : "无", s_atvv_audio ? "有" : "无",
             s_atvv_ctrl ? "有" : "无");
  } else {
    ESP_LOGW(TAG, "[spike#6] ATVV 服务未发现（风险：ReportMap 形态与预期不符）");
  }
  if (s_map_handle) {
    read_report_map();
  } else if (s_report_handle) {
    find_cccd();
  } else {
    ESP_LOGW(TAG, "HID Report/ReportMap 特征均未发现");
  }
}

// Report Map 长读回调：累积并打印（spike 清单第 4 项，hex 供离线比对 13 键 usage 表）
// v5.5.1 的 read_long 用 ble_gatt_attr_fn：数据挂在 attr->om
static int on_read_map(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg) {
  struct os_mbuf *om = attr ? attr->om : NULL;
  if (error->status == 0 || error->status == BLE_HS_EDONE) {
    size_t chunk = om ? OS_MBUF_PKTLEN(om) : 0;
    if (chunk > 0 && s_map_len + chunk <= sizeof(s_map_buf)) {
      ble_hs_mbuf_to_flat(om, s_map_buf + s_map_len, chunk, NULL);
      s_map_len += chunk;
    }
  }
  if (error->status != BLE_HS_EDONE) {
    return 0;  // 继续收 blob 段
  }
  ESP_LOGI(TAG, "[spike#4] Report Map 共 %u 字节：", (unsigned)s_map_len);
  for (size_t i = 0; i < s_map_len; i += 32) {
    char line[3 * 32 + 1];
    int p = 0;
    for (size_t j = i; j < s_map_len && j < i + 32; j++) {
      p += snprintf(line + p, sizeof(line) - p, "%02x ", s_map_buf[j]);
    }
    line[p > 0 ? p - 1 : 0] = '\0';
    ESP_LOGI(TAG, "  +%04u: %s", (unsigned)i, line);
  }
  if (s_report_handle) {
    find_cccd();
  }
  return 0;
}

static void read_report_map(void) {
  int rc = ble_gattc_read_long(s_conn, s_map_handle, 0, on_read_map, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "read_long ReportMap 启动失败 rc=%d", rc);
  }
}

// Report 特征描述符枚举：找 CCCD(0x2902)
static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  if (error->status == BLE_HS_EDONE) {
    if (s_cccd_handle) {
      static uint16_t enable_notify = 0x0001;  // 静态：write_flat 异步引用生命周期
      int rc = ble_gattc_write_flat(s_conn, s_cccd_handle, &enable_notify,
                                    sizeof(enable_notify), NULL, NULL);
      if (rc == 0) {
        ESP_LOGI(TAG, "[spike#5] CCCD(0x%04x) 已写入 0x0001，notify 使能——请按遥控器按键",
                 s_cccd_handle);
      } else {
        ESP_LOGE(TAG, "[spike#5] CCCD 写入失败 rc=%d", rc);
      }
    } else {
      ESP_LOGE(TAG, "[spike#5] Report 特征未找到 CCCD，无法订阅");
    }
    return 0;
  }
  if (error->status != 0) return 0;
  if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD && dsc->handle > s_report_handle &&
      dsc->handle <= s_report_handle + 4) {
    s_cccd_handle = dsc->handle;
  }
  return 0;
}

static void find_cccd(void) {
  ESP_LOGI(TAG, "[spike#5] Report 特征值句柄 0x%04x，查找 CCCD…", s_report_handle);
  int rc = ble_gattc_disc_all_dscs(s_conn, s_report_handle + 1, s_report_handle + 5, on_dsc, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "disc_all_dscs 启动失败 rc=%d", rc);
  }
}

void gatt_explore_start(uint16_t conn_handle) {
  s_conn = conn_handle;
  s_hid_start = s_hid_end = s_map_handle = s_report_handle = s_cccd_handle = 0;
  s_atvv_seen = s_atvv_tx = s_atvv_audio = s_atvv_ctrl = false;
  s_svc_count = 0;
  s_svc_cursor = 0;
  s_map_len = 0;
  int rc = ble_gattc_disc_all_svcs(s_conn, on_svc, NULL);
  if (rc != 0) {
    ESP_LOGE(TAG, "disc_all_svcs 启动失败 rc=%d", rc);
  }
}

uint16_t gatt_explore_report_handle(void) { return s_report_handle; }

void gatt_dump_notify(uint16_t attr_handle, const uint8_t *data, size_t len) {
  char hex[3 * 40 + 1];
  size_t n = len < 40 ? len : 40;
  int p = 0;
  for (size_t i = 0; i < n; i++) {
    p += snprintf(hex + p, sizeof(hex) - p, "%02x ", data[i]);
  }
  hex[p > 0 ? p - 1 : 0] = '\0';
  if (attr_handle == s_report_handle) {
    ESP_LOGI(TAG, "[spike#5] Report notify (len=%u): %s", (unsigned)len, hex);
    // 9 字节报文 = 01 00 00 前缀 + 3×LE16 当前按下 usage（集合语义，沿由消费端 diff）
    if (len >= 3) {
      size_t pairs = (len - 3) / 2 < 3 ? (len - 3) / 2 : 3;
      for (size_t i = 0; i < pairs; i++) {
        uint16_t usage = data[3 + 2 * i] | (data[4 + 2 * i] << 8);
        if (usage != 0) {
          ESP_LOGI(TAG, "    按下集合 usage[%u] = 0x%04x", (unsigned)i, usage);
        }
      }
    }
  } else {
    ESP_LOGI(TAG, "notify attr 0x%04x (len=%u): %s", attr_handle, (unsigned)len, hex);
  }
}
