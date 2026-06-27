# Voice Stick Protocol

This document describes the protocol implemented by the current firmware and macOS desktop app.

## Goals

- Low-latency push-to-talk audio from StickS3 to macOS.
- Opus over BLE to keep wireless bandwidth low.
- Ogg Opus forwarding from macOS to either Volcengine ASR or the VoiceStick Cloud relay.
- Final ASR text insertion into the focused macOS input field after release and confirmation.

## BLE GATT

Device name: `VS-XXXX`, where `XXXX` is derived from the last two bytes of the device eFuse MAC.

Service UUID:

```text
8f2f0b84-6e6f-4b23-88f7-3a3ceafc5100
```

Characteristics:

| Name | UUID | Direction | Properties |
| --- | --- | --- | --- |
| `audio_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5101` | StickS3 -> Mac | notify |
| `state_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5102` | StickS3 -> Mac | notify |
| `control_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5103` | Mac -> StickS3 | write without response |
| `ota_rx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5104` | Mac -> StickS3 | write, write without response |
| `ota_tx` | `8f2f0b84-6e6f-4b23-88f7-3a3ceafc5105` | StickS3 -> Mac | notify |

The desktop app scans for this service and only connects to devices whose `VS-XXXX` ID is present in the local paired-device list. Multiple paired devices may be connected at the same time; audio, state, control, and OTA handling are scoped by CoreBluetooth peripheral identity.

## Audio Frame

All multibyte fields are little-endian.

```text
struct AudioBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x01 audio
  uint16_t header_len;    // 16
  uint32_t session_id;
  uint32_t seq;
  uint8_t  flags;         // bit0=start, bit1=end
  uint8_t  reserved;      // currently 0
  uint16_t payload_len;
  uint8_t  payload[payload_len];
}
```

The payload contains one raw Opus packet when `payload_len > 0`. The firmware currently encodes 60 ms of 16 kHz mono audio per packet. When recording stops, the firmware also sends an end frame with `flags & 0x02` and an empty payload.

The macOS app wraps incoming Opus packets into an Ogg Opus stream before sending them to ASR. It does not decode Opus to PCM.

## State Event

All multibyte fields are little-endian.

