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

// HOGP 主机侧两个必需动作（都不是可选优化，缺一即按键零推送）：
//  - CCCD 订阅：不开 Report notify 通道，遥控器除语音键（走 ATVV 帧）以外的
//    所有按键都不会上行。真机定案：置 0 时方向键/音量键/电源键全死，
//    语音键仍能触发（它由 ATVV Control 帧驱动，与 HID 通道无关），
//    现象即"只有语音键有反应、其他键全没反应"。
//  - Exit Suspend：HOGP 规范要求主机写 HID Control Point(0x2A4C)=0x00，
//    让重连后处于 suspend 态的 HID 设备恢复输入报告推送。
// 历史上"三件套致小米停推"的假设已被证伪——真因是 sec_cb 每次连接无条件
// delete_peer 重配对把遥控器配对状态机搞坏（见方案文档 §6.3 问题 6）。
// 保留独立开关仅用于必要时二分定位。
#define XIAOMI_HID_CCCD_SUBSCRIBE 1
#define XIAOMI_HID_EXIT_SUSPEND 1

#define UUID_HID_SVC 0x1812
#define UUID_REPORT 0x2A4D
#define UUID_HID_CTRL_POINT 0x2A4C  // HID Control Point：写 0x00 = Exit Suspend
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
// direct connect 连续失败计数（改绑自愈：3 次后清 peer 转扫描重新配对）
static uint8_t s_direct_fail_streak;
// 本连接是否已触发 HID 发现（bond 恢复与新建配对两条加密路径的幂等保护）
static bool s_hid_explored;
// 本连接清旧键重配的次数：用现存 bond 起不来时最多清一次（见 wipe_peer_and_retry）
static uint8_t s_sec_wipe_count;
// 小米 HID 服务内有 25 个 Report 特征（真机枚举），其中 7 个带 notify。按键流
// 落在不同 Report 特征上（每个 Report ID 一个特征），只认第一个会漏掉大部分按键
// ——真机定案：语音键 usage 走 0x0064，其余按键零推送（报文落到别的 Report 特征
// 后被当成 ATVV 包丢弃）。故枚举阶段收集全部带 notify 的 Report 句柄，逐个写
// CCCD 订阅；落入任一句柄的报文都按 HID 按键报文解析。
#define MAX_REPORT_HANDLES 8
static uint16_t s_report_handles[MAX_REPORT_HANDLES];
static uint8_t s_report_handle_count;
// CCCD 订阅顺序推进：一次只挂一个写事务，避免与小米 ~4.9s L2CAP 参数请求
// 0x212 窗口撞车时叠压（真机 Phase 2 定案——该窗口会吞掉在途 ATT 事务）。
// s_cccd_index = 当前推进到的特征下标（失败重试到上限即跳过前进，不卡整条链）；
// s_cccd_confirmed = 实际订阅成功的特征数（诊断用）。
static uint8_t s_cccd_index;
static uint8_t s_cccd_confirmed;
static uint8_t s_cccd_retry_count;
static bool s_dsc_found;  // 本次描述符枚举是否命中 0x2902
static struct ble_npl_callout s_cccd_retry_callout;
static bool s_cccd_retry_callout_inited;
static bool s_running;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_report_handle;  // 小米侧 Report 特征值句柄（notify 源）
static uint16_t s_hid_ctrl_point_handle;  // HID Control Point(0x2A4C) 句柄（唤醒推送）
static uint8_t s_own_addr_type;
static gateway_report_parser_t s_parser;
static struct ble_npl_callout s_retry_callout;
static bool s_retry_callout_inited;
static struct ble_npl_callout s_sec_callout;
static bool s_sec_callout_inited;

static void start_scan(void);
static void try_connect_known(void);

