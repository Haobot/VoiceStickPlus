# WiFi 按需启停与省电管理 RFC

> 状态：草案 v1
> 日期：2026/06/28
> 范围：`firmware/components/voice_net/`；桌面端无需改动（协议兼容）
> 关联：`Doc/Plan/wifi-sta-ble-provisioning.md`（WiFi 配网基础）、`Doc/Ref/protocol.md`（协议主源）

## 1. 背景与问题

### 1.1 现状

当前 WiFi 生命周期：

```
voice_net_init()                → esp_wifi_init（只 init，不 start）
BLE 连接成功                    → voice_net_resume_if_configured()
  └─ 有 NVS 凭据               → 800ms 延迟 → ensure_wifi_started() → esp_wifi_start() + connect
  └─ 无 NVS 凭据               → WiFi 保持未启动

WiFi 连接成功后                → **永久保持 STA 连接，永不关闭**
```

关键事实：

- `esp_wifi_stop()` — 固件全量代码中**从未被调用**
- `esp_wifi_set_ps()` (Modem 省电) — **从未被调用**
- `esp_wifi_deinit()` — **从未被调用**
- WiFi 断开只发生在：用户手动 `wifi_clear` 凭据，或 AP 端主动踢掉设备

### 1.2 问题

1. **功耗浪费**：WiFi STA 连接态持续消耗 ~80-120mA（ESP32-S3 在 STA connected idle 模式），而 BLE 仅需 ~15-30mA。WiFi 全天候开启使电池续航缩短 3-5 倍。
2. **2.4GHz 同频竞争**：WiFi 和 BLE 共享 2.4GHz 射频，WiFi 空闲信标接收仍会周期抢占时隙，可能导致 BLE 音频传输抖动。
3. **语义不符**：WiFi 在设计文档中被定位为"运维侧路"（见 `Doc/Plan/wifi-sta-ble-provisioning.md` §2），但实际实现中成了常驻链路。

### 1.3 目标

将 WiFi 从"常驻链路"改为"按需启停"：

| 场景 | WiFi 需求 | 典型时长 |
|---|---|---|
| Wi-Fi 扫描 | 需要 | 2-5 秒 |
| OTA 下载 (HTTPS/HTTP) | 需要 | 30-180 秒 |
| Wi-Fi 配网验证 | 需要 | 连接成功后保留 60 秒供用户确认 |
| mDNS 响应 | 需要 | 有查询时临时响应 |
| SNTP 时间同步 | 需要 | 首次拿到 IP 后数秒 |
| **正常使用（录音/待机）** | **不需要** | **99% 的时间** |

## 2. 顶层设计

### 2.1 核心机制：空闲倒计时自动关闭

引入**WiFi 租约 (lease)** 概念。WiFi 打开后进入租约期，每次需要 WiFi 的操作（扫描、OTA、配网）重置租约倒计时；租约到期后自动 `esp_wifi_stop()`。

```
WiFi 打开
  ↓
操作进行中（扫描/OTA/配网验证）  →  重置租约倒计时
  ↓
操作完成，进入空闲
  ↓
租约倒计时中（可配置，默认 60s）  →  如有新操作，重置倒计时
  ↓
倒计时归零  →  esp_wifi_stop() 关闭射频
  ↓
wifi_status.state = "disabled"
```

### 2.2 决策表

| 维度 | 决策 |
|---|---|
| 关闭方式 | `esp_wifi_stop()`（关闭 STA 射频，保留 `esp_wifi_init` 的状态以便下次快速 `start`） |
| 触发关闭的条件 | 租约到期 + 无进行中的 OTA + Park gate 锁定（非录音状态） |
| 租约默认时长 | 60 秒（OTA 下载期间暂不启动倒计时，OTA 完成后单独给 30 秒窗口供 commit） |
| 录音期间行为 | 录音开始 → 若 WiFi 处于 idle（无 OTA 进行中），立即 `esp_wifi_stop()`，不等租约到期 |
| 扫描触发开机 | `wifi_scan` 命令到达时，若 WiFi 未 start → 先 `ensure_wifi_started()` → 等待连接（如果有凭据）或直接扫描（STA 模式无需连接即可扫描）→ 扫描完成后重置租约 |
| OTA 触发开机 | `ota_pull` 命令到达时 → 先 `ensure_wifi_started()` → 连接 → 下载 → 完成后独立 30s 窗口 |
| 配网触发开机 | `wifi_set` → `ensure_wifi_started()` → 连接 → 成功/失败后 60s 租约 |
| 桌面端可见性 | `wifi_status` 新增 `radio_on: bool` 字段，桌面端 UI 可据此显示射频状态 |
| 桌面端兼容 | 新增字段为可选读取，旧版桌面端忽略即可；协议向后兼容 |

