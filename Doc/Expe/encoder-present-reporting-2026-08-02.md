# 编码器在线状态上报与设置区块显隐（2026-08-02）

## 需求与方案

需求：Stick 把「当前是否装有 MiniEncoderC 编码器」上报给 Windows 端；设置对话框据此显隐编码器区块（无编码器则整段隐藏）。

最终方案（协议见 `Doc/Ref/protocol.md` 的 `encoder_status` 条目）：

- 固件新增独立小帧 `{"event":"encoder_status","present":true|false}`（约 44 字节），在 `state_tx` 订阅后紧随 `device_info` 发送；编码器运行期 I2C 连续失败降级为 absent 时主动补推一次（链路断开时只更新缓存标志，下次连接补报）。
- 固件侧状态源：`mini_encoder_c_present()` → `voice_ble_set_encoder_present()`（voice_ble 静态标志，因为 `device_info`/`encoder_status` 由 voice_ble 内部 subscribe 回调触发，main 无法直接传参）。
- Windows：`StateEvent.encoder_present`（`optional<bool>`，`ble_protocol.cc` 仅对 `encoder_status` 事件解析 `present` 键）→ 协调器路由到新 UI 接口 `SetDeviceEncoderPresent(device_id, present)` → `Win32App::device_info_map_` 更新 `DeviceInfo.encoder_present`（默认 true）。
- 显隐规则（`Win32App::ShowSettings`）：任一已知设备报告在线即显示；`device_info_map_` 为空（未连接过/老固件）默认显示。仅当所有已知设备都报告 absent 才隐藏。`SettingsDialog` 新增构造参数 `show_encoder_settings`，替换原编译期常量 `kShowEncoderSettings`；对话框是缓存复用的，显隐标志变化时销毁重建。
- 兼容约定：老固件不发 `encoder_status`，桌面端一律按「在线」处理，编码器设置保持可见。

## 关键教训

1. **`device_info` 曾处于 BLE 通知 MTU 临界点（已于 2026-08-02 修复）**。实测（MTU 247 链路）旧固件 258 字节的 device_info JSON 被截断到 244 字节，桌面端 parse failed。修复：精简 `interaction_modes` 数组（移除 `hold_to_talk_instant`，plan 文档注明该广告位可选且桌面端从不解析）→ 235B；并在 `send_state_json` 加超预算告警（`4B 帧头 + JSON > att_mtu − 3` 时 WARN，不拦截发送）。教训仍成立：给 state 帧加新字段前必须先算字节数，新增状态一律走独立小帧。
2. **固件加字段容易，发现 MTU 截断靠真机日志**。本次是在重启应用后例行检查 `VoiceStickApp.log` 时撞到 `state notify ... parse failed` 才发现的。构建通过 ≠ 链路可用，改 BLE 协议后必须看一次真机连接日志。
3. **向后兼容方向要想清楚**：新字段/新事件缺失时默认什么？这里默认「在线」（显示设置），因为误隐藏比误显示更伤用户——老固件用户升级桌面端后设置凭空消失会被当 bug。
4. **VoiceStickUi 接口有三处实现**：`Win32App`、`core_tests.cc` 的 FakeUi、`integration_tests.cc` 的 FakeUi。加纯虚函数必须三处同改，否则链接/编译错。
5. **设置对话框的布局表机制**（`settings_dialog.cc`）：控件一律创建，只有加入 `layout_` 表的条目才参与 Relayout；未入表控件由 BuildControls 统一隐藏，但 Load/Save 照常读写——所以整段隐藏不会丢 config.toml 里的值，按加载值回写。区块前置的 `separator()` 要挂同样的可见性谓词，否则末尾残留孤立分隔线。
6. **`build_win.bat` 第一步会杀掉运行中的 VoiceStick.exe**：后台跑的 app 任务报 failed（exit_code 1）是预期行为，不是崩溃。
7. **后台构建期间继续改源码会导致产物不一致**：本次第一版构建进行中又改了 `win32_app.cc`，靠核对 `.obj` 时间戳晚于源码 mtime 才确认产物包含了全部修改。更稳的做法是改完再构建，或构建后对改动文件重新增量编译一次。

## 验证状态

- 固件 `idf_cli.py -c` 编译通过；Windows `build_win.bat` 编译通过；CTest `voicestick_windows_tests` 全绿（含新增 `TestEncoderStatusParsing`：present true/false/缺字段/其它事件不携带/DeviceInfo 默认值）。
- 真机端到端已验证（2026-08-02，VS-53A8，BLE OTA 1485424B 完成并切分区重启）：重连后日志依次出现 `encoder_status` 45 字节完整帧（无截断）、解析成功、`SetDeviceEncoderPresent VS-53A8 encoder_present=true`；同连接上 device_info 仍 parse failed（既存 MTU bug 的再次实证）。present=true 走显示分支符合预期；隐藏路径（无编码器设备）未真机验证，由单测与缺省逻辑兜底。
- 老固件（2.2.0 release）兼容已验证：无 encoder_status 事件，桌面端缺省按在线处理，编码器设置保持可见。
