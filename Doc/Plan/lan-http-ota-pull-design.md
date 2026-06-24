# 局域网 HTTP OTA Pull 设计

> 状态：已确认设计
> 日期：2026-06-25
> 范围：`firmware/components/voice_net/`、`Doc/Ref/protocol.md`；不新增设备端 HTTP server，不改 Windows UI

## 背景

当前 voice_net 已支持：

- BLE 下发 Wi-Fi STA 凭据；
- `wifi_status` 状态上报；
- mDNS / SNTP；
- HTTPS pull OTA；
- OTA rollback + `pending_verify` 健康签到。

现在需要增加 **局域网 HTTP OTA**，目标是在开发/调试阶段由电脑在同一局域网内临时托管 `.bin`，设备通过 Wi-Fi 直接下载，避免 BLE OTA 的慢速传输，同时不引入设备端 Web 上传页面。

## 目标

1. 设备作为 **HTTP client** 从局域网 URL 拉取固件镜像。
2. 继续复用现有 BLE `ota_pull` 控制命令。
3. 不新增设备端 HTTP server，不开放 `/update` 上传入口。
4. `http://` 只允许局域网私有 IPv4，避免公网明文 OTA。
5. `http://` 必须带外部 `sha256_hex`，避免局域网中间人替换镜像。
6. 复用现有 Park gate、`wifi_status.ota_pull` 进度上报、rollback / `pending_verify` 签到逻辑。

## 非目标

- 不做 AP 模式；
- 不做 Captive Portal；
- 不做设备端 HTML 上传页面；
- 不允许公网 HTTP OTA；
- 不在本轮实现 Windows `WifiSettingsDialog` UI。

## 协议设计

继续复用 `control_rx` 中的 `ota_pull` 事件：

```json
{"event":"ota_pull","url":"https://example.com/app.bin"}
{"event":"ota_pull","url":"https://example.com/app.bin","sha256_hex":"64位hex"}
{"event":"ota_pull","url":"http://192.168.3.96:8000/voice_stick.bin","sha256_hex":"64位hex"}
```

### URL 规则

`https://`：

- 继续允许公网或局域网 URL；
- TLS 证书链使用 IDF 证书包校验；
- `sha256_hex` 可选；如果提供，固件额外比对镜像 SHA256。

`http://`：

- host 必须是 IPv4 字面量，不接受域名；
- IPv4 必须属于私有网段：
  - `10.0.0.0/8`
  - `172.16.0.0/12`
  - `192.168.0.0/16`
- 必须提供 `sha256_hex`；
- URL 总长 ≤256。

### 错误码

扩展 `wifi_status.ota_pull.last_error`：

| code | 含义 |
|---|---|
| `ota_url_invalid` | URL 非法、HTTP host 非私有 IPv4、HTTP 缺 sha256 |
| `ota_sha256_invalid` | `sha256_hex` 不是 64 位十六进制 |
| `ota_sha256_mismatch` | 下载镜像 SHA256 与 `sha256_hex` 不一致 |
| `ota_http_failed` | TCP/HTTP 状态/读取失败 |
| `ota_validate_failed` | `esp_ota_write` / `esp_ota_end` / `esp_ota_set_boot_partition` 失败 |
| `ota_park_required` | Park gate 未锁，拒绝启动 OTA |

## 固件架构

### 实现位置

集中修改：

- `firmware/components/voice_net/voice_net_ota.c`
- `firmware/components/voice_net/voice_net_internal.h`
- `firmware/components/voice_net/include/voice_net.h`（如需暴露新错误状态）
- `Doc/Ref/protocol.md`

不修改 `main.c` 的事件名；仍由现有 `ota_pull` 分发到 `voice_net_start_ota_pull()`。

### 下载实现

把当前 `esp_https_ota_*` 高阶 API 改为统一 streaming OTA：

```text
auth / URL / sha256 / Park gate 校验
  ↓
esp_http_client_open(url)
  ↓
esp_http_client_fetch_headers()
  ↓
esp_ota_get_next_update_partition()
  ↓
esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES)
  ↓
循环 esp_http_client_read()
  → esp_ota_write()
  → mbedtls_sha256_update()
  → 每 5% 推 wifi_status
  ↓
esp_ota_end()
  ↓
如提供 sha256_hex：比对 digest
  ↓
esp_ota_set_boot_partition(target)
  ↓
success → wifi_status → 延迟 1s → esp_restart()
```

统一 streaming OTA 的原因：

- HTTPS 与 HTTP 共用一套写分区、进度、错误处理；
- 可以逐 chunk 计算外部 SHA256；
- 避免 `esp_https_ota` 高阶 API 无法直接暴露镜像内容给外部校验；
- 分区控制与现有 BLE OTA 的 `esp_ota_*` 路径一致。

