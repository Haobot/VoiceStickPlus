# Wi-Fi STA 配置 + 桌面端 BLE 配网 RFC

> 状态：草案 v1（2026/06/23）
> 范围：固件 `firmware/` + Windows 桌面端 `desktop/windows/`；macOS、网站、发布脚本本期不动
> 关联：`Doc/Ref/protocol.md`（协议主源）、`CLAUDE.md`/`AGENTS.md`

## 1. 背景与目标

voicestick 目前只有 BLE 单链路，OTA 镜像 ≥3 MB 通过 GATT 写入耗时 ≥10 分钟，且固件未启用 `pending_verify` 健康签到，刷崩需 USB 救砖。

本次升级把硬件目标从 M5Stack StickS3（ESP32-S3FN8，无 PSRAM）切换到 **ESP32-S3-PICO-1 (LGA56) revision v0.2**（8 MB GD Quad Flash + 8 MB AP_3v3 Octal PSRAM），定位等价 N8R8 的 PSRAM 容量但 Flash 仍为 Quad。新增：

1. **Wi-Fi STA 侧路**：BLE 继续做主交互链路，Wi-Fi 用于 OTA pull + mDNS + SNTP。
2. **桌面端 BLE 配网**：Windows 桌面端 → BLE `control_rx` → 固件 NVS → `state_tx` 回传状态；不内嵌 AP/HTTP 服务器。
3. **HTTPS pull OTA**：`esp_https_ota` + IDF 双槽 + `pending_verify` 自动签到；保留现有 BLE OTA 作为兜底。

**安全模型**：BLE 配对即信任，不引入 AUTH / DEV / PIN / Captive Portal。

## 2. 顶层决策

| 维度 | 决策 |
|---|---|
| 目标硬件 | ESP32-S3-PICO-1 (LGA56) rev v0.2，8 MB GD Quad Flash + 8 MB AP_3v3 Octal PSRAM |
| 框架 | 纯 ESP-IDF v5.5.1 |
| 网络模式 | 仅 STA；BLE 与 STA 始终并行 |
| 配置通路 | BLE `control_rx (0x5103)` JSON → NVS → `state_tx (0x5102)` JSON |
| 凭据持久化（固件） | NVS 命名空间 `voicestick`，键 `sta_ssid` / `sta_pass` / `sta_en` / `ota_url` |
| 凭据持久化（桌面端） | `config.toml` `[device.<id>.wifi]`；密码本期不存桌面端，每次重新输入 |
| Park gate | `s_recording \|\| s_ota_updating \|\| voice_ble_ota_is_active() \|\| s_https_ota_in_progress` |
| 双槽 + rollback | 开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + `CONFIG_APP_ROLLBACK_ENABLE=y` |
| 桌面端 UI | 仅 Windows 先做；托盘 → paired device 子菜单 → `Wi-Fi 设置...` → `WifiSettingsDialog` |
| 协议格式 | JSON，与现有 `ui_state`/`interaction_mode` 同风格 |

## 3. BLE 协议扩展

### 3.1 桌面端 → 固件（control_rx，JSON 文本，无头）

| event | 字段 | 语义 |
|---|---|---|
| `wifi_set` | `ssid` (≤32)、`password` (≤63，可为空) | 写 NVS，延迟 800 ms 触发 `esp_wifi_connect`，先让 BLE 回包 |
| `wifi_clear` | — | 擦除 NVS 凭据，断开 STA |
| `wifi_status_request` | — | 立刻推送一帧 `wifi_status` |
| `ota_pull` | `url` (HTTPS, ≤256)、`sha256_hex` (可选) | 启动 `esp_https_ota` task |
| `ota_commit` | — | 调 `esp_ota_mark_app_valid_cancel_rollback` |

字段长度上限固件侧硬校验；超长直接丢弃，下一帧 `wifi_status.last_error = "payload_too_large"`。

### 3.2 固件 → 桌面端（state_tx，4 字节头 `[1, 0x10, len_lo, len_hi]` + JSON）

新增 `event: "wifi_status"`，完整快照（不做差分推送）：

```json
{
  "event": "wifi_status",
  "state": "disabled|configured|connecting|connected|disconnected|error",
  "ssid": "MyHomeWiFi",
  "ip": "192.168.1.42",
  "rssi": -54,
  "last_error": "",
  "ota_pull": {
    "state": "idle|downloading|finishing|success|failed",
    "progress_pct": 0,
    "url": "",
    "last_error": ""
  },
  "ota_pending_verify": false,
  "park_locked": true
}
```

