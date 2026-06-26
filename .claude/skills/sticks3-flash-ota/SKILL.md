---
name: sticks3-flash-ota
description: >-
  给 M5Stack StickS3 / ESP32-S3 固件烧录与升级的标准流程。当用户说“烧固件 / 刷固件 / 刷机 /
  下载固件 / 烧录固件 / 给 stick 刷进去 / 串口烧录 / 串口下载 / OTA 升级 / 推送固件 /
  flash / 把改好的固件刷到设备 / 更新设备固件”等，或在改完 firmware/ 后需要把固件装到设备上验证时使用。
  涵盖两条路径：默认优先的 BLE 触发 HTTP OTA（用 VoiceStickCtl，免拔线免按键），
  以及回退的串口烧录（idf_cli.py，需人工按前面板按键进 Boot/复位）。
---

# StickS3 固件烧录与 OTA

把改好的固件装到 M5Stack StickS3（ESP32-S3）设备上的标准操作手册。两条路径，**默认优先 HTTP OTA**。

## 决策：先选路径

```
固件已构建通过？
├─ 设备已配对 + Wi-Fi 已连 + 不是分区表变更 + 桌面端可用
│     → ✅ 路径 A：HTTP OTA（默认，免拔线免按键，最快）
└─ 未配对 / Wi-Fi 未连 / 改了分区表 / OTA 失败不可恢复 / 首次烧录新板
      → 路径 B：串口烧录（需人工按键进 Boot）
```

只有在 A 不可用或失败不可恢复时才回退 B。详见根记忆 `firmware-http-ota-default`。

---

## 路径 A：BLE 触发 HTTP OTA（默认优先）

设备通过已连接的 BLE 会话收到指令后，自己用 Wi-Fi 从局域网 HTTP server 拉取并刷写固件。**不用拔线、不用按键。**

### 步骤

1. **构建固件**（产物 `firmware/build/voice_stick.bin`）：
   ```sh
   python scripts/idf_cli.py -c
   ```

2. **计算 SHA256**（HTTP OTA 必须提供，否则 VoiceStickCtl 报错）：
   ```powershell
   certutil -hashfile firmware\build\voice_stick.bin SHA256
   ```
   取输出中间那行 64 位十六进制（去掉空格）。

3. **起本地 HTTP server** 指向 `firmware/build`（另开一个终端，别占住当前会话）：
   ```powershell
   cd firmware\build; python -m http.server 8000
   ```
   用本机局域网 IP（如 `192.168.3.96`，用 `ipconfig` 查），不要用 `localhost` —— URL 要让设备能访问到。

4. **触发 OTA**（设备 id 是屏幕上 `VS-XXXX` 的后四位，如 `5D74`）：
   ```powershell
   desktop\windows\build-x64\VoiceStickCtl.exe ota-pull `
     --device 5D74 `
     --url http://192.168.3.96:8000/voice_stick.bin `
     --sha256 <上一步的SHA256> `
     --wait healthy --timeout 240
   ```
   - 看到 `done ok=true`（退出码 0）= 成功并健康回连。
   - `VoiceStickCtl` 若没找到正在运行的 `VoiceStick.exe`，会自动拉起主程序复用其 BLE 会话。

### VoiceStickCtl 参数速查（源：`desktop/windows/src/ota_command.cc`）

| 参数 | 说明 |
|---|---|
| `ota-pull` | 唯一子命令，必须是第一个参数 |
| `--device <XXXX>` | 目标设备 id；只有一个配对设备时可省略 |
| `--url <http(s)://...>` | 固件 URL；http 必须配 `--sha256` |
| `--sha256 <64hex>` | 固件 SHA256；http 强制要求，https 可选 |
| `--wait success\|healthy` | 默认 `healthy`（等设备刷完重启回连并稳定） |
| `--timeout <秒>` | 默认 180，HTTP OTA 建议 240 |
| `--save-config` | 把本次 url/sha256 存进设备 Wi-Fi profile，下次可省 |
| `--json` | 结构化输出 |

退出码：`0` 成功；`124` 超时；`4` Wi-Fi/park 类错误；`5` 其它 OTA 错误；`2` 参数/配置错误；`3` IPC 错误。

### A 失败时的诊断顺序

`error code=` 后缀指明原因：

- `wifi_*` → 设备 Wi-Fi 没连上。先用桌面端/`VoiceStickCtl` 配网，或确认 STA 凭据已下发。
- `park_required` / `ota_park_required` → 设备正在录音或有其它 OTA 在跑。等空闲再试。
- `timeout` → server 不可达（IP/防火墙/端口）、设备拉取慢。检查 URL 能否从设备所在网段访问。

排查不动、或属于“不可恢复 / 改了分区表”才回退路径 B。

---

## 路径 B：串口烧录（回退）

⚠️ **这块板的 Boot/复位/电源由前面板按钮电路接管，esptool 的自动复位无效，必须人工按键。** 详见根记忆 `stick-s3-button-boot-control`。

### 一键编译+烧录+监控

```sh
python scripts/idf_cli.py -cus
```

`idf_cli.py` 会自动探测 ESP-IDF 环境、自动选串口（评分制，可 `-p COM17` 指定）。烧录 921600 / 监控 115200。单步：`-c` 编译、`-u` 烧录、`-s` 监控；`--list-ports` 列串口。

### 必须的人工按键时序（不能跳过）

1. **烧录前**：提示用户「请**长按**前面板按钮进入 Boot/下载模式」，等用户确认后再执行 `-u`。否则 esptool 识别不到芯片。
2. **烧录成功后**：提示用户「烧录完成，请**短按**前面板按钮重启」，等确认后再 `-s` 监控。否则芯片留在下载模式，串口能开但无数据（`waiting for download`）。
3. 看到「复位后串口完全无数据 / 读到 0 字节」时，**先怀疑是否漏了人工按键**，不要去调脚本。

### 分区表变更：首次需擦除重刷

设备从旧单应用分区表升级到当前 OTA 分区表时，普通 flash 不够，需先擦除（`idf_cli.py` 无 erase 选项，用原生 idf.py）：

```sh
idf.py -p COMxx erase-flash flash monitor
```

同样要先长按进 Boot。

---

## 速查：关键陷阱

| 现象 | 原因 / 处理 |
|---|---|
| 复位后串口 0 字节、`waiting for download` | 漏了人工短按重启；esptool 自动复位在本板无效 |
| esptool 识别不到芯片 | 烧录前没长按进 Boot 模式 |
| HTTP OTA `--sha256` 报错 | http URL 必须带 sha256；用 `certutil -hashfile` 算 |
| OTA timeout | server 用了 localhost/不可达 IP，或防火墙挡了端口 |
| `error code=wifi_*` | 设备 Wi-Fi 未连，先配网 |
| `error code=park_required` | 设备在录音/有 OTA 在跑，等空闲 |
| idf_cli 在 Git Bash 下静默不构建 | MSys/Mingw 标记被 idf.py 拒绝；脚本已自动剥离，仍异常时换原生 cmd 环境 |

## 相关文件

- `scripts/idf_cli.py` / `scripts/idf_cli.yaml` —— 串口编译/烧录/监控
- `desktop/windows/src/ota_command.cc` —— VoiceStickCtl 参数与退出码权威源
- `Doc/Plan/lan-http-ota-pull-design.md`、`Doc/Plan/windows-ota-command-tool.md` —— OTA 设计
- `Doc/Ref/protocol.md` —— BLE/Wi-Fi/OTA 帧契约
- `VERIFICATION.md`（同目录）—— 本 skill 自动发现链路的验证报告
