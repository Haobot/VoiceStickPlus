# CHANGELOG.md

## 2026-06-30 v1.7.2

- 版本号从 `v1.7.1` 更新到 `v1.7.2`。
- fix(windows): 修复开启精修后悬浮窗持续闪动、程序卡死。
  - 根因：`OverlayWindow::OnTimer` 在 `kListening` 模式下每 16ms 无条件 `InvalidateStaticLayer`，导致 `BuildStaticLayer`/`PaintText` 每 16ms 全量重建 D2D 文本布局（`CreateTextLayout` 不缓存），UI 线程渲染过载卡死；流式精修 token 每 ~60ms 重置 140ms 文字滚动过渡动画，`scroll_offset` 中途反复跳动闪动。
  - `desktop/windows/src/overlay_window.cc/.h`：`OnTimer` 在 kListening 静态文本时不重建 static layer，仅重绘动态指示器（音浪条），复用缓存文本布局；新增 `AppendPartial` 流式追加入口，跳过文字滚动过渡动画；`Show` 增加 `skip_text_transition` 参数。
  - `desktop/windows/src/voice_stick_coordinator.cc/.h`：`UiDelegate` 增加 `AppendPartial` 纯虚；`TransformText` 精修分支恢复 `refiner_.RefineStream`，`on_token` 节流式调 `AppendPartial` 逐字流式显示。
  - `desktop/windows/src/win32_app.cc/.h`：`AppendPartial` 转发至 `overlay_->AppendPartial`。
  - `desktop/windows/tests/core_tests.cc`：`FakeUi` 补 `AppendPartial` 实现。
  - 新增 `Doc/Plan/overlay-render-streaming-refine.md`。
  - 实测精修耗时 2.5~12.8s 随文本长度增长，LLM 延迟不可压缩；"压缩总时间"方向（definite 分段并行 / 倒计时并行）经查证否决，优化重心为"让精修等待可感知"。

## 2026-06-30 v1.7.1

- 版本号从 `v1.6.8` 更新到 `v1.7.1`。
- fix(固件): hold_to_talk 连接就绪过渡期录音启动增加重试。
  - 设备重连后 Windows 需重新做 GATT 服务发现 + 特征值订阅才能让 `ble_ready` 置位（约 1.5–2s）。在此过渡期内按住按钮触发 hold threshold，`start_recording` 会因 `ble_ready=0` 被拒，且只调一次不再重试，用户必须松开重按。
  - `firmware/main/main.c`：hold threshold 到点因 `ble_ready=0` 被拒时，只要按钮仍按下就按 100ms 间隔重试，覆盖订阅过渡期；超时（2s）或松开则干净放弃。只对 `ble_ready=0` 这一可恢复原因重试，ota/ui_state 等不可恢复原因走原放弃逻辑。复用 `s_double_click_timer`，新增 `s_recording_retry_pending` 状态，`handle_primary_up` 与断连清理同步处理。
  - 新增 `Doc/Plan/hold-to-talk-recording-start-retry.md`。
- fix(固件): 扩大 NimBLE mbuf 池并节流告警，消除音频通知瞬时耗尽。
  - 长录音中 central 处理慢 / conn interval 偏大时，NimBLE host 队列堆积未发送 notification，MSYS_1 池（100 块 ×256B）瞬时耗尽，出现 `tx seq=N mbuf alloc failed` 刷屏并丢帧。
  - `firmware/sdkconfig` / `sdkconfig.defaults`：`CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT` 100→200（多占约 25KB 内部 RAM，Wi-Fi 已禁用空间充裕）。真机长录音验证再无 alloc failed。
  - `firmware/components/voice_ble/voice_ble.c`：`voice_ble_send_audio` 的 alloc failed 告警节流（`s_mbuf_fail_streak`，每 10 次打印一条，恢复时打印恢复计数），断连清零。
- docs: 修正 `CLAUDE.md`/`AGENTS.md` 中 `Doc`/`docs` 措辞与 `run_build.bat` 悬空引用。

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