// 写 HID Control Point = 0x00 (Exit Suspend)：HOGP 规范要求 HID 设备重连后
// 处于 suspend 态，主机必须写该特征才恢复输入报告推送。此前从未写过。
// 直写即可，不再绕道 Report Map 长读（内容本就丢弃，只多一个可能被 ~4.9s
// L2CAP 参数窗口吞掉的 ATT 事务）。写失败不致命：CCCD 订阅才是按键通道的硬前提。
static void write_hid_exit_suspend(void) {
#if XIAOMI_HID_EXIT_SUSPEND
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_hid_ctrl_point_handle == 0) {
    ESP_LOGW(TAG, "HID Control Point 不可用，跳过 Exit Suspend");
    return;
  }
  static const uint8_t exit_suspend = 0x00;  // 静态生命周期：write_flat 异步引用
  int rc = ble_gattc_write_flat(s_conn, s_hid_ctrl_point_handle, &exit_suspend, 1,
                                NULL, NULL);
  ESP_LOGI(TAG, "写 HID Control Point(0x%04x)=Exit Suspend rc=%d",
           s_hid_ctrl_point_handle, rc);
#endif
}

#define CCCD_RETRY_MAX 2

static void report_cccd_step(void);

// 推进到下一个 Report 特征；链尾写 Exit Suspend（HOGP 的唤醒动作放在全部订阅
// 之后，避免与 CCCD 写在 ~4.9s L2CAP 参数窗口里叠压）。
static void cccd_advance(void) {
  s_cccd_index++;
  s_cccd_retry_count = 0;
  ble_npl_callout_stop(&s_cccd_retry_callout);
  if (s_cccd_index >= s_report_handle_count) {
    ESP_LOGI(TAG, "Report CCCD 订阅完成：成功 %u/%u（按键推送通道开通）",
             (unsigned)s_cccd_confirmed, (unsigned)s_report_handle_count);
    write_hid_exit_suspend();
    return;
  }
  report_cccd_step();
}

// Report CCCD 订阅写完成回调：成功即推进下一个；失败交给 retry callout
// （重试到上限就跳过该特征继续——单个句柄被拒不能卡死整条链，真机实证
// 0x0077 返回 ATT 0x03 Write Not Permitted）。
static int on_cccd_write_done(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (error->status != 0) {
    ESP_LOGW(TAG, "Report CCCD(0x%04x) 写失败 status=%d",
             (uint16_t)(s_report_handles[s_cccd_index] + 1), error->status);
    return 0;
  }
  s_cccd_confirmed++;
  cccd_advance();
  return 0;
}

// 落入任一带 notify 的 Report 特征句柄即按键报文
static bool is_report_handle(uint16_t handle) {
  for (uint8_t i = 0; i < s_report_handle_count; i++) {
    if (s_report_handles[i] == handle) {
      return true;
    }
  }
  return false;
}

// 订阅第 s_cccd_index 个 Report 特征：先枚举描述符找真正的 0x2902（与
// xiaomi_atvv_client 同一手法，ATVV 侧实测该枚举可用），再用 GATT 布局惯例
// 的特征值句柄 +1 兜底（spike 实证 0x0065；小米 HID 侧描述符枚举时有不响应）。
static int on_report_dsc(uint16_t conn, const struct ble_gatt_error *error,
                         uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                         void *arg) {
  (void)chr_val_handle; (void)arg;
  if (error->status == BLE_HS_EDONE) {
    if (!s_dsc_found) {
      uint16_t fallback = (uint16_t)(s_report_handles[s_cccd_index] + 1);
      ESP_LOGW(TAG, "Report 0x%04x 未枚举到 CCCD，回退直写 0x%04x",
               s_report_handles[s_cccd_index], fallback);
      static const uint16_t enable_notify = 0x0001;  // 静态：write_flat 异步引用
      int rc = ble_gattc_write_flat(conn, fallback, &enable_notify,
                                    sizeof(enable_notify), on_cccd_write_done, NULL);
      ESP_LOGI(TAG, "Report CCCD(0x%04x) 直写 0x0001 rc=%d（%u/%u）", fallback, rc,
               (unsigned)(s_cccd_index + 1), (unsigned)s_report_handle_count);
    }
    return 0;
  }
  if (error->status != 0 || dsc == NULL) {
    return 0;
  }
  if (ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
    s_dsc_found = true;
    static const uint16_t enable_notify = 0x0001;  // 静态：write_flat 异步引用
    int rc = ble_gattc_write_flat(conn, dsc->handle, &enable_notify,
                                  sizeof(enable_notify), on_cccd_write_done, NULL);
    ESP_LOGI(TAG, "Report 0x%04x 的 CCCD(0x%04x) 写 0x0001 rc=%d（%u/%u）",
             s_report_handles[s_cccd_index], dsc->handle, rc,
             (unsigned)(s_cccd_index + 1), (unsigned)s_report_handle_count);
  }
  return 0;
}

