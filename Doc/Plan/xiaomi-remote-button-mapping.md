# 小米蓝牙遥控器 2 Pro 按键自定义（罗技式设置界面）设计方案

状态：方案待评审。日期：2026-09-03。

目标：为小米蓝牙遥控器 2 Pro（`RC-XXXX`）提供类似 Logitech Options 的按键自定义界面——用户看到遥控器图，点击图中按钮，为每个按钮指定输出动作（键盘按键/组合键、系统功能、语音功能、恢复原生、禁用）。

## 1. 现状事实（决定方案形状的两个前提）

**前提一：12 个键里只有语音键进入 app。** 语音键走 ATVV Control 特征（2 Pro 按下发 `0x04` STREAM_START、松开发 `0x00` STOP），在桌面端归一化为 `primary` 的 `button_down/up/click/double_click`；其余 11 个键走标准 HID over GATT（service `0x1812`，Report ID 1，每报告最多 3 个 16 位小端 usage），被 OS 原生消费为键盘/消费控制键，桌面端不可见（`Doc/Ref/protocol.md:494-503`）。逆向实测键码表（同文 + `Doc/Plan/xiaomi-remote-2-pro-support.md:38-48`）：

| 图中按键 | HID usage | 当前 OS 原生行为 |
|---|---|---|
| 电源 ⏻ | `0x66` | 系统电源键 |
| 语音 🎙 | 非 HID（ATVV），连带发 F5 | app 录音会话 + F5 抑制 |
| 上/下/左/右 | `0x52`/`0x51`/`0x50`/`0x4F` | 方向键 |
| OK（圆心） | `0x28` | Enter |
| 返回 ← | `0xF1`（非标准） | 系统返回语义 |
| 主页 ⌂ | `0x4A` | Home |
| 菜单 ☰ | `0x65` | Application/上下文菜单键 |
| TV | `0x35` | 依系统 |
| 音量+/− | `0x80`/`0x81` | 系统音量 |
| （静音） | `0x7F` | 实测键表中存在，图无独立键（型号/组合差异） |

**前提二：拦截、注入、配置、设置的零件都已有先例。**

- F5 抑制已有双端 OS 级键盘拦截基建：macOS CGEvent HID event tap（`XiaomiF5Suppressor.swift`，需辅助功能权限），Windows `WH_KEYBOARD_LL`（`voice_f5_suppressor.h/.cc`）。但它不能区分事件来源设备，靠 80ms 邻近窗与 mic-open 证据关联——这是「拦截其余键」必须补齐的能力缺口。
- StickS3 编码器已有「按键→动作」映射的完整模式：配置 `[device.<id>.encoder]` 的 `press_action/press_key` 对、动作枚举、`ParseKeySpec` 热键语法与注入管线（Windows 消费点 `voice_stick_coordinator.cc:1067-1139`），双端各有现成设置对话框（`EncoderSettingsWindowController` / `encoder_settings_dialog.cc`）。本方案的配置模型与动作分发直接复用这套模式。
- 语音键双击检测在桌面端 ATVV 会话层（macOS `XiaomiAtvvSession.swift` / Windows `xiaomi_atvv_session.cc`），双击当前硬编码注入 Enter，是唯一无需新拦截即可自定义的点。

## 2. 需求拆解

功能需求：

1. 图形化设备视图：按实拍图版式呈现 12 个键，可点击（罗技 Options 的核心交互）。
2. 逐键动作配置：点击按钮 → 弹出动作选择 → 保存生效。
3. 动作词汇（v1）：`原生`（默认，维持系统行为）、`键盘按键/组合键`（KeySpec）、`语音功能`（仅语音键相关）、`禁用`。
4. 按设备存储（用户可能有多台 RC），可一键全部恢复默认。
5. 语音键双击动作可配（当前硬编码 Enter）。

非功能需求：

