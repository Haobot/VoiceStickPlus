# CHANGELOG.md

## Unreleased

- 编码器旋转快慢分档（Windows）：窗口格速 = steps × 100（格/秒）≥ `encoder_rotate_fast_threshold`（默认 400）判为快速手势，改注快速档按键（默认 cw=`pagedown` / ccw=`pageup`，慢速逐行、快速翻页）；一次快速手势只注入一次并进入停转锁定，屏蔽减速段慢速输出与换向事件，静默 >250ms 判定停稳后恢复识别；快速档按键非法回退普通按键；设置对话框「编码器」一节新增阈值与快速档按键 3 个控件。

## 2026-07-29 v2.2.0

编码器设置与热词处理。

- 新增热词处理（Windows）：划词加词可选 LLM 提炼，长选文自动提取热词去重入表，浮窗展示结果 3 秒；设置对话框新增「热词处理」配置栏（启用开关 + 提示词，复用文本精修 LLM 配置）。
- 新增编码器设置项（Windows + 固件）：MiniEncoderC 旋转/单击/双击动作可配置。
  - 旋转 cw/ccw 自定义按键注入（key_spec 热键语法：方向键/enter/tab/pageup/pagedown/f1-f24/单字符/ctrl/alt/shift/win 组合），非法配置回退方向键。
  - 单击动作可选 recording（同主键录音语义，默认）或 key（注入自定义按键）；配 key 时桌面端派生下发固件录音门控关闭（编码器按压只发 click/double_click 按键事件）。
  - 双击动作可选 key（默认 enter，沿用取消当前会话结构）或 recording（经 remote_button 通道切换录音起停）。
  - 编码器录音灯颜色 8 预设（red/.../off）经 BLE 下发固件并 NVS 持久化，重启保持。
  - 固件按键事件新增 `source:"encoder"` 标签，桌面端按 source 路由编码器事件，物理键路径行为不变。
  - 设置对话框新增「编码器」一节（9 个控件，按键字段保存前 ParseKeySpec 校验）。

## 2026-07-25 v2.1.2

性能修复与录音稳定性。

- **perf(desktop/windows)**: 快速重启回连两轮提速（~10.8s → 中位 5.4s，7 样本全部一次成功）。
  - 一轮：僵尸链路安定窗（4.5s），根因定性为 Windows 僵尸链路（OS 持有已消失对端的旧链路，首次连接空挂 ~3.5-4s + 5s 退避）。
  - 二轮：安定窗缩至 1.5s（判出僵尸时已主动 Close，栈立即发 LL_TERMINATE，无需等被动监督超时）+ state 订阅 2.5s 应用层超时（撞未死僵尸提前取消，不再空挂 3.5-4s）+ zombie_suspect 免退避重试（15s 窗内最多 3 次，覆盖连按重启产生的多重僵尸）。
- **perf(desktop/windows)**: 深睡唤醒回连提速（连接编排 1.4s + boot 到广播 0.9s）。
- **fix(desktop/windows)**: 录音稳定性——finalizing/停滞 watchdog 兜底与 drain 尾帧保留，修复松开按钮后偶发卡 listening 与尾字丢失。
- **feat(firmware)**: 软件 AGC 电平归一与按键音抑制——HPF 后、Opus 前自研 AGC（target -6dBFS、max +20dB、噪声门、0.8FS 瞬时限幅），关闭硬件 ALC；开头 60ms 静音+淡入、drain 尾帧淡出消除按键音。轻声均值电平 -31~-39dB → -17~-25dB。
- **feat(scripts)**: 腾讯 ASR 回放工具与调试音频频谱分析页（`scripts/e2e_test/spectrogram_server.py`）。
- 文档：回连压缩设计与实测记录、BLE 僵尸链路经验、项目记忆蒸馏增补。

## 2026-07-21 v2.1.1

- **fix(windows)**: 设置保存闪退根治--`SaveSettings` 的 `config.Save()` 增加异常安全（try-catch），并修复路径含非 ASCII 字符时按 ACP(GBK) 解析抛 `system_error` 致 `std::terminate` 闪退（改用 UTF-8/UTF-16 显式转换）。
- **fix(windows)**: BLE 僵尸会话心跳探活与拆除重连，修复 deep sleep 唤醒后卡 pairing 死锁、无法回连录音。
- **feat(firmware)**: 点动模式主键双击直接注入回车，不触发录音。
- **feat(desktop/windows)**: 第三方输入法按触发模式（长按/点按）分别记忆触发热键。
- **fix(desktop/windows)**: 第三方输入法触发方式与全局 `interaction_mode` 解耦，修复切回 focused_app 长按失效。
- **fix(desktop/windows)**: 点动模式快速点动错位修复（`session_id` 校验）。
- 文档：新增 MSI 代码签名证书配置指南、蒸馏项目记忆为长期经验参考、CLAUDE.md/AGENTS.md 增量同步。