### 2.3 范围外（明确不做）

- 不做 WiFi 低功耗监听模式（`WIFI_PS_MAX_MODEM`）——直接关闭更省电
- 不做 DTIM 间隔调优
- 不做 AP 模式下的省电管理
- 不修改桌面端 UI（`radio_on` 字段为协议预留，桌面端本期不读）
- 不做 macOS / Linux 桌面端适配

## 3. 固件改动

### 3.1 组件：`firmware/components/voice_net/voice_net.c`

#### 3.1.1 新增状态与常量

```c
// WiFi 自动关闭空闲倒计时（秒）
#define WIFI_IDLE_TIMEOUT_SEC      60
#define WIFI_OTA_POST_TIMEOUT_SEC  30    // OTA 完成后额外窗口
#define WIFI_PROVISION_TIMEOUT_SEC 60    // 配网验证窗口

// 租约类型：影响倒计时时长
typedef enum {
    LEASE_NONE = 0,       // WiFi 未启动或已关闭
    LEASE_SCAN,           // 扫描完成后，短租约
    LEASE_PROVISION,      // 配网验证期
    LEASE_OTA_POST,       // OTA 下载完成后
    LEASE_DEFAULT,        // 通用
} wifi_lease_reason_t;

static TimerHandle_t      s_idle_timer = NULL;
static wifi_lease_reason_t s_lease_reason = LEASE_NONE;
static atomic_bool        s_wifi_stopping = ATOMIC_VAR_INIT(false);
```

#### 3.1.2 新增内部函数

```c
// 重置/启动空闲倒计时。根据 lease_reason 选择时长。
static void wifi_idle_timer_reset(wifi_lease_reason_t reason);

// 空闲倒计时归零回调（timer cb，投递到 voice_net_task 执行）
static void wifi_idle_timeout_cb(TimerHandle_t timer);

// 执行真正的 esp_wifi_stop：在 voice_net_task 上安全调用
static void do_wifi_stop(void);

// 条件检查：是否可以安全关闭 WiFi
static bool wifi_stop_is_safe(void);
```

`wifi_stop_is_safe()` 实现：

```c
static bool wifi_stop_is_safe(void)
{
    // 有 OTA 正在进行中 → 不安全
    if (voice_net_ota_is_active()) return false;
    // Park gate 未锁定（录音/OTA 中）→ 不安全
    if (s_park_query && !s_park_query()) return false;
    return true;
}
```

#### 3.1.3 修改现有函数

**`ensure_wifi_started()`**：成功后重置空闲倒计时（LEASE_DEFAULT）。

**`do_apply_pending_credentials()`**（配网连接成功后）：在 `GOT_IP` 事件处理中将 lease reason 设为 `LEASE_PROVISION`，重置倒计时为 60s。

**`do_wifi_scan()`**：扫描完成并推送结果后，调用 `wifi_idle_timer_reset(LEASE_SCAN)`。

**`voice_net_clear_credentials()`**：停止空闲倒计时，执行 `do_wifi_stop()`。

**`wifi_event_handler()` → `WIFI_EVENT_STA_DISCONNECTED`**：
- 如果断开原因是 AP 侧踢出（非我们主动 stop），且没有待重试的凭据 → 启动短倒计时（10s），超时后 `do_wifi_stop()`。
- 如果有 NVS 凭据且非 `auth_failed` / `no_ssid`，继续尝试重连（现有行为）。

**`voice_net_task()`**：新增 `VN_CMD_IDLE_TIMEOUT` 和 `VN_CMD_STOP_WIFI` 两个命令处理。

#### 3.1.4 录音联动

新增公开 API：

```c
// 由 main.c 在录音开始时调用。如果 Wi-Fi 当前处于 idle（无 OTA 进行中），
// 立即关掉 Wi-Fi 射频以省电并避免 2.4GHz 干扰。
// 录音结束后不自动恢复 Wi-Fi——下次需要时由操作命令（scan/ota/wifi_set）重新触发。
void voice_net_on_recording_started(void);

// 由 main.c 在录音结束时调用。当前为空操作（Wi-Fi 不会自动重开），
// 但保留 hook 以备未来策略变化。
void voice_net_on_recording_stopped(void);
```