- 不破坏现状：未自定义的键必须保持原生行为（遥控器今天兼任方向键/音量键，回归不可接受）。
- app 退出/崩溃后设备行为自动回归 OS 原生（拦截随进程消亡）。
- 双端（macOS/Windows）能力对齐，config.toml 同一份 schema。
- 固件零改动（红线）。

## 3. 按键可定制性分级

| 级 | 按键 | 说明 |
|---|---|---|
| A：现在就能做 | 语音键双击、语音键单击（click_to_talk 模式下） | 事件已在 app 内，只需把硬编码动作改为配置驱动 |
| B：需 HID 拦截层 | 方向×4、OK、返回、主页、菜单、TV、音量± | 桌面端当前不可见，必须新增拦截子系统 |
| C：不建议开放 | 电源键 | 长按控制遥控器自身休眠/开关机，OS 电源语义有副作用；界面呈现但标注不可配置 |
| 固定语义 | 语音键按住=录音（PTT） | 与 ATVV 音频会话绑定，是产品核心功能，不开放 remap |

## 4. 总体架构（三层，自底向上）

```text
config.toml [device.<id>.buttons]          ← 数据模型（双端同一 schema）
        ↑↓
HID 拦截与归因层（新，B 级键） / ATVV 会话层（已有，A 级键）
        ↓ 归一化为 button id
动作分发器（复用编码器动作模式：native 透传 / KeySpec 注入 / disabled 吞掉）
        ↑
按键映射设置界面（新，罗技式，双端各自实现）
```

### 4.1 数据模型（config.toml）

沿用 `[device.<id>.encoder]` 的「动作/键值对」命名，新增 `[device.<id>.buttons]` 表：

```toml
[device.RC-1A2B.buttons]
intercept = true                 # HID 拦截总开关（B 级键生效前提），默认 false
# 每键一对 <button>_action / <button>_key；缺省即 native
back_action = "key"
back_key    = "alt+left"
home_action = "disabled"
tv_action   = "key"
tv_key      = "ctrl+alt+t"
voice_double_click_action = "key"   # A 级，无需 intercept
voice_double_click_key    = "enter"
```

- 按钮 id 集合：`power voice up down left right ok back home menu tv vol_up vol_down`（`mute` 预留）。
- `native` 是默认与兜底：拦截开启后，未自定义的键由拦截层 1:1 重注入原键，行为等同不拦截。
- 全局默认节 + 按设备覆盖语义与 encoder 完全一致；UI 保存走两端既有的 `Save()`/`SaveSettingsDialog` 路径。
- 同步更新 `Doc/Ref/desktop-config.md` 与 `desktop/macos/Config/config.example.toml`。

### 4.2 HID 拦截与归因层（核心新增，B 级键的前提）

**Windows（可行性高，模式已被第三方工具验证）**：Raw Input + 低级键盘钩关联（LuaMacros 模式）：
- `RegisterRawInputDevices` 注册键盘页（0x01/0x06）与消费控制页（0x0C/0x01），从 `WM_INPUT` 拿到来源设备 `HANDLE`，用 `GetRawInputDeviceInfo` 按 VID `0x2717`/PID `0x32B8` 识别遥控器。
- 现有 `WH_KEYBOARD_LL`（F5 抑制同钩子，含 `VK_VOLUME_*` 等媒体键虚拟码）负责抑制；钩事件与 Raw Input 事件按序关联判定来源。
- 命中自定义映射 → 抑制原键 + 走 `ParseKeySpec` 注入管线注入目标动作；未命中 → 放行（天然透传，无需重注入）。

**macOS（IOHID 观察驱动处置 + tap 吞除原生事件；2026-09-04 真机调试后重构）**：
- 非独占 `IOHIDManager` 按 VID `0x2717` 观察，value 回调的 variable 元素给出精确
  usage（val=1 按下 / val=0 抬起；page 过滤 0x07，防 vendor page 0xFF00 的
  OTA/ATVV 数据流误判）。VID 匹配天然带设备归因，无需时序关联。
