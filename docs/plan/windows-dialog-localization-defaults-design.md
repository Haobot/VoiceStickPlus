# Windows 弹窗双语与默认显示偏好优化设计

## 背景

Windows 端已经有 `localization.h/.cc` 和 `ui_language` 配置，设置窗口与托盘菜单的一部分文案已支持中英文切换。但配对设备窗口、首次引导、热键设置、固件更新以及部分 `MessageBoxW` 仍存在硬编码英文或中文，导致语言选项切换后界面体验不一致。

同时，当前未显式配置时的主题颜色和悬浮窗位置仍按旧默认值处理：主题色为白色，悬浮窗位置为居中。新需求要求默认主题色改为“自动”，悬浮窗位置改为“底部居中”。

## 目标

1. Windows 端所有用户可见弹窗支持中英文双语，并依据 `ui_language` 的有效语言切换。
2. 配对设备窗口完整接入本地化，包括标题、列名、按钮、手动配对说明、扫描状态和错误状态。
3. Windows 其它弹窗补齐本地化，包括首次引导、热键设置、固件更新、应用层 `MessageBoxW`。
4. 未显式配置时，默认主题颜色为 `auto`，默认悬浮窗位置为 `bottom_center`。
5. 保留用户已有显式偏好，不强制迁移旧配置。

## 非目标

- 不重构 Windows UI 为资源文件 `.rc` 多语言方案。
- 不调整 BLE 协议、固件行为或 macOS 实现。
- 不改变已保存的用户显式主题色、窗口位置、设备输出设置。
- 不新增独立语言包文件；继续使用现有编译期字符串表。

## 数据模型与配置契约

### 语言

沿用现有配置项：

```toml
ui_language = "system" # 或 "en" / "zh-Hans"
```

运行时统一使用：

```cpp
EffectiveUiLanguage(config.ui_language)
```

得到有效语言后传给弹窗或在窗口内部缓存，所有用户可见文案通过 `TrW(StringId, language)` 获取。

### 默认主题色与悬浮窗位置

默认值语义调整为：

```cpp
OverlayThemeColor::kAuto
OverlayPosition::kBottomCenter
```

保存配置时，设备级覆盖只保存与新默认值不同的值：

- `device_theme_colors` 默认过滤值改为 `OverlayThemeColor::kAuto`。
- `device_overlay_positions` 默认过滤值改为 `OverlayPosition::kBottomCenter`。

这样新用户或新设备不会把旧默认值写入配置；用户显式选择过的非默认值会继续保存。

## 组件设计

### 1. `localization.h/.cc`

扩展 `StringId`，补充 Windows 弹窗所需文案。文案使用稳定语义 key，不用英文原文作为 key。

新增文案覆盖：

- 配对设备窗口：标题、扫描状态、列名、手动输入说明、配对/重试/取消按钮、各类错误与等待状态。
- 首次引导窗口：标题、步骤说明、按钮、状态提示。
- 热键设置窗口：标题、当前快捷键、录制按钮、提示、无效热键、冲突提示。
- 固件更新窗口：更新中、取消中、完成、失败、取消/关闭按钮。
- 应用层弹窗：固件更新建议/可用、检查失败、Cloud 配置/试用异常提示。

`LocalizationTablesAreComplete()` 继续作为完整性保护，新增 `StringId` 必须同时填入英文和中文表。

### 2. `PairDeviceDialog`

构造函数增加 `UiLanguage language` 或等价参数，由 `Win32App` 在创建时传入有效语言。

窗口创建和状态更新全部使用本地化文案：

- `BuildDialogTemplate()` 标题使用 `kPairTitle`。
- `BuildContent()` 中列名、按钮、手动 ID 提示使用本地化字符串。
- `StartScan()`、`RestartScanIfNeeded()`、`PairSelectedDevice()`、`PairManualDeviceId()`、`HandlePairing*()` 的状态文本使用本地化字符串。
- 动态文本通过小型格式化辅助函数生成，例如 `VS-1234`、错误详情、固件版本等变量插入。

### 3. 其它 Windows 弹窗

补齐以下文件中的用户可见硬编码文案：

- `onboarding_dialog.cc`
- `hotkey_settings_dialog.cc`
- `firmware_update_dialog.cc`
- `win32_app.cc`

处理方式：

- 对话框类保存有效语言或从传入配置计算有效语言。
- 静态控件、按钮、状态提示用 `TrW()`。
- `MessageBoxW` 标题与正文用 `TrW()` 或本地化格式化函数。
- 托盘菜单中已本地化的部分保持现状，只补遗漏文案。

## 数据流

1. 应用启动加载 `AppConfig`。
2. 通过 `EffectiveUiLanguage(config.ui_language)` 得到实际语言。
3. 创建设置、配对、引导、热键、固件更新等窗口时传入或内部计算该语言。
4. 窗口构建控件时通过 `TrW()` 获取文案。
5. 用户修改语言并保存设置后，后续打开的新窗口使用新语言；已打开窗口按现有窗口刷新能力决定是否即时刷新。

## 边界条件与异常流

- 系统语言既不是中文也不是英文时，`system` 回退英文。
- 蓝牙扫描启动失败时，错误码和系统错误详情保留，同时外层说明本地化。
- 手动配对 ID 无效、设备已配对、等待设备广播、配对超时均显示当前语言。
- 如果新增 `StringId` 漏填某个语言表，核心测试应失败。
- 旧配置文件中已保存的设备级主题色/位置保持原值。
- 配置文件缺少主题色/位置覆盖时使用新默认值。

## 测试策略

遵循红-绿-重构：先补测试，再实现。

### 单元测试

在 `desktop/windows/tests/core_tests.cc` 中补充或调整：

1. `LocalizationTablesAreComplete()` 覆盖新增弹窗文案。
2. `AppConfig::Defaults()` 或等价默认行为断言：
   - 默认主题色为 `OverlayThemeColor::kAuto`。
   - 默认悬浮窗位置为 `OverlayPosition::kBottomCenter`。
3. 保存配置过滤逻辑：
   - 设备主题色为 `auto` 时不写入 `device_theme_colors` 覆盖。
   - 设备悬浮窗位置为 `bottom_center` 时不写入 `device_overlay_positions` 覆盖。
   - 显式非默认值仍会写入。

### 构建与回归验证

修改完成后运行：

```powershell
cmake -S desktop\windows -B desktop\windows\build-x64 -G Ninja
cmake --build desktop\windows\build-x64
ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests
```

如构建通过，再启动 Windows 应用人工检查：

- 语言设为英文，打开配对设备、设置、热键、固件更新相关窗口。
- 语言设为简体中文，重复检查同一批窗口。
- 未配置设备主题色/位置时，托盘菜单默认选中“自动 / 底部居中”。

## 实施顺序

1. 增加失败测试，锁定本地化表完整性和新默认值行为。
2. 扩展 `StringId` 与中英文表。
3. 改造 `PairDeviceDialog` 接收有效语言并替换硬编码文案。
4. 改造首次引导、热键设置、固件更新、应用层 MessageBox 文案。
5. 调整默认主题色与悬浮窗位置的配置保存过滤逻辑。
6. 运行 Windows 核心测试与构建。
7. 启动应用做基础人工验证。

## 自查结果

- 无 `TODO`、`TBD` 或占位要求。
- 范围聚焦 Windows 端弹窗本地化与默认显示偏好，不包含协议、固件或 macOS。
- 默认值策略明确为“仅影响未显式配置”，不会强制覆盖用户旧偏好。
- 测试覆盖新增文案完整性与配置默认值行为。