推送时机：状态切换 / 进度每 5% / BLE 连上时主动推一次 / 收到 `wifi_status_request` 时。

### 3.3 错误码

| code | 触发 |
|---|---|
| `""` | 无错 |
| `payload_too_large` | 字段越界 |
| `no_ssid` | `WIFI_REASON_NO_AP_FOUND` |
| `auth_failed` | 4-way handshake 失败 |
| `timeout` | 30 s 未拿到 IP |
| `park_required` | OTA 期间收到 `wifi_set` |
| `ota_url_invalid` | URL 非 HTTPS / 越界 |
| `ota_park_required` | OTA 启动时录音/BLE OTA 进行中 |
| `ota_http_failed` | `esp_https_ota` 错误 |
| `ota_validate_failed` | SHA256 或 `esp_ota_end` 失败 |

**首次写入保留**：`last_error` 非空时后续瞬态事件不能覆盖，直到下次成功或 `wifi_clear` 才清零。

## 4. 固件改动

### 4.1 sdkconfig.defaults 增量

PSRAM Octal 相关项 (`CONFIG_SPIRAM=y` / `CONFIG_SPIRAM_MODE_OCT=y` / `CONFIG_SPIRAM_SPEED_80M=y` / `CONFIG_SPIRAM_USE_MALLOC=y`) **已在现有 `sdkconfig.defaults:22-25` 配置**，2026/06/23 在 COM17 实物板上 erase-flash + flash 后启动验证通过（boot 完整、BLE 接管、无 PANIC）。本期实际新增 **仅 1 行**：

```ini
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
```

**不要**加 `CONFIG_ESPTOOLPY_OCT_FLASH=y`——S3-PICO-1 / N8R8 的 Flash 是 Quad（`CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`），只有 PSRAM 是 Octal。N8R8V 等 Octal Flash 模组才需要这一项。

**rollback 两行 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + `CONFIG_APP_ROLLBACK_ENABLE=y` 延后到 §4.4 一起做**——见下文。

**分区表 `firmware/partitions_ota.csv` 零改动**——现有 3 MB × 2 OTA 槽 + 1984 KB storage 已满足。

### 4.2 新增组件 `firmware/components/voice_net/`

```
firmware/components/voice_net/
├── CMakeLists.txt          # 依赖 esp_wifi / esp_netif / esp_event / esp_https_ota / mdns / mbedtls / nvs_flash
├── include/voice_net.h     # 公开 API
├── voice_net.c             # Wi-Fi 状态机 + OTA pull task
└── voice_net_nvs.c         # 凭据 NVS 读写
```

公开 API：

```c
void voice_net_init(void);
bool voice_net_park_locked(void);
void voice_net_apply_credentials(const char *ssid, const char *password);
void voice_net_clear_credentials(void);
void voice_net_start_ota_pull(const char *url, const char *sha256_hex);
void voice_net_mark_app_valid(void);
voice_net_status_t voice_net_snapshot(void);
void voice_net_register_status_cb(voice_net_status_cb_t cb);
```

启动时序：`esp_netif_init` → `esp_event_loop_create_default`（去重检查）→ `esp_wifi_init`/`WIFI_MODE_STA` → 读 NVS → 有凭据则 `esp_wifi_start` + `esp_wifi_connect` → `mdns_init` hostname `voicestick-<mac4>` → SNTP。

### 4.3 现有组件改动

| 文件 | 改动 |
|---|---|
| `firmware/main/main.c` | `ble_control_cb` JSON 分发追加 `wifi_set` / `wifi_clear` / `wifi_status_request` / `ota_pull` / `ota_commit`（与 `interaction_mode` 同级）；`app_main` 末尾 `voice_net_init`，主循环 10 s 后自动 `voice_net_mark_app_valid` |
| `firmware/components/voice_ble/voice_ble.{c,h}` | 加 `voice_ble_send_wifi_status(const voice_net_status_t *)`；`control_access_cb` 保持不变 |
| `firmware/components/audio_pipeline/audio_pipeline.{c,h}` | 暴露 `audio_pipeline_is_recording()` |
| `firmware/components/ui_status/ui_status.{c,h}` | 加 `ui_status_set_wifi_hint(const char *)` |
| `firmware/main/idf_component.yml` | 加 `espressif/mdns` 依赖 |

### 4.4 OTA `pending_verify` 健康签到（与现有 BLE OTA 联动）