```text
struct StateBleFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x10 state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

State events report device facts from the firmware to the app. They do not carry
business actions such as "cancel" or "confirm"; the app owns that interpretation.

Currently emitted state events:

```json
{"event":"device_info","hardware":"stick_s3","firmware_version":"0.2.2","buttons":["primary","secondary"],"interaction_modes":["hold_to_talk","click_to_talk"],"ui_states":["ready","recording","thinking","pending_confirmation","error"]}
{"event":"button_down","button":"primary","session_id":1234}
{"event":"button_up","button":"primary","duration_ms":620,"session_id":1234}
{"event":"button_down","button":"secondary"}
{"event":"button_up","button":"secondary","duration_ms":90}
{"event":"wifi_status","state":"connected","ssid":"MyHomeWiFi","ip":"192.168.1.42","rssi":-54,"last_error":"","ota_pull":{"state":"idle","progress_pct":0,"last_error":""},"pending":false,"partition":"ota_0","park":true}
```

Buttons are named by role instead of physical placement. On StickS3, the front
button maps to `primary` and the side button maps to `secondary`. `session_id` is
included when a `primary` press starts or stops a local audio recording.

Deprecated firmware-to-app events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `press_start` | `button_down` with `button:"primary"` | The old name assumed the front button and implied recording semantics. |
| `press_end` | `button_up` with `button:"primary"` | The old name implied recording semantics and did not include a button role. |
| `cancel` | `button_down` / `button_up` with `button:"secondary"` | The old event encoded app meaning; the same button can cancel, restore, or be ignored depending on app state. |

## Control Event

The desktop app writes compact JSON to `control_rx`. Control events are authoritative UI
state from the app to the firmware display.

Current desktop events:

```json
{"event":"ui_state","state":"ready","text":""}
{"event":"ui_state","state":"recording","text":""}
{"event":"ui_state","state":"thinking","text":"partial text"}
{"event":"ui_state","state":"pending_confirmation","text":"final text"}
{"event":"ui_state","state":"error","text":"ASR timeout"}
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"click_to_talk"}
{"event":"show_imu_debug","enabled":true}
{"event":"show_wifi_info","enabled":true}
{"event":"imu_wake_sensitivity","threshold":500}
```

| Event | Field | Direction | Meaning |
| --- | --- | --- | --- |
| `ui_state` | `state`: string, `text`: string | Mac -> StickS3 | Authoritative display state from the app to the firmware display. |
| `interaction_mode` | `mode`: string | Mac -> StickS3 | Controls the front-button behavior and idle screen hint. |
| `show_imu_debug` | `enabled`: boolean | Windows -> StickS3 | Toggles the on-screen IMU acceleration debug overlay. Default false. |
| `show_wifi_info` | `enabled`: boolean | Windows -> StickS3 | Toggles on-screen display of the connected Wi-Fi SSID and IP below the IMU debug line. Default false. |
| `imu_wake_sensitivity` | `threshold`: integer (LSB) | Windows -> StickS3 | Sets the pick-up/shake-to-wake sensitivity threshold. Recommended range 50–2000 LSB; lower values are more sensitive. Default 800 LSB. |

For `ui_state`, the desktop helper always includes a `text` field; older firmware
can ignore it. Firmware may immediately render local physical feedback, such as
showing the recording cat when the primary button starts audio, but the app's
`ui_state` is the authoritative display state. Current StickS3 firmware does not
render recognition text on-device because the LVGL font set does not include
Chinese glyphs; `text` is used only to choose fixed English hints.

`interaction_mode` controls the front-button behavior and idle screen hint.
`hold_to_talk` starts audio on primary down and stops on primary up.
`click_to_talk` starts audio on the first primary click and stops on the next
primary click.

Deprecated app-to-firmware events:

| Event | Replacement | Reason |
| --- | --- | --- |
| `connected` | `ui_state:ready` | Connection is not a display state after pairing. |
| `partial` | `ui_state:thinking` with `text` | Partial text is display content for the thinking state. |
| `final` | `ui_state:pending_confirmation` with `text` | Final text is still cancellable until pasted. |
| `paste_done` | `ui_state:ready` | Once pasted, the device returns to ready. |
| `paste_cancelled` | `ui_state:ready` | Once cancelled, the device returns to ready. |
| `error` | `ui_state:error` with `text` | Errors are another UI state. |

## Wi-Fi Provisioning and HTTPS OTA Pull

设备通过 BLE `control_rx` 接收 Wi-Fi STA 凭据，连接成功后通过 `state_tx`
（type=0x10）上报 `wifi_status` 快照。Wi-Fi 仅作为运维侧路使用：HTTPS 拉取
OTA、mDNS 发现、SNTP 时间同步。BLE 仍是主交互链路，二者并行共存。详细背景与
风险见 `Doc/Plan/wifi-sta-ble-provisioning.md`。

### 桌面端 → 固件（control_rx，JSON 文本）

| event | 字段 | 语义 |
| --- | --- | --- |
| `wifi_set` | `ssid` (≤32 ASCII)、`password` (≤63，可空) | 写 NVS，延迟 800 ms 后 `esp_wifi_connect`；先让 BLE 回包 |
| `wifi_clear` | — | 擦除 NVS 凭据，断开 STA |
| `wifi_status_request` | — | 立刻补推一帧 `wifi_status` |
| `wifi_scan` | — | 启动周围 2.4GHz Wi-Fi 扫描，结果通过 `wifi_scan_result` 异步回报 |
| `ota_pull` | `url` (HTTPS 或局域网 HTTP, ≤256)、`sha256_hex` (HTTP 必填，HTTPS 可选) | 启动固件主动 OTA pull；HTTP 仅允许私有 IPv4 |
| `ota_commit` | — | 调 `esp_ota_mark_app_valid_cancel_rollback`，确认新固件健康 |

示例：

```json
{"event":"wifi_set","ssid":"MyHomeWiFi","password":"p@ss\"w0rd"}
{"event":"wifi_set","ssid":"OpenAP","password":""}
{"event":"wifi_clear"}
{"event":"wifi_status_request"}
{"event":"wifi_scan"}
{"event":"ota_pull","url":"https://oss.example.com/voicestick/0.4.0.bin","sha256_hex":"deadbeef..."}
{"event":"ota_pull","url":"https://oss.example.com/voicestick/0.4.0.bin"}
{"event":"ota_pull","url":"http://192.168.3.96:8000/voice_stick.bin","sha256_hex":"64位hex"}
{"event":"ota_commit"}
```

字段长度由固件硬校验；超长直接丢弃，下一帧 `wifi_status.last_error =
"payload_too_large"`。所有写日志路径必须把 `password` 字段脱敏为 `<redacted>`。

`ota_pull.url` 规则：

- `https://`：证书链由 IDF 证书包校验，`sha256_hex` 可选；若提供，固件额外比对镜像 SHA256。
- `http://`：只允许私有 IPv4 字面量（`10.0.0.0/8`、`172.16.0.0/12`、`192.168.0.0/16`），不接受域名或公网 IP，且必须提供 64 位十六进制 `sha256_hex`。
- HTTP 明文路径仅用于局域网调试；完整性由 `sha256_hex` 保证。