- **处置挂在 IOHID 回调**（而非 tap）：0xF1(back)/0x65(menu) 等键在 macOS 不产生
  keyDown CGEvent，挂 tap 的旧架构对它们永远静默。inject 在 IOHID 按下时即注入
  KeySpec；该键若有原生行为（方向/OK/Home/TV→keyDown，音量→subtype=8 的
  systemDefined，data1=(keyCode<<16)|0xa00(down)/0xb00(up)），记吞除标记由 tap
  按 keycode/systemKey 反映射（`RemoteButtonHIDMap.usageToMacKeyCode` /
  `volumeSystemKeyToUsage`）吞掉原生事件防双触发。吞除标记=一次性消费+300ms
  短窗兜底；keyDown 吞后按 keycode 闩锁联动吞自动重复与 keyup；音量 down/up
  用闩锁配对（长按 up 可能超窗）。
- native / intercept=false → 什么都不做，OS 原生消费，忠实透传。
- 已否决备选（真机证据）：
  - 独占 seize：已连接设备被系统 HID 栈持有，`IOHIDDeviceOpen(seize)` 恒
    `0xe00002c1`（命令行 probe 与 app 内一致），放弃。
  - 「IOHID 锚点 + tap 80ms 关联窗」：无必要（IOHID 自带设备归因）且对
    无 CGEvent 的键失效。
- **真机坑（2026-09-04）**：macOS 配对缓存异常时遥控器只有 F5/确认键报告到达
  IOHID 层（其余键静默，连系统音量都不响应）；**「忽略此设备」后重新配对
  （主页+菜单 3-5s）即恢复全部键报告**。排查「按键无报告」先重配对。

**电源键**：两端都不进映射表（或仅可读展示），避免破坏遥控器自身电源管理。

**生命周期**：拦截随 app 进程存亡；进程退出/崩溃即回归原生，无残留状态。

### 4.3 动作分发器

- 拦截层把命中按键归一化为按钮 id，查 `[device.<id>.buttons]` 得动作。
- `key` 动作复用编码器同款注入（Windows 已在协调器侧；macOS 侧补齐对称实现）；`native` 透传/重注入；`disabled` 吞掉。
- **与语音交互状态机解耦**：按钮映射不进 `button_*` 状态事件流，走独立分发路径；语音键按住/松开维持现有 ATVV→primary 语义不动，仅其双击/单击动作改为配置驱动（替换 `VoiceStickCoordinator.swift:534-547` 与 `voice_stick_coordinator.cc:1042-1065` 的硬编码 Enter）。

### 4.4 设置界面（罗技 Options 风格）

布局（双端一致）：

```text
┌───────────────────────────────────────────────┐
│  Xiaomi Remote 2 Pro        [HID 拦截: 开▾]    │
│ ┌───────────┐  ┌────────────────────────────┐ │
│ │           │  │ 选中: 返回键                │ │
│ │  遥控器图  │  │ 动作: [键盘按键        ▾]  │ │
│ │  (可点击   │  │ 键位: [ alt+left  ][捕获] │ │
│ │   热点)    │  │ ───────────────────────  │ │
│ │           │  │ 已自定义: back, home, tv   │ │
│ └───────────┘  └────────────────────────────┘ │
│         [全部恢复默认]          [保存]          │
└───────────────────────────────────────────────┘
```

交互细则（对齐 Logitech Options）：

- 设备图上每个可配置键一个热点，hover 高亮，点击选中；也可直接点击弹动作菜单。
- 已自定义的键带角标/着色；电源键置灰并提示「系统电源键，不可配置」。
- `键盘按键` 动作复用现有热键捕获件（macOS `HotkeyCaptureWindowController` / Windows hotkey 捕获对话框）+ `KeySpec` 校验。
- 拦截总开关旁提示权限状态（macOS 输入监控/辅助功能；未授权给跳转引导，复用 onboarding 先例）。
- 保存即写 config.toml 并经两端既有 `onConfigChanged`/`on_config_changed` 通道内存生效；HID 拦截开关变更即时启停拦截层。