static void report_cccd_step(void) {
  if (s_cccd_index >= s_report_handle_count) {
    cccd_advance();
    return;
  }
  s_dsc_found = false;
  uint16_t val = s_report_handles[s_cccd_index];
  int rc = ble_gattc_disc_all_dscs(s_conn, (uint16_t)(val + 1), (uint16_t)(val + 5),
                                   on_report_dsc, NULL);
  if (rc != 0) {
    // 枚举起不来（或有在途 GATT 过程）：直接退回直写
    ESP_LOGW(TAG, "Report 0x%04x 描述符枚举启动失败 rc=%d，退回直写", val, rc);
    static const uint16_t enable_notify = 0x0001;  // 静态：write_flat 异步引用
    int wrc = ble_gattc_write_flat(s_conn, (uint16_t)(val + 1), &enable_notify,
                                   sizeof(enable_notify), on_cccd_write_done, NULL);
    ESP_LOGI(TAG, "Report CCCD(0x%04x) 直写 0x0001 rc=%d（%u/%u）",
             (uint16_t)(val + 1), wrc, (unsigned)(s_cccd_index + 1),
             (unsigned)s_report_handle_count);
  }
  ble_npl_callout_reset(&s_cccd_retry_callout, pdMS_TO_TICKS(3000));
}

static void cccd_retry_cb(struct ble_npl_event *ev) {
  (void)ev;
  if (!s_running || s_conn == BLE_HS_CONN_HANDLE_NONE || s_report_handle_count == 0) {
    return;
  }
  if (s_cccd_index >= s_report_handle_count) {
    return;  // 链已跑完，等确认
  }
  if (s_cccd_retry_count >= CCCD_RETRY_MAX) {
    ESP_LOGW(TAG, "Report 0x%04x 订阅连续 %u 次未成功，跳过继续下一个",
             s_report_handles[s_cccd_index], s_cccd_retry_count);
    cccd_advance();
    return;
  }
  s_cccd_retry_count++;
  ESP_LOGW(TAG, "Report 0x%04x 订阅未确认，第 %u 次重试（%u/%u）",
           s_report_handles[s_cccd_index], s_cccd_retry_count,
           (unsigned)(s_cccd_index + 1), (unsigned)s_report_handle_count);
  report_cccd_step();
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
    if (s_report_handle_count == 0) {
      ESP_LOGW(TAG, "HID 服务内无带 notify 的 Report 特征");
      return 0;
    }
    ESP_LOGI(TAG, "小米 HID 服务内带 notify 的 Report 特征共 %u 个",
             (unsigned)s_report_handle_count);
    // 订阅：逐个 Report 特征枚举描述符找 0x2902（见 report_cccd_step）；写是
    // ATT 事务，可能被 ~4.9s 参数窗口吞——retry callout 兜底重试/跳过。
    // 不订阅则遥控器除语音键（走 ATVV 帧）以外的按键全部不上行。
#if XIAOMI_HID_CCCD_SUBSCRIBE
    if (!s_cccd_retry_callout_inited) {
      ble_npl_callout_init(&s_cccd_retry_callout, nimble_port_get_dflt_eventq(),
                           cccd_retry_cb, NULL);
      s_cccd_retry_callout_inited = true;
    }
    s_cccd_index = 0;
    s_cccd_confirmed = 0;
    s_cccd_retry_count = 0;
    report_cccd_step();
    ble_npl_callout_reset(&s_cccd_retry_callout, pdMS_TO_TICKS(3000));
#else
    ESP_LOGI(TAG, "CCCD 订阅已关闭（对照实验）");
    write_hid_exit_suspend();
#endif
    return 0;
  }
  if (error->status != 0) {
    return 0;
  }
  if (ble_uuid_u16(&chr->uuid.u) == UUID_REPORT &&
      (chr->properties & CHR_PROP_NOTIFY) &&
      chr->val_handle >= range->start && chr->val_handle <= range->end) {
    // 收集全部带 notify 的 Report 特征（不只是第一个）：按键流分散在多个
    // Report ID 上，漏订/漏解析哪一个就丢哪一组按键。
    if (s_report_handle_count < MAX_REPORT_HANDLES) {
      s_report_handles[s_report_handle_count++] = chr->val_handle;
    }
    if (s_report_handle == 0) {
      s_report_handle = chr->val_handle;  // 首个：链路就绪信号与 connected 判定
      ESP_LOGI(TAG, "锁定小米 Report 特征 0x%04x，链路就绪", s_report_handle);
      if (s_on_link != NULL) {
        s_on_link(true);
      }
    }
  } else if (ble_uuid_u16(&chr->uuid.u) == UUID_HID_CTRL_POINT &&
             chr->val_handle >= range->start && chr->val_handle <= range->end) {
    s_hid_ctrl_point_handle = chr->val_handle;
  }
  return 0;
}