### 固件 → 桌面端（state_tx，event=`wifi_status`）

完整快照，**不做差分推送**——桌面端解析时直接覆盖本地缓存：

```json
{
  "event": "wifi_status",
  "state": "disabled|configured|connecting|connected|disconnected|error",
  "ssid": "MyHomeWiFi",
  "ip": "192.168.1.42",
  "rssi": -54,
  "last_error": "",
  "radio_on": false,
  "ota_pull": {
    "state": "idle|downloading|finishing|success|failed",
    "progress_pct": 0,
    "last_error": ""
  },
  "pending": false,
  "partition": "ota_0",
  "park": true
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `radio_on` | bool | Wi-Fi 射频是否已启动（`esp_wifi_start` 已调用且未 `stop`）。`false` 时 `state` 固定为 `"disabled"`，`rssi` 省略。旧版桌面端忽略此字段。 |

> **按需启停**：Wi-Fi 射频在空闲倒计时（默认 60 s）归零后自动关闭，或在录音开始时立即关闭。
> 下次 `wifi_set` / `wifi_scan` / `ota_pull` 命令到达时自动重新启动。
> 详见 `Doc/Plan/wifi-on-demand-power-management.md`。

> **MTU 注意**：`ota_pull` 中不再携带 `url` 字段。完整 OTA URL 可能很长，加上
> Wi-Fi 字段后容易超过 BLE MTU（Windows 协商后约 244 字节），导致桌面端解析失败、
> UI 不更新。URL 由发起 `ota_pull` 的桌面端自行保存，设备侧只回报状态、进度和错误码。

推送时机：状态切换 / OTA 进度每 5% / BLE 重连接后主动一次 / 收到 `wifi_status_request` 时。

### 固件 → 桌面端（state_tx，event=`wifi_scan_result`）

固件收到 `wifi_scan` 后，扫描周围 2.4GHz Wi-Fi AP，按 RSSI 降序返回最多 15 个非空 SSID：

```json
{
  "event": "wifi_scan_result",
  "aps": [
    {"ssid": "MyWiFi", "rssi": -45, "auth": 3},
    {"ssid": "NeighborWiFi", "rssi": -70, "auth": 0}
  ]
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `aps` | array | AP 列表，按 RSSI 降序，最多 15 个 |
| `aps[].ssid` | string | AP 名称（空 SSID 的 AP 被过滤） |
| `aps[].rssi` | int | 信号强度 (dBm)，负值 |
| `aps[].auth` | int | 认证模式：0=OPEN, 1=WEP, 2=WPA_PSK, 3=WPA2_PSK, 4=WPA_WPA2_PSK, 5=WPA2_ENTERPRISE, 6=WPA3_PSK, 7=WPA2_WPA3_PSK |

过滤规则：
- 仅返回 2.4 GHz 频段（channel 1-14）的 AP
- 跳过空 SSID（隐藏网络）
- RSSI 降序排列，最多 15 个
- 扫描失败或无结果时返回 `{"event":"wifi_scan_result","aps":[]}`

扫描通过 `esp_wifi_scan_start` 实现，跑在专用 `voice_net_task` 上（6 KB 栈），约 2-5 秒完成。

### 错误码 `last_error` 枚举

| code | 触发条件 |
| --- | --- |
| `""` | 无错 |
| `payload_too_large` | SSID / 密码 / URL 字段越界 |
| `no_ssid` | `WIFI_REASON_NO_AP_FOUND` |
| `auth_failed` | 4-way handshake 失败 |
| `timeout` | 30 s 内未拿到 IP |
| `park_required` | OTA 期间收到 `wifi_set`，拒绝以免中断升级 |
| `ota_url_invalid` | URL 非法、HTTP host 非私有 IPv4、HTTP 缺少 `sha256_hex` 或 URL 长度越界 |
| `ota_sha256_invalid` | `sha256_hex` 不是 64 位十六进制 |
| `ota_sha256_mismatch` | 下载镜像 SHA256 与 `sha256_hex` 不一致 |
| `ota_park_required` | OTA 启动时录音或 BLE OTA 进行中 |
| `ota_http_failed` | TCP/HTTP 状态/读取错误（含 HTTP 状态非 2xx、TLS 失败等） |
| `ota_validate_failed` | `esp_ota_write` / `esp_ota_end` / `esp_ota_set_boot_partition` 失败 |

**首次写入保留**：`last_error` 非空时后续瞬态事件不能覆盖，直到下次成功事件或显式 `wifi_clear` 才清零。

### Park gate（业务侧锁）

`park=true` 当且仅当：录音空闲 + 当前未在 BLE OTA + 当前未在 HTTPS OTA。OTA pull 启动前固件侧硬校验 `park`，未锁立刻回 `ota_park_required`。

### OTA `pending_verify` 健康签到

启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + `CONFIG_APP_ROLLBACK_ENABLE=y` 后：

- 新固件首次启动 bootloader 标记 `ESP_OTA_IMG_PENDING_VERIFY`，固件侧暴露在 `wifi_status.pending=true`。
- 默认行为：固件在 "主循环跑过 ≥10 s + BLE 至少一次连接成功" 后自动 `esp_ota_mark_app_valid_cancel_rollback()`。
- 严格模式（可选）：桌面端 UI 看到 `pending=true` 红色横幅 → 用户点 Commit → 下发 `ota_commit` → 固件签到。
- 超时未签到（5 分钟） → 设备重启 → bootloader 自动回滚到上一槽。

**注意**：rollback 配置必须与所有 OTA 路径的 mark_valid 调用同时上线（包括现有 BLE OTA），否则会出现"升级后每次重启都被回滚"的死锁。本期上线计划见
`Doc/Plan/wifi-sta-ble-provisioning.md` §4.4。

## BLE OTA

The firmware uses a custom OTA channel over the same Voice Stick service. The macOS app writes OTA `begin` and `end` frames with BLE write-with-response, and streams OTA `data` frames with write-without-response using CoreBluetooth flow control.
The device sends progress notifications roughly every 32 KB of accepted firmware data.

The macOS app starts OTA for one connected device at a time. It discovers updates from the latest firmware manifest, downloads the manifest `ota_url`, verifies byte size and SHA-256, then sends the verified app-slot image over BLE. The browser flasher uses the manifest `merged_url` instead because USB flashing writes a merged image at offset `0x0`.

The 8 MB flash layout uses two 3 MB OTA app slots and keeps the remaining flash as a reserved SPIFFS data partition:

| Name | Offset | Size |
| --- | ---: | ---: |
| `ota_0` | `0x10000` | 3 MB |
| `ota_1` | `0x310000` | 3 MB |
| `storage` | `0x610000` | 1984 KB |

All multibyte fields are little-endian.

```text
struct OtaBeginFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x20 begin
  uint16_t header_len;    // 12
  uint32_t image_size;
  uint32_t transfer_id;
}

struct OtaDataFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x21 data
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t offset;
  uint8_t  payload[];
}

struct OtaEndFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x22 end
  uint16_t header_len;    // 12
  uint32_t transfer_id;
  uint32_t image_size;
}

struct OtaAbortFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x23 abort
  uint16_t header_len;    // 8
  uint32_t transfer_id;
}
```

`ota_tx` sends a state frame:

```text
struct OtaStateFrame {
  uint8_t  version;       // 1
  uint8_t  type;          // 0x30 OTA state
  uint16_t payload_len;
  uint8_t  json[payload_len];
}
```

OTA state events include:

```json
{"event":"ready","transfer_id":1,"size":1385760,"partition":"ota_1"}
{"event":"progress","transfer_id":1,"written":32768,"size":1385760}
{"event":"done","transfer_id":1,"reboot_ms":500}
{"event":"error","code":"bad_offset","esp_err":258}
{"event":"aborted"}
```

On the device display, OTA switches the normal idle/recording UI into an update state:

- `Updating` with percentage while the image is being written.
- `Rebooting` after the new boot partition is selected.

While OTA is active, the device ignores push-to-talk input and pauses display dimming/deep sleep timers. After a successful transfer, the firmware waits about 500 ms after sending the `done` event and then calls `esp_restart()`.
The desktop updater can cancel an in-progress transfer by sending `OtaAbortFrame`; the device aborts the OTA handle and keeps booting the current firmware.

## Runtime State Machine

StickS3:

```text
boot -> advertising -> connected -> idle -> recording -> idle
```

The firmware also dims the display after 30 seconds of idle time. On battery power it enters deep sleep after 5 minutes; while charging or USB powered it stays at the dimmed-screen stage. The front button wakes the device from deep sleep.

macOS:

```text
needs_pairing -> scanning -> ready -> recording -> thinking -> pending_confirmation -> ready
```

During recognition and confirmation, the firmware keeps showing the thinking cat
until the app sends `ui_state:ready`. During pending confirmation, `primary`
confirms or pauses according to the app's internal countdown mode, and
`secondary` cancels. When idle, `secondary` restores the last recoverable input
confirmation. These meanings are app state-machine behavior, not firmware
protocol events.

Recordings shorter than 0.5 seconds are discarded locally and are not sent to ASR.

## ASR Transport

The desktop app can connect either directly to Volcengine or to VoiceStick Cloud. Both providers use the same WebSocket binary framing in the client, so request, audio, response, and error handling are shared.

Volcengine endpoint:

```text
wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async
```

VoiceStick Cloud default endpoint:

```text
wss://api.xiaozhi.me/voicestick/asr/
```

The first request payload currently sent by the desktop app is:

```json
{
  "user": {"uid": "voice-stick-local"},
  "audio": {
    "format": "ogg",
    "codec": "opus",
    "rate": 16000,
    "bits": 16,
    "channel": 1
  },
  "request": {
    "model_name": "bigmodel",
    "enable_nonstream": true,
    "show_utterances": false,
    "enable_ddc": true
  }
}
```

The desktop app buffers Ogg chunks until the recording reaches 0.5 seconds, then starts ASR and flushes the buffered chunks. On button release, it sends the final Ogg chunk with the WebSocket last-packet flag and waits for the final response.

VoiceStick Cloud business errors should use the same error frame shape as Volcengine: message type `0x0f`, a four-byte big-endian error code, a four-byte big-endian message size, and a UTF-8 message. For quota or billing errors, the message should be JSON so the desktop app can surface an upgrade action:

```json
{
  "error": "quota_exceeded",
  "message": "Daily free quota has been used up.",
  "upgrade_url": "https://voicestick.app/account/billing"
}
```

See `docs/volcengine-asr.md` for the trimmed Volcengine API notes used by the desktop app.
