# 编码器设置项设计（MiniEncoderC 可配置化）

日期：2026-07-28
状态：已批准（方案 A）
前置：feat/mini-encoder-c 已合并 main（编码器驱动、旋转箭头、录音红灯、双击 Enter 均为硬编码/仅 config.toml 行为）

## 1. 需求

在 Windows 设置对话框中新增「编码器」一节，暴露：

- 旋转注入总开关（已有键 `encoder_to_arrow`，此前只能手改 config.toml）
- 旋转方向翻转（已有键 `encoder_rotation_invert`）
- 旋转顺时针/逆时针注入的按键，用户自定义（当前硬编码 Down/Up）
- 录音灯颜色（当前固件硬编码红色），含「关」
- 编码器按钮单击/双击各自的动作：`录音` 或 `自定义按键`（当前单击固定录音、双击固定 Enter）

架构边界不变：所有手势语义映射在桌面端执行；固件只做事件上报（补 source 标签）与 LED 颜色执行（NVS 持久化）。

## 2. 协议扩展（固件 → 桌面，只加可选字段）

编码器触发的按键事件增加 `"source":"encoder"`：

```json
{"event":"button_down","button":"primary","session_id":1,"source":"encoder"}
{"event":"button_up","button":"primary","duration_ms":7043,"session_id":1,"source":"encoder"}
{"event":"button_click","button":"primary","duration_ms":131,"source":"encoder"}
{"event":"button_double_click","button":"primary","source":"encoder"}
```

- 物理主键/侧键事件**不带** `source` 字段；桌面端缺省视为物理键，零迁移成本。
- `button` 字段仍填 `"primary"`（编码器在固件侧等价主键的语义不变）。
- 兼容性：旧桌面忽略未知 JSON 字段（行为同当前）；旧固件 + 新桌面时编码器退化为与物理主键一致（单击录音、双击 Enter）。三端无需同步发布。
- 同步 `Doc/Ref/protocol.md`；macOS 不改代码。

## 3. 配置结构（config.toml）

```toml
# 旋转（桌面端消费）
encoder_to_arrow = true              # 已有：旋转注入总开关
encoder_rotation_invert = false      # 已有：方向翻转
encoder_rotate_cw_key = "down"       # 新增：顺时针注入的按键
encoder_rotate_ccw_key = "up"        # 新增：逆时针注入的按键

# 录音灯（桌面配置 → BLE 下发 → 固件 NVS）
encoder_led_color = "red"            # 新增：red/green/blue/yellow/purple/cyan/white/off

# 按键手势（桌面端消费）
encoder_press_action = "recording"        # 新增：单击动作 recording|key
encoder_press_key = ""                    # 新增：单击自定义按键（action=key 时生效）
encoder_double_click_action = "key"       # 新增：双击动作 key|recording（recording=双击开始/停止录音，等价 click_to_talk 点按起停语义）
encoder_double_click_key = "enter"        # 新增：双击自定义按键（默认 enter=现行为）
```

- 按键值语法复用现有热键格式：单键（`up`/`down`/`left`/`right`/`enter`/`tab`/`esc`/`pageup`/`pagedown`/`volumeup`/`volumedown`/`f1`-`f12`/单字符）或修饰键组合（`ctrl+z`、`ctrl+shift+v`；修饰键 ctrl/alt/shift/win）。
- 单击 `recording` = 现有录音语义（hold_to_talk/click_to_talk 不变）；`key` = 单击注入自定义键、不录音。
- 双击 `key`（默认 enter）保持现行为；`recording` = 双击开始/停止录音（等价 click_to_talk 的点按起停语义；物理主键双击注入 Enter 的行为不受影响）。
- `off` 灯色 = 录音也不亮，覆盖「关闭灯光」；不做亮度调节（YAGNI）。
- 默认值全部等价当前硬编码行为，旧配置文件不新增任何键也能无缝升级。

## 4. 桌面端事件路由与按键注入

- `StateEvent` 解析增加 `source` 字段；协调器在按键事件入口分流：`source=="encoder"` → 编码器可配置动作表；否则 → 现有物理键逻辑（不改）。
- 单击 `key`：在 click（松开配对成单击）时注入一次自定义键；同时按第 5a 节下发门控关闭固件侧录音触发。`recording` 走现有主键路径（门控打开）。
- 双击 `key`：注入 `double_click_key`（默认 enter，等价今天）；`recording` 按第 5b 节 remote_button 切换录音起停。
- 旋转：`HandleEncoderRotate` 把硬编码 `SendArrowUp/Down` 换成配置表——先按 `encoder_rotation_invert` 翻转 cw/ccw，再取对应 key 注入；`encoder_to_arrow` 总开关与录音/识别/体感门控不变。
- `InputInjector` 新增 `SendKeyCombo(spec)`：修饰键按下 → 主键 → 全释放，主键带 scan code（第三方输入法要求，见既有经验）；单键退化为一次按键；非法 spec 记日志并忽略该次注入。
- LED 下发编排：`UpdateConfig` 与设备连接全量重发链路上各加一项 `SendEncoderLedColor`。