### SHA256 顺序

顺序为：

1. `esp_ota_end()`；
2. 外部 `sha256_hex` 比对；
3. `esp_ota_set_boot_partition()`。

原因：

- `esp_ota_end()` 先校验镜像格式、magic byte、内置 SHA256；
- 外部 SHA256 不匹配时，镜像虽然已写入 next partition，但不会被设为 boot partition；
- 下次 OTA 会覆盖该 next partition，不需要额外擦除。

## 状态上报

继续复用：

```json
"ota_pull": {
  "state": "idle|downloading|finishing|success|failed",
  "progress_pct": 0,
  "url": "http://192.168.3.96:8000/voice_stick.bin",
  "last_error": ""
}
```

进度策略：

- `Content-Length > 0`：`read_bytes * 100 / total`；
- 无 `Content-Length`：下载中保持 0，`esp_ota_end()` 成功后设 100；
- 每变化 5% 推送一次完整 `wifi_status`；
- 最终 `success` 帧发送后延迟 1 秒重启。

## 错误处理

| 阶段 | 处理 |
|---|---|
| URL 校验失败 | 不启动 OTA task，`state=failed` + 对应错误码 |
| Park gate 未锁 | 不启动 OTA task，`ota_park_required` |
| HTTP 打不开 / 状态非 2xx | `state=failed` + `ota_http_failed` |
| 读流中断 | `esp_ota_abort()` + `ota_http_failed` |
| `esp_ota_write()` 失败 | `esp_ota_abort()` + `ota_validate_failed` |
| `esp_ota_end()` 失败 | `ota_validate_failed` |
| SHA256 mismatch | 不 set boot partition，`ota_sha256_mismatch` |
| `esp_ota_set_boot_partition()` 失败 | `ota_validate_failed` |

失败路径不重启设备。成功路径重启进入新槽位。

## rollback / pending_verify

保持现有逻辑：

1. OTA 成功后 `esp_ota_set_boot_partition()`；
2. 延迟 1 秒 `esp_restart()`；
3. 新固件首次启动后 bootloader 标记 `ESP_OTA_IMG_PENDING_VERIFY`；
4. `wifi_status.ota_pending_verify=true`；
5. 固件满足 `uptime ≥10s + BLE 至少连接一次` 后自动 `esp_ota_mark_app_valid_cancel_rollback()`；
6. 若未签到，下次重启自动回滚。

## 验证计划

### 拒绝路径

- `http://example.com/app.bin` + sha256 → `ota_url_invalid`；
- `http://8.8.8.8/app.bin` + sha256 → `ota_url_invalid`；
- `http://192.168.3.96:8000/app.bin` 无 sha256 → `ota_url_invalid`；
- `http://192.168.3.96:8000/app.bin` + 非 64 位 hex → `ota_sha256_invalid`。

### 成功路径

电脑端：

```powershell
cd firmware\build
certutil -hashfile voice_stick.bin SHA256
python -m http.server 8000
```

设备端 BLE 下发：

```json
{
  "event":"ota_pull",
  "url":"http://<电脑局域网IP>:8000/voice_stick.bin",
  "sha256_hex":"certutil输出的64位hex"
}
```

预期：

```text
downloading → finishing → success → reboot
→ ota_pending_verify=true
→ BLE 连上 + 10s
→ mark_app_valid
→ ota_pending_verify=false
```

### mismatch 路径

对同一 URL 使用错误 SHA256：

- 预期 `ota_sha256_mismatch`；
- 不重启；
- 不切 boot partition。

## 风险与缓解

| 风险 | 缓解 |
|---|---|
| 明文 HTTP 被中间人替换 | 仅允许私有 IPv4 + HTTP 强制 SHA256 |
| 用户误填公网 HTTP | URL 校验拒绝 |
| HTTP server 不带 Content-Length | 进度保持 0，结束时 100，不拒绝 |
| OTA task 栈不足 | 继续使用独立 `voice_net_ota` task，栈 ≥8KB |
| BLE notify JSON 太长 | URL 长度上限 256；必要时后续把 `url` 从进度帧中截断显示 |
| rollback 签到失败 | 保持现有自动签到 + 手动 `ota_commit` 双路径 |

## 后续 UI

Windows `WifiSettingsDialog` 后续只需要一个 OTA URL 输入框和可选 SHA256 输入框：

- URL 以 `http://` 开头时，SHA256 输入框必填；
- URL 以 `https://` 开头时，SHA256 可选；
- 点击按钮后仍发送 `ota_pull`。
