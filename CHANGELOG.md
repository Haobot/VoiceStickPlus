# CHANGELOG.md

## 2026-06-26 v1.6.8

- 版本号从 `v1.6.7` 更新到 `v1.6.8`。
- feat(desktop, firmware): 在主设置中显示设备已连接 Wi-Fi 信息。
  - `desktop/windows/src/app_config.h/.cc`：新增 `DeviceWifiInfo` 结构、`show_device_wifi_info` 开关与 `device_wifi_infos` 映射；在 `[device.<id>.wifi_info]` 段持久化存储每个配对设备的 SSID 与 IP；解绑设备时同步清理；新增 `Load(path)`/`Save(path)` 重载便于测试。
  - `desktop/windows/src/ble_protocol.h/.cc`：新增 `ShowWifiInfoPayload()`，生成 `{"event":"show_wifi_info","enabled":...}` 控制帧。
  - `desktop/windows/src/ble_central_win.h/.cc`：新增 `SendShowWifiInfo()`，向指定或全部已连接设备下发 `show_wifi_info`。
  - `desktop/windows/src/voice_stick_coordinator.h/.cc`：连接成功后与配置变更时均发送 `show_wifi_info`；连接建立后向所有设备请求 `wifi_status_request`，确保状态及时同步。
  - `desktop/windows/src/settings_dialog.h/.cc`：新增"显示已连接 Wi-Fi 信息"复选框与只读的 SSID/IP 编辑框；打开设置时请求 Wi-Fi 状态，收到状态后刷新显示；窗口客户区高度由 720 增至 840。
  - `desktop/windows/src/win32_app.cc`：收到固件 `wifi_status` 后将 SSID/IP 写入 `config_.device_wifi_infos` 并持久化，同时触发设置界面刷新。
  - `desktop/windows/src/localization.h/.cc`：补充中英文"显示已连接 Wi-Fi 信息"、"Wi-Fi SSID"、"IP 地址"、"WIFI Idle"等文案。
  - `firmware/components/voice_net/include/voice_net.h` / `voice_net.c`：新增 `voice_net_set_status_changed_callback()` 与 `voice_net_get_status()`，在 STA 状态变化时回调 SSID/IP/状态字符串。
  - `firmware/main/main.c`：解析 `show_wifi_info` 控制事件；新增 `update_wifi_info_ui()` 与 `on_voice_net_status_changed()`；注册 Wi-Fi 状态变化回调，实现开关与网络变化的实时 UI 刷新。
  - `firmware/components/ui_status/include/ui_status.h` / `ui_status.c`：新增 `ui_status_set_wifi_text()` 与 `s_wifi_label`，在 IMU 调试行下方居中显示 SSID/IP 或 "WIFI Idle"，由桌面端开关控制显隐。
  - `desktop/windows/tests/core_tests.cc`：新增 `TestAppConfigWifiInfoRoundTrip()`，验证 `show_device_wifi_info` 与 `[device.<id>.wifi_info]` 的读写持久化。
  - 同步更新 `Doc/Ref/protocol.md`，补充 `show_wifi_info` 事件说明；新增 `Doc/Plan/...` 实施计划与 `Doc/...` 设计文档；删除过时的 `run_build.bat`。
- feat(firmware/ui): 放大设备号、电量与 Wi-Fi 信息字号，并将电量百分比移至电池图标下方。
  - `firmware/components/ui_status/ui_status.c`：
    - 顶部设备号 `s_top_label` 字体由 `lv_font_montserrat_10` 提升至 `lv_font_montserrat_16`，宽度由 66 增至 72，垂直位置略微下移至 y=2，提升可读性。
    - 电池图标 `s_battery_shell` 水平位置由 `-31` 右移至 `-8`，与右侧边界对齐。
    - 电量百分比 `s_battery_label` 字体由 10 提升至 16，宽度由 28 增至 46，并使用 `lv_obj_align_to` 对齐到电池图标底部右侧，避免与设备号/IMU 行重叠。
    - IMU 调试行 `s_imu_label` 垂直位置由 y=14 下移至 y=38，为大号设备号与电池图标留出空间。
    - Wi-Fi 信息行 `s_wifi_label` 字体由 10 提升至 14，垂直位置由 y=80 下移至 y=104，保持与 IMU 行的新布局间距。
