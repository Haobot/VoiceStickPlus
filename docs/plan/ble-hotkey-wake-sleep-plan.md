# BLE 热键唤醒与休眠策略方案

## 问题

Stick 在空闲后会先降低屏幕亮度，电池供电且继续空闲时进入 ESP32 deep sleep。Windows 全局快捷键依赖已有 BLE 连接向固件写入 `remote_button_down` / `remote_button_up` 控制事件；设备进入 deep sleep 后，BLE controller、GATT 服务和连接状态都会停止，因此电脑端没有无线通道可以唤醒设备。

本次修复的目标是：已连接电脑时保持 BLE 在线，让电脑快捷键随时可用；未连接且电池供电时继续保留 deep sleep 省电策略。

## 约束

- ESP32 deep sleep 不能通过普通 BLE GATT 写入唤醒。
- 当前深睡唤醒源是 Stick 正面主键 GPIO11。
- 桌面端快捷键只能在已有 BLE 连接上发送远程主键事件。
- 屏幕变暗/息屏不应等同于整机 deep sleep。

## 推荐策略

### 固件

- BLE 已连接时：
  - 保留 30 秒无操作后的屏幕变暗逻辑。
  - 暂停 5 分钟 deep sleep timer。
  - `enter_deep_sleep()` 入口再次检查 BLE 连接，避免 timer 与连接事件竞态导致误睡。
- BLE 未连接且电池供电时：
  - 保留 5 分钟 deep sleep 策略。
  - 进入 deep sleep 后只能通过主键等硬件唤醒源唤醒。
- BLE 断开后：
  - 清理录音/OTA 状态。
  - 从断开时刻重新启动显示变暗和 deep sleep 计时。

关键修改点：

- `firmware/main/main.c`
  - 新增 `deep_sleep_allowed_now()`。
  - 修改 `restart_deep_sleep_timer()`。
  - 修改 `enter_deep_sleep()`。
  - 修改 `APP_EVENT_BLE_DISCONNECTED` 分支。

### Windows 桌面端

- 保持现有热键录音链路：`HandleGlobalHotkeyPressed()` 解析目标设备后发送 `RemoteButtonAction::kDown`。
- 无连接时不尝试“无线唤醒 deep sleep”，只给出明确提示：
  - 没有配对设备：提示先配对。
  - 有配对但无连接：提示设备可能已休眠，需要按主键唤醒。

关键修改点：

- `desktop/windows/src/voice_stick_coordinator.cc`
  - 改进 `HandleGlobalHotkeyPressed()` 的无连接分支提示。
- `desktop/windows/tests/core_tests.cc`
  - 覆盖无连接热键不发送 remote button。
  - 覆盖已连接热键正常发送 remote button。

## 验证

### 自动验证

- Windows CTest：

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe" --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests'
```

- 固件构建：

```sh
cd firmware
. "$HOME/esp/v5.5.1/esp-idf/export.sh"
idf.py build
```

### 手动验证

1. 电池供电且 Windows 已连接，等待超过 5 分钟：
   - Stick 屏幕可变暗。
   - 设备不进入 deep sleep。
   - Windows 托盘仍显示连接。
   - 按全局快捷键可立即开始录音。
2. 屏幕已变暗但 BLE 已连接：
   - 按全局快捷键后固件收到 remote primary down。
   - 屏幕亮度恢复并进入录音 UI。
3. 电池供电且未连接，等待超过 5 分钟：
   - Stick 仍进入 deep sleep。
   - Windows 热键不开始录音，并提示按主键唤醒。
4. BLE 连接断开后：
   - deep sleep timer 从断开时刻重新开始计算。
5. USB/外接电源供电：
   - 继续跳过 deep sleep。

## 风险与边界

- 已连接状态不 deep sleep 会增加电池消耗，这是电脑快捷键随时可用的必要代价。
- 电脑睡眠、蓝牙栈节能、距离过远仍可能导致 BLE 断开；断开后设备会恢复未连接 deep sleep 策略。
- 已经进入 deep sleep 的设备无法被电脑热键通过 BLE 唤醒，需要按主键物理唤醒。
- 后续如需进一步省电，应优先优化屏幕背光和 BLE 慢连接参数，而不是在已连接状态下 deep sleep。