## 2026-07-06 v1.9.0

- **feat(wechat_input_method)**: 新增微信输入法语音输出模式——将识别语音经 Opus 解码为 PCM 后渲染到系统虚拟麦克风（如 VB-CABLE Output），供微信输入法等应用作为音频输入源。
  - Windows 端新增 Opus 解码器（vendored xiph/opus v1.5.2）、PCM 环形缓冲、WASAPI 虚拟麦克风渲染器（shared mode + AUTOCONVERTPCM）。
  - 配置模型、协调器状态机、设置对话框 UI 扩展支持 `wechat_input_method` 输出模式。
  - 修复虚拟麦 16kHz 格式被 WASAPI 拒绝导致渲染启动失败（需 AUTOCONVERTPCM）。
  - 接入调试音频缓存并修复 BLE 闪断后录音卡死（漏接 `CancelActiveCycleIfDeviceDisconnected` 断连清理分支）。
- **fix(audio)**: 改善近场 ASR 识别效果。
  - 根因：ES8311 PGA 固定 36 dB（最大档，63 倍）致近场 ADC 硬削波，谐波失真破坏语音频谱。固定增益不改变 SNR，问题是增益超出 ADC 线性区导致削波。
  - 方案一：PGA 36→24 dB + Opus 码率 20→32 kbps。
  - 方案二：启用 ES8311 硬件 ADC ALC（target -18dBFS + winsize=2 + automute off）自适应不同说话距离。
- **fix(windows)**: 去除管理员启动要求回退为 asInvoker，不再弹 UAC；开机自启从任务计划程序改回 `HKCU\...\Run`。
  - 权衡：`focused_app` 粘贴注入模式无法再向微信 4.0 等高完整性窗口发送 SendInput（UIPI）；微信输入法模式走虚拟麦克风渲染，不受影响。

## 2026-07-05 v1.8.2

- 版本号从 `v1.8.0` 更新到 `v1.8.2`。
- fix(windows): 修复腾讯 ASR 配置字段映射错误导致的 4002 "密钥不存在"。
  - 根因：`settings_dialog.cc` / `onboarding_dialog.cc` 中 ASR 提供商下拉框切换时只区分了 VoiceStick Cloud 与其他，切换到 Tencent 时错误地从 `volcengine_api_key` 加载密钥，用户保存后可能把正确的 Tencent SecretId 写入错误字段。
  - 修复：用显式 switch 把三个提供商各自映射到正确的配置字段。
  - `desktop/windows/src/app_config.cc`：加载配置时自动迁移——若当前提供商为 Tencent，且 `volcengine_api_key` 是 `AKID...` 形式的 Tencent SecretId，而 `tencent_secret_id` 为空或不以 `AKID` 开头，则自动回迁并落盘。
  - `desktop/windows/src/app_config.cc`：TOML 解析时对 `tencent_secret_id`、`tencent_secret_key`、`tencent_appid`、`volcengine_api_key`、`voicestick_api_key`、`llm_api_key` 等凭据字段自动 Trim，避免前后空格导致鉴权失败。
  - `desktop/windows/tests/core_tests.cc`：新增 `TestTencentSecretIdRecoveryFromVolcengineField()` 与 `TestTencentCredentialsTrimmedOnLoad()`。

## 2026-07-01 v1.8.0

- **breaking**: 移除 Wi-Fi STA 配网与局域网 HTTP(S) OTA pull 全链路，固件升级改走 BLE OTA（与 USB COM 口烧录，后者未来实现）。
  - 固件：删除 `components/voice_net/` 整个组件、`VOICE_NET_DISABLE` 宏与 `main.c` 全部 `voice_net_*` 调用点；`ota_commit` 改为直接 `esp_ota_mark_app_valid_cancel_rollback`；boot 兜底签到改为无条件；清理 `sdkconfig.defaults` 中 Wi-Fi/mDNS/TLS 相关项；`ui_status` 移除 Wi-Fi 信息行。
  - Windows：删除 `wifi_settings_dialog`/`wifi_credentials_win`/`ota_command`/`voice_stick_ctl` 四个源文件与 `VoiceStickCtl` 目标；清理 `ble_protocol`/`app_config`/`coordinator`/`ble_central_win`/`win32_app`/`settings_dialog`/`localization` 中 Wi-Fi/LAN-OTA 符号与 10 个 `core_tests`；保留 BLE OTA、固件清单、WinSparkle。
  - 文档：`Doc/Ref/protocol.md` 删 Wi-Fi 章节、`show_wifi_info`/`wifi_status` 帧；删除 5 个 `Doc/Plan` 方案文件与 `scripts/probe_wifi_provisioning.py`；`CLAUDE.md`/`AGENTS.md` 逐行清理。
  - macOS 端零改动（从未实现 Wi-Fi/LAN-OTA）。

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