`voice_net_on_recording_started()` 实现：

```c
void voice_net_on_recording_started(void)
{
    if (!atomic_load(&s_inited)) return;
    if (!s_wifi_started) return;
    if (voice_net_ota_is_active()) return;   // OTA 进行中，不能关
    // 停止空闲倒计时，立即关 WiFi
    xTimerStop(s_idle_timer, 0);
    voice_net_cmd_t cmd = VN_CMD_STOP_WIFI;
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100));
}
```

### 3.2 组件：`firmware/components/voice_net/voice_net_ota.c`

#### 3.2.1 修改 OTA 生命周期

**`voice_net_start_ota_pull_internal()`**：
- 入口处先 `ensure_wifi_started()` + 停止空闲倒计时（OTA 期间 WiFi 必须保持）
- OTA 进行中 `s_ota_in_progress = true`

**OTA 完成/失败时**（`VOICE_NET_OTA_STATE_SUCCESS` / `VOICE_NET_OTA_STATE_FAILED`）：
- 设置 `s_ota_in_progress = false`
- 调用新增的内部回调通知 `voice_net.c`：OTA 已结束，启动 30s 后关闭倒计时

在 `voice_net_internal.h` 中新增：

```c
// 由 voice_net_ota.c 在 OTA 结束时调用，通知 voice_net.c 启动关闭倒计时。
void voice_net_notify_ota_ended(void);
```

### 3.3 组件：`firmware/main/main.c`

#### 3.3.1 录音启停处插入钩子

在录音开始处（约 `main.c:507` `s_recording = true` 之后）：

```c
s_recording = true;
voice_net_on_recording_started();  // 新增：关 WiFi 省电 + 防干扰
```

在录音停止处（约 `main.c:523` `s_recording = false` 之后）：

```c
s_recording = false;
voice_net_on_recording_stopped();  // 新增：保留 hook（当前为空操作）
```

#### 3.3.2 BLE 断连行为

BLE 断连时（`APP_EVENT_BLE_DISCONNECTED`）：
- **不主动关 WiFi**。用户可能正在等 OTA 完成（WiFi OTA 不依赖 BLE），租约机制会在 OTA 完成后自动关闭。
- 但如果 WiFi 处于 idle（无 OTA 无 scan），可考虑缩短倒计时到 15s。

### 3.4 协议扩展：`wifi_status` 新增字段

在 `wifi_status` JSON 中新增 `radio_on` 字段：

```json
{
  "event": "wifi_status",
  "state": "disabled",
  "ssid": "",
  "ip": "",
  "rssi": -54,
  "last_error": "",
  "radio_on": false,
  "ota_pull": {...},
  "pending": false,
  "partition": "ota_0",
  "park": true
}
```

| 字段 | 类型 | 语义 |
|---|---|---|
| `radio_on` | bool | WiFi 射频是否已启动（`esp_wifi_start` 已调用且未 `stop`）。`false` 时 `state` 固定为 `"disabled"`。 |

注意：
- `radio_on=false` 时 `rssi` 省略（与 `has_rssi=false` 的现有逻辑一致）
- 旧版桌面端解析不认识 `radio_on` 会忽略，协议向后兼容
- `state="disabled"` + `radio_on=false` 表示彻底关闭；`state="disabled"` + `radio_on=true` 表示射频已开但未连接（过渡态）

### 3.5 状态转换图

```text
                    voice_net_init()
                         │
                    ┌────▼────┐  有 NVS 凭据 + BLE 连上   ┌──────────┐
                    │ radio_on │─────────────────────────►│connecting│
                    │ = false  │   voice_net_resume_       │          │
                    │ disabled │   if_configured()         └────┬─────┘
                    └────▲────┘                               │
                         │                                GOT_IP
                         │                                    │
                    租约到期/                             ┌────▼─────┐
                    录音开始                              │connected │
                         │                               │ lease=   │
                         │                               │PROVISION │
                         │                               └────┬─────┘
                         │                         租约 60s 到期│
                    ┌────┴────┐                               │
                    │ radio_on │◄──────────────────────────────┘
                    │ = false  │
                    │ disabled │
                    └─────────┘

        scan/ota_pull/wifi_set 命令到达时：
        radio_on=false → ensure_wifi_started() → 进入 connecting → ...
```

## 4. 配置项（后续扩展）

本期不引入用户可配参数，所有超时写为常量。后续可通过 `control_rx` 下发覆盖：