## 5. 录音灯颜色链路（桌面 → 固件 NVS）

复用 tap 设置的既有模式：

```text
config.toml encoder_led_color
  → 设置保存 / 设备连接时 SendEncoderLedColor("red")
  → control_rx: {"event":"encoder_led_color","color":"red"}
  → 固件 ble_control_cb 识别 → 预设名映射 RGB → nvs key "enc_led"（i32 打包 0xRRGGBB）
  → boot 时 load（无值默认红）
  → 录音亮灯（main.c 现 255,0,0 调用点）改用存储 RGB
```

- 未知颜色名：固件忽略并保持当前值（LOGW）；桌面端保存前校验预设清单。
- 灭灯路径（停止录音/断连/init 清灯）全部不动；`off` 即存 0x000000。
- 固件收到设置即存 NVS（同 tap_en/tap_lvl2 模式），单设备全局命名空间。

## 5a. 单击 key 动作的固件门控（架构约束补丁 1）

桌面端无法凭空造出音频流；编码器按钮在固件侧与物理主键共用录音触发逻辑（长按 300ms 阈值后自动开播）。为避免 `press_action=key` 时「长按旋钮固件空播音频、桌面丢弃」，新增门控：

```text
press_action = key        → control_rx: {"event":"encoder_recording_gate","enabled":false}
press_action = recording  → control_rx: {"event":"encoder_recording_gate","enabled":true}（默认）
```

- 固件收到即存 NVS key `enc_rec_gate`（i32 0/1，默认 1），boot 加载，同 tap 模式。
- 门控关闭时：编码器来源的按下**只发按键事件、不启动音频会话**（录音 LED 也不亮）；物理主键不受影响。
- 下发编排与 LED 颜色相同：`UpdateConfig` 与设备连接全量重发时各带一项。
- 配置端不新增键：门控值由 `encoder_press_action` 派生。

## 5b. 双击 recording 动作走 remote_button（架构约束补丁 2）

双击配 `recording` 时桌面端翻译成切换语义，复用固件已有的远程按键通道（`remote_button_down/up`，`ble_control_cb` 已支持，primary only）：

- 无活跃会话 → `SendRemoteButton("down")` 开播；有活跃会话 → `SendRemoteButton("up")` 停播。
- 固件零改动；音频链路真实完整（等同 click_to_talk 的点按起停）。
- `SendRemoteButton` 桌面端已存在（`ble_central_win.cc`），协调器直接调用。

## 6. 设置对话框 UI（settings_dialog.cc）

新增「编码器」组，沿用 `LayoutEntry` 声明式排版与 Win32 原生控件：

- ☑ 旋转时注入按键（`encoder_to_arrow`）
- ☑ 旋转方向翻转（`encoder_rotation_invert`）
- 顺时针按键 / 逆时针按键：热键编辑框（复用微信热键捕获/校验模式）
- 录音灯颜色：COMBOBOX（红/绿/蓝/黄/紫/青/白/关）
- 单击动作：COMBOBOX（录音/自定义按键）+ 单击按键热键编辑框（选自定义时启用）
- 双击动作：COMBOBOX（自定义按键/录音）+ 双击按键热键编辑框（默认 `enter`）

保存流程不变：`SaveSettings()` → `config_.Save()` → `on_config_changed` → `UpdateConfig` 即时生效（含 LED 立即下发）。按键值非法时保存旁提示，不写回坏值。

## 7. 测试与兼容性

- Windows core_tests：新配置键解析/保存往返；`source` 解析；encoder 单击/双击动作路由（recording vs key，含双击 recording 的 remote_button 切换）；旋转 invert + 自定义键映射；`encoder_led_color` 与 `encoder_recording_gate` payload 构造；非法按键 spec。
- 固件：`idf.py build` 通过；真机：事件带 source（从 Windows 日志确认）、LED 换色即时生效、重启 NVS 保持、`off` 不亮、门控关闭时长按旋钮不录音不亮灯但事件照发。
- 真机回归：物理主键/侧键行为与合并前一致；旧固件 + 新桌面退化路径（编码器 = 物理键行为）。
- 兼容性见第 2 节；macOS 仅同步 `Doc/Ref/protocol.md`。

## 8. 明确不做（YAGNI）

- 不做按设备（`device.<id>.encoder`）覆盖。
- 不做 LED 亮度调节、多状态变色（识别中/连接态等）。
- 不做 macOS 端代码实现（仅协议文档同步）。
- 不改编码器单击/双击的固件手势检测逻辑（时序阈值等维持现状）。