设备图素材：以用户提供的实拍图处理（裁正、去背景、统一光影）为底图 PNG。macOS 复用 `AppDelegate.applicationIconImage()` 的多路径探测加载模式（或正式注册 SwiftPM resources）；Windows 打进 RC 资源用 GDI+/WIC 解码（GDI+ 已链接，`battery_monitor_dialog.cc` 有 PNG 先例，但需新增「解码」路径）。热点坐标用归一化 `(x, y, r)` 表，按设备型号一份（2 Pro 先行，RC003 后续补），按仓库惯例以双端对齐注释的代码常量维护，不新增共享资产管线。

实现落点（沿用既有窗口模式，无新范式）：

- macOS：新增 `ButtonMappingWindowController`（NSWindowController + 自绘 NSView 做 `mouseDown` 命中测试，`BatteryMonitorWindowController` 的手绘 NSView 是先例），入口挂 `StatusController` 设备子菜单，`AppDelegate` 接线。
- Windows：新增 `button_mapping_dialog.{h,cc}`（`CreateDialogIndirectParamW` + 控件工厂模式；若要非模态大窗可参考 `air_mouse_tuning_window`），自绘子窗口 `WM_PAINT` 画设备图 + `WM_LBUTTONDOWN` 命中测试（全端首个 hit-test UI，实现直白），托盘设备子菜单加入口。
- 文案同步两端本地化表（`Localization.swift` ↔ `localization.cc` 的 `L10nKey`/`StringId`）。

## 5. 分期

| 期 | 内容 | 依赖 |
|---|---|---|
| 一期 | config schema + 设置界面（含设备图与热点）+ 语音键双击/单击动作可配；B 级键在 UI 可配置但标注「需开启 HID 拦截」 | 无新子系统，风险低，先交付完整交互 |
| 二期 | Windows Raw Input + LL 钩拦截层；macOS IOHID seize spike → 拦截层；未自定义键透传验证 | 一期数据模型 |
| 三期（可选） | 按前台应用切换映射（真·罗技 app-specific profile）、长按/宏 | 二期稳定后评估 |

## 6. 风险与验证项

- ~~macOS seize 可行性~~ 已 spike 否决（见 §4.2）；macOS 关联式归因的错配率与「输入监控」权限引导是 macOS 侧主要风险，需真机回归（方向键连发、音量连按、与真键盘并发敲击）。
- 关联式归因（Windows 及 macOS 兜底）在系统高负载下的错配率：LuaMacros 模式成熟，风险可控，但需真机回归（方向键连发、音量连按）。
- 拦截开启后未自定义键的 1:1 透传是回归红线：E2E 真机清单逐项过 12 键原生行为。
- 权限变化（用户事后撤销输入监控）→ 拦截层降级为仅 A 级可配并在 UI 明示。
- 多设备：两台 RC 同时连接时按设备 id 独立映射；Raw Input/IOHID 均按设备句柄/对象区分，天然支持。

## 7. 验收标准

- 双端构建与既有测试通过（Windows `ctest`；macOS `swift run VoiceStickTests`），新增映射解析/分发的单元测试。
- 真机：语音键双击动作可改；开启拦截后自定义键输出目标键位、未自定义键行为与拦截前完全一致；app 退出后全部回归原生。
- 文档同步：`Doc/Ref/protocol.md`（HID 键拦截章节）、`Doc/Ref/desktop-config.md`、`config.example.toml`、`Doc/Agent/interaction-model.md`、两端本地化表；涉及整体性描述时同步 `AGENTS.md`/`CLAUDE.md`/`CODEBUDDY.md`。