static void explore_hid(uint16_t conn) {
  s_report_handle = 0;
  s_report_handle_count = 0;
  s_hid_ctrl_point_handle = 0;
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

// 链路加密就绪后的统一入口：落盘对端地址 + 重置按键解析器 + 启动 HID 发现。
// 幂等保护：bond 恢复时加密在连接建立阶段即完成，ENC_CHANGE 可能早于 sec_cb
// 触发，两条路径都会走到这里；重复探索会打乱 ATT 事务并叠加重试。
static void on_link_secured(uint16_t conn) {
  if (s_hid_explored) {
    return;
  }
  s_hid_explored = true;
  s_sec_wipe_count = 0;
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(conn, &desc) == 0) {
    save_peer(desc.peer_id_addr.val, desc.peer_id_addr.type);
  }
  gateway_report_parser_reset(&s_parser);
  explore_hid(conn);
}

// 用现存 bond 起不来时的兜底：清掉本机存的旧 LTK 再重配（每次连接最多一次）。
// 不无条件清键——那会把每次重连都变成一次全新配对，遥控器 NVS 被反复写键、
// 配对状态机搞坏（真机问题 6 的元凶：所有按键零推送，重配对后才恢复）。
static void wipe_peer_and_retry(const char *why, int rc) {
  struct ble_gap_conn_desc desc;
  if (s_conn == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(s_conn, &desc) != 0) {
    return;
  }
  s_sec_wipe_count++;
  ESP_LOGW(TAG, "%s rc=%d，清旧键重配对（第 %u 次）", why, rc, s_sec_wipe_count);
  (void)ble_store_util_delete_peer(&desc.peer_id_addr);
  ble_npl_callout_reset(&s_sec_callout, pdMS_TO_TICKS(500));
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
  // bond 正常时链路已由 controller 用现存 LTK 加密，不需要也不应该重配。
  if (desc.sec_state.encrypted) {
    on_link_secured(s_conn);
    return;
  }
  int rc = ble_gap_security_initiate(s_conn);
  if (rc == 0 || rc == BLE_HS_EALREADY) {
    // EALREADY：controller 已用现存 bond 完成加密，而 ENC_CHANGE 早于本回调
    // 触发过了——补一次发现，否则会一直空等到超时。
    if (ble_gap_conn_find(s_conn, &desc) == 0 && desc.sec_state.encrypted) {
      on_link_secured(s_conn);
    }
    return;
  }
  if (s_sec_wipe_count == 0) {
    wipe_peer_and_retry("security_initiate 失败", rc);
    return;
  }
  // 清键后仍失败：悬挂连接会占满连接表（MAX_CONNECTIONS=3 含桌面端一条），
  // 后续 connect 全部 ENOMEM 死循环（真机验收实测教训），断开重试
  ESP_LOGW(TAG, "security_initiate 再次失败 rc=%d，断开后重试", rc);
  ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) {
        // direct connect 建立失败也计入改绑自愈 streak（对端不在此地址时
        // 每次 15s 超时，3 次后清 peer 转扫描）
        if (++s_direct_fail_streak >= 3) {
          s_direct_fail_streak = 0;
          nvs_handle_t h;
          if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, NVS_KEY_PEER);
            (void)nvs_commit(h);
            nvs_close(h);
          }
          ESP_LOGW(TAG, "direct connect 连续失败 status=%d，清 peer 转扫描（对端可能已改绑）",
                   event->connect.status);
          s_conn = BLE_HS_CONN_HANDLE_NONE;
          start_scan();
          return 0;
        }
        ESP_LOGW(TAG, "连接小米失败 status=%d，3s 后重试", event->connect.status);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        schedule_retry();
        return 0;
      }
      s_conn = event->connect.conn_handle;
      s_latency_fix_count = 0;
      s_hid_explored = false;
      s_sec_wipe_count = 0;
      s_report_handle_count = 0;
      s_cccd_index = 0;
      s_cccd_confirmed = 0;
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
        // 现存 bond 用不起来（对端删了键/换了 LTK）清一次键重配；清过之后仍失败
        // 属「遥控器未进配对模式」，静默等待即可（Phase 0 结论）。
        if (event->enc_change.conn_handle == s_conn && s_sec_wipe_count == 0) {
          wipe_peer_and_retry("加密失败", event->enc_change.status);
          return 0;
        }
        ESP_LOGI(TAG, "加密失败 status=%d（遥控器可能未进配对模式），静默等待",
                 event->enc_change.status);
        return 0;
      }
      on_link_secured(event->enc_change.conn_handle);
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      if (event->disconnect.conn.conn_handle != s_conn) {
        return 0;
      }
      ESP_LOGW(TAG, "小米断开 reason=%d，3s 后重试", event->disconnect.reason);
      s_conn = BLE_HS_CONN_HANDLE_NONE;
      s_report_handle = 0;
  s_report_handle_count = 0;
      s_hid_ctrl_point_handle = 0;
      s_hid_explored = false;
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
      // 非 HID Report 特征的 notify（ATVV Control/Audio 等）转发给额外消费者。
      // 注意判定必须用「全部 Report 句柄」集合：只比对 s_report_handle（首个）
      // 会把落在其余 Report 特征上的按键报文误当 ATVV 包丢弃——真机定案：
      // 语音键 usage 在 0x0064，音量/方向等其他按键在别的 Report 特征上。
      if (!is_report_handle(event->notify_rx.attr_handle)) {
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
  // 对端改绑自愈：遥控器与其他主机重新配对后地址形态改变（配对模式广播用
  // 小米 OUI 地址），NVS 身份地址 direct connect 不再可达而死循环重试。
  // 连续失败达 3 次即清 peer 转扫描，重新走配对抓取（白名单含配对模式名）。
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
    if (++s_direct_fail_streak >= 3) {
      s_direct_fail_streak = 0;
      nvs_handle_t h;
      if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_erase_key(h, NVS_KEY_PEER);
        (void)nvs_commit(h);
        nvs_close(h);
      }
      ESP_LOGW(TAG, "direct connect 连续失败，清 peer 转扫描（对端可能已改绑）");
      start_scan();
      return;
    }
    ESP_LOGW(TAG, "direct connect 启动失败 rc=%d，3s 后重试", rc);
    schedule_retry();
  } else {
    s_direct_fail_streak = 0;
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
  s_report_handle_count = 0;
  s_hid_ctrl_point_handle = 0;
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
  s_report_handle_count = 0;
  s_hid_ctrl_point_handle = 0;
  ESP_LOGI(TAG, "小米 central 链路停止");
}

bool gateway_hid_host_connected(void) { return s_report_handle != 0; }

void gateway_hid_host_set_notify_router(gateway_hid_notify_router_t router) {
  s_notify_router = router;
}

uint16_t gateway_hid_host_conn_handle(void) { return s_conn; }