**⚠️ rollback 配置必须与 mark_app_valid 调用同步上线**。`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + `CONFIG_APP_ROLLBACK_ENABLE=y` 开启后，每次 `esp_ota_set_boot_partition` → 重启 → 新固件首启会被标记 `PENDING_VERIFY`，5 分钟（或代码上限）内不调 `esp_ota_mark_app_valid_cancel_rollback()` → 下次重启自动回滚到上一槽。

**voicestick 现有 BLE OTA 路径（`voice_ble.c:282-307`）在 `esp_ota_end` 成功后直接 `esp_restart`，没有 mark_app_valid**。所以开 rollback 必须同时：

1. 在 `firmware/main/main.c::app_main` 末尾（或 `voice_net_init` 内）按"主循环跑过 ≥10 s + BLE 至少有过一次连接"的双条件调 `esp_ota_mark_app_valid_cancel_rollback()`。
2. 在 sdkconfig.defaults 加入：
   ```ini
   CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
   CONFIG_APP_ROLLBACK_ENABLE=y
   ```
3. BLE OTA 与未来 HTTPS pull OTA **共用同一签到逻辑**，由 `voice_net_mark_app_valid()` 暴露给 main.c。
4. 桌面端看到 `ota_pending_verify=true` 红色横幅 → 点 Commit → 下发 `ota_commit`，可作为生产环境严格手动签到的备用通道（与自动签到并行）。
5. 5 分钟超时未签到 → `esp_restart()` 由 bootloader 自动回滚。

**这一步必须在 voice_net 集成同一阶段完成，不可单独提前打开 rollback 配置**——否则当前的 BLE OTA 链路会变成"每次升级都被回滚"。

### 4.5 Park gate

```c
bool voice_net_park_locked(void) {
    return !audio_pipeline_is_recording()
        && !voice_ble_ota_is_active()
        && !s_https_ota_in_progress;
}
```

`voice_net_start_ota_pull` 入口先做 `voice_net_park_locked()` 检查，未锁回 `ota_park_required`。

## 5. Windows 桌面端改动

### 5.1 协议层（`voicestick_core` 库）

| 文件 | 改动 |
|---|---|
| `src/ble_protocol.{h,cc}` | 新增 `WifiStatusSnapshot` 结构；新增 5 个 payload 构造器：`WifiSetPayload` / `WifiClearPayload` / `WifiStatusRequestPayload` / `OtaPullPayload` / `OtaCommitPayload`；`ParseStateEvent` 加 `event=="wifi_status"` 分支，扩展 `StateEvent` 加 `wifi` 子字段 |
| `src/voice_stick_coordinator.{h,cc}` | `BleCentral` 接口加 5 个 `Send*`；`VoiceStickUi` 接口加 `SetDeviceWifiStatus`、`SetWifiOperationResult`；`HandleStateEvent` 分发 `wifi_status` |
| `src/app_config.{h,cc}` | 新增 `[device.<id>.wifi]` 子表读写（仿 `[device.<id>.output]` 模板） |

### 5.2 UI 层（`VoiceStickApp` 可执行）

| 文件 | 改动 |
|---|---|
| `src/wifi_settings_dialog.{h,cc}` | **新文件**。`WifiSettingsDialog::Show(parent, device_id, initial_snapshot, callbacks)`，手动 SetWindowPos 布局，与 `SettingsDialog` 同模板 |
| `src/win32_app.{h,cc}` | 1) 托盘菜单 paired device 子菜单加 `Wi-Fi 设置...` 项；2) `device_wifi_status_map_` 缓存；3) `BleCentral` 接口实现转发到 `BleCentralWin` |
| `src/ble_central_win.{h,cc}` | 5 个 `Send*` 调 `WriteControlPayloadAsync`（仿 `SendUiState`） |
| `src/localization.{h,cc}` | 新增 Wi-Fi 相关 StringId + 中英文表 |
| `CMakeLists.txt` | 加 `wifi_settings_dialog.cc` |

### 5.3 `WifiSettingsDialog` 布局

垂直从上到下：

1. 设备名 label：`设备：VS-1234 (V0.5.0)`
2. **Wi-Fi 凭据组**：SSID Edit / Password Edit（带"显示密码"复选框）/ Apply / Clear
3. **当前状态组**（只读，订阅 `wifi_status` 实时刷新）：state / SSID / IP / RSSI / last_error（4-5 行）
4. **OTA 组**：OTA URL Edit / Check Update 按钮 / 进度条 / OTA last_error
5. **Pending Verify 横幅**（仅当 `ota_pending_verify=true` 显示）：红色背景文字 + Commit 按钮
6. Close 按钮

## 6. 测试策略（TDD）

### 6.1 桌面端（host 跑，无硬件依赖）

新增 Test 函数（`desktop/windows/tests/core_tests.cc`，沿用 `assert` 风格）：

- `TestBleWifiPayloads`：断言 5 个 payload 构造器输出字面量。
- `TestBleWifiStatusParsing`：手搓 `[1, 0x10, len_le16, json]` 帧 → `ParseStateEvent` → 断言所有 `wifi_status` 字段。
- `TestWifiErrorCodeRetention`：模拟 `auth_failed → disconnected` 序列，断言首条错误保留。
- `TestCoordinatorDispatchesWifiStatusToUi`：FakeUi 计数。
- `TestOtaPullRejectedWhenNotParked`：录音中触发 OTA pull 被拦截。
- 加入 `core_tests.cc::main()`。

### 6.2 固件（IDF unity host build）

新增 `firmware/components/voice_net/test/`：

- NVS 凭据 read/write/clear + 边界。
- Park gate 状态机 8 种组合枚举。
- 错误码"首次保留"规则。

### 6.3 端到端手动验收

1. 桌面端连 BLE → 弹 Wi-Fi 设置 → 填 SSID/密码 → Apply → 设备 LCD "Wi-Fi: 已连接 192.168.1.42"；桌面端 dialog 状态行刷新到 `connected`。
2. 故意填错密码 → 桌面端显示 `auth_failed`，首条错误保留。
3. 录音中点 OTA Check Update → 拒绝 + "录音中不可升级"提示。
4. OTA pull 全流程 → 重启 → `ota_pending_verify=true` 红色横幅 → 点 Commit → 横幅消失。
5. 故意刷 `abort()` 固件 → 设备重启后回到旧分区，`ota_pending_verify=false`。
6. `ping voicestick-<mac>.local` 通。

## 7. 实施顺序（严格 TDD）

1. 硬件起步：开 PSRAM/OCT/rollback sdkconfig → 空 sketch 验证 `psram=8388608 flash=8MB`。
2. NVS + 状态结构 + IDF 单测。
3. BLE 协议 round-trip：先写桌面端红灯测试 → 扩 `ble_protocol` 转绿 → 同步扩 `Doc/Ref/protocol.md` → 固件 `voice_ble` 拼装 `wifi_status` JSON。
4. Wi-Fi 主流程：`esp_wifi` 接通，跑通 `connecting → connected → ip`。
5. 错误码 + 重试 + UI 显示。
6. Park gate：`audio_pipeline_is_recording()` + `voice_net_park_locked()` 单测 + 端到端。
7. mDNS + SNTP。
8. `esp_https_ota` 主路径 + 进度上报。
9. `pending_verify` 自动签到 + 故意崩固件验证回滚。
10. `WifiSettingsDialog` UI 接入。
11. 本地化文案 + `LocalizationTablesAreComplete()` 跑过。
12. `Doc/Ref/protocol.md` / `CLAUDE.md` / `AGENTS.md` 同步。
13. Windows 测试通过 → 按 memory 约定签名打包 MSI + 提交 Git。

## 8. 风险与回退

| 风险 | 缓解 |
|---|---|
| Octal PSRAM 开启后 BLE 控制器初始化异常 | 先空 sketch 验证 PSRAM，再分步引入 BLE / Wi-Fi |
| Wi-Fi 与 BLE 2.4 GHz 共存导致音频采集异常 | `esp_wifi_set_max_tx_power` 限制；如有干扰，回退"录音期间暂停 Wi-Fi RX" |
| `pending_verify` 自动签到假阳 → 坏固件标记 valid 失去回滚 | 双条件："主循环 ≥10 s + BLE 至少一次连接成功" |
| BLE WriteWithoutResponse 不带 ack，桌面端不知是否到达 | 桌面端发完 `wifi_set` 启动 35 s 计时器等 `wifi_status` 回包，超时视作失败 |
| 用户填错 SSID 但密码"碰巧"匹配 → 显示 `no_ssid` 用户疑惑 | UI 文案：`未找到该名称的 Wi-Fi 网络（请检查 SSID 拼写）` |

## 9. 范围外（明确不做）

- AP / 内嵌 HTTP 服务器 / Captive Portal / TCP 2323
- DEV 三态 / AUTH 密码层 / PIN 配对屏
- macOS / Linux 桌面端 Wi-Fi UI（本期镜像协议落地后再接）
- 网站端配网（不在场景内，Web Serial 烧录器与本特性独立）
- 远程日志上报（协议留扩展位但不实现）