```json
{"event":"wifi_power_config","idle_timeout_sec":120}
```

当前不做此命令的解析。常量值见 §3.1.1。

## 5. 实施顺序（严格 TDD）

1. **`voice_net.c` 内部重构**：新增 `s_idle_timer`、`wifi_idle_timer_reset()`、`do_wifi_stop()`、`wifi_stop_is_safe()`，在 `voice_net_task` 中加 `VN_CMD_IDLE_TIMEOUT` / `VN_CMD_STOP_WIFI`。
2. **修改现有 WiFi 生命周期**：`do_apply_pending_credentials` 完成后重置租约、`do_wifi_scan` 完成后重置租约、`wifi_event_handler` 中断连后短倒计时。
3. **OTA 联动**：`voice_net_ota.c` 开始/结束时通知 `voice_net.c` 暂停/重置租约。
4. **录音联动**：新增 `voice_net_on_recording_started/stopped`，在 `main.c` 录音启停处调用。
5. **协议字段**：`build_status_json` 新增 `radio_on` 字段。
6. **桌面端单测**：`core_tests.cc` 中验证 `radio_on` 字段解析不破坏现有 `wifi_status` 解析。
7. **端到端验证**：
   - 配网成功 → 等 60s → 桌面端看到 `radio_on: false` → LCD 不再显示 WiFi 信息
   - 录音期间 WiFi 自动关闭（若之前处于 idle）
   - OTA pull 全流程 → 完成后 30s → WiFi 自动关闭
   - `wifi_scan` 命令触发 WiFi 自动开启 → 扫描完成 → 短倒计时 → 关闭
8. **文档同步**：`Doc/Ref/protocol.md`、`CLAUDE.md`、`AGENTS.md`。

## 6. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 频繁 `esp_wifi_start/stop` 导致连接抖动 | start/stop 间隔由租约保证 ≥60s（扫描场景 ≥10s），避免乒乓效应 |
| `esp_wifi_stop` 后 BLE 短暂受影响 | 与 `esp_wifi_start` 对称——stop 也会触发 RF 状态切换，但实测冲击远小于 start。在 voice_net_task 上执行，与 BLE 任务隔离 |
| 用户在 OTA 完成后 30s 内未点 Commit → WiFi 关闭 → `pending_verify` 横幅无法手动签到 | 不影响：自动签到在 boot 后 10s + BLE 连接时已执行（`voice_net_notify_ble_connected`）。严格模式的手动 `ota_commit` 走 BLE `control_rx`，不需要 WiFi |
| 录音开始关 WiFi 导致正在排队的 scan 失败 | `wifi_stop_is_safe()` 已检查 OTA 进行中；scan 是瞬态操作（2-5s），录音开始会立即中断 scan 并关 WiFi。scan 失败会推送空结果，桌面端已有处理 |
| 桌面端 Wi-Fi 面板在 `radio_on=false` 时显示异常 | `radio_on=false` + `state="disabled"` 的组合语义清晰；旧版桌面端忽略 `radio_on` 只读 `state`，看到 `disabled` 会显示"未配置"——与当前未配网时一致 |
| 用户配网后想看 IP/状态，但 60s 后 WiFi 关了 | 桌面端 Wi-Fi 面板可加"刷新/重连"按钮（本期不做），或用户重新打开 Wi-Fi 设置对话框触发 `wifi_set` |

## 7. 与现有功能的交互矩阵

| 操作 | WiFi 行为 |
|---|---|
| BLE 连接 + 有凭据 | `ensure_wifi_started()` → connect → 60s 租约 |
| `wifi_set` 配新网 | `ensure_wifi_started()` → connect → 60s 租约 |
| `wifi_scan` | `ensure_wifi_started()` → scan → 推送结果 → 10s 租约 |
| `ota_pull` | `ensure_wifi_started()` → connect → download → 完成后 30s 租约 |
| 录音开始 | 若 WiFi idle → 立即 `esp_wifi_stop()` |
| 录音结束 | 不恢复 WiFi（等下次操作触发） |
| BLE 断连 | WiFi 如果有 OTA 进行中继续保持；否则缩短租约到 15s |
| `wifi_clear` | 立即 `esp_wifi_stop()` + 擦 NVS |
| `wifi_status_request` | 纯查询，不重置租约 |
| 设备深度休眠 | `esp_wifi_stop()` 已在租约到期时执行；休眠前确保 WiFi 已关 |
