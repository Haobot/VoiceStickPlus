# Windows 设置界面布局整理方案

## 背景与问题

`desktop/windows/src/settings_dialog.cc` 的 `BuildControls()` 用累加 `y` 的方式手动定位每个 Win32 控件。当前布局存在三类问题：

1. **5 处空 label 占位符制造"多余空格"**：第 467、481、492、499、518 行各有一个 `CreateLabel(hwnd_, L"", ...)`，文本为空的右对齐静态控件，纯粹为占用一行空间，是用户感知到的"多余空格"的直接来源。
2. **间距规则混乱**：行距混用 `row_h+Dp(10)` / `row_h+Dp(16)` / `row_h+Dp(20)` / `Dp(80)+Dp(26)` / `Dp(70)`，组与组之间没有统一规则。
3. **逻辑分组不可见**：ASR、LLM 精修、硬件交互、输出、系统这几组设置之间无视觉边界，checkbox 类设置（开机自启/调试音频/IMU 调试/敲击转方向键）散落各处且都靠空 label 隔开。

## 目标

- 删除全部空 label 占位符，消除多余空格。
- 用「分组标题 + 分隔线」呈现分组（已与用户确认）。
- 统一组内行距与组间间距，让逻辑关系一目了然。
- 不改变任何配置字段的读写语义，仅重组视觉布局。

## 分组划分（按逻辑递进）

| 组 | StringId | 包含控件 |
|---|---|---|
| 通用 | `kSettingsSectionGeneral` | 界面语言 |
| 语音识别 | `kSettingsSectionAsr` | 服务提供方、API Key、资源 ID、热词+提示 |
| 文本精修 | `kSettingsSectionRefine` | LLM Base URL、LLM API Key、LLM 模型、启用精修、精修提示词 |
| 输出 | `kSettingsSectionOutput` | 输出目标、微信语音热键、微信虚拟麦克风 |
| 设备交互 | `kSettingsSectionDevice` | 拿起灵敏度、双击方向键、敲击灵敏度、体感鼠标左右/上下灵敏度 |
| 系统 | `kSettingsSectionSystem` | 开机自启、保存调试音频、显示 IMU 调试、音频文件夹 |

顺序逻辑：基础 → ASR 核心 → ASR 后处理 → 结果去向 → 硬件行为 → 系统杂项/调试。

## 实现细节

### 1. 新增本地化字符串（`localization.h` + `localization.cc`）

在 `kSettingsSaveFailed` 之后、托盘菜单段之前，新增 6 个 `StringId`：

| 枚举 | 英文 | 中文 |
|---|---|---|
| `kSettingsSectionGeneral` | General | 通用 |
| `kSettingsSectionAsr` | Speech Recognition | 语音识别 |
| `kSettingsSectionRefine` | Text Refinement | 文本精修 |
| `kSettingsSectionOutput` | Output | 输出 |
| `kSettingsSectionDevice` | Device Interaction | 设备交互 |
| `kSettingsSectionSystem` | System | 系统 |

`localization.cc` 的英文表（`EnglishStrings`）和中文表各加 6 条。`kStringCount` 由枚举末值自动推导，无需手改。

### 2. 加粗字体（`dpi_util.h` + `settings_dialog.h`）

- `dpi_util.h` 新增 `CreateUiFontBold(UINT dpi)`：复用 `CreateUiFont` 的 `NONCLIENTMETRICS` 取值逻辑，在 `CreateFontIndirectW` 前把 `lfWeight` 置为 `FW_BOLD`。
- `settings_dialog.h` 新增成员 `HFONT title_font_ = nullptr;`。
- `BuildControls()` 创建 `title_font_`；`DestroyControls()` 释放（与 `ui_font_` 同处）。
- 字体应用循环：对分组标题控件单独 `WM_SETFONT` 设 `title_font_`，其余控件仍设 `ui_font_`。用一个 `std::vector<HWND> title_controls_` 记录标题控件以便区分。

### 3. 新控件辅助函数（`settings_dialog.cc` 匿名命名空间）

```cpp
// 分组标题：左对齐静态文本，应用加粗字体
HWND CreateSectionTitle(HWND parent, const wchar_t* text, int x, int y, int w, int h, HINSTANCE inst);

// 水平蚀刻分隔线
HWND CreateSeparator(HWND parent, int x, int y, int w, HINSTANCE inst);
```

- `CreateSectionTitle`：`SS_LEFT` STATIC，记入 `title_controls_`（用于设加粗字体），同时记入 `label_controls_`（享受透明背景）。
- `CreateSeparator`：`SS_ETCHEDHORZ` STATIC，高 `Dp(2)`，**不**记入 `label_controls_`（蚀刻线自绘背景，避免被 `WM_CTLCOLORSTATIC` 设成透明而失效），仅记入 `all_controls_`。

### 4. 布局规则统一

引入两个间距常量并贯穿全局：

| 常量 | 值 | 用途 |
|---|---|---|
| `row_h + Dp(10)` | Dp(38) | 组内控件行距 |
| `Dp(24)` | Dp(24) | 分组标题行推进（标题高 20 + 4 间距） |
| `Dp(14)` | Dp(14) | 分隔线推进（线高 2 + 上下间距 12） |

组与组之间固定为：组末控件 → 分隔线(Dp14) → 下一组标题(Dp24) → 首个控件。首组标题上方不再额外加空行（顶部已 `Dp(20)` 起始）。

热词多行块（`Dp(74)` 编辑框 + 提示行）和精修提示词多行块（`Dp(64)` 编辑框）保留原有块内间距，但块后接统一的分隔线。

### 5. 重写 `BuildControls()` 布局

按分组顺序重排，删除全部空 label 占位符。新布局 y 推进（Dp 单位）：

```
y=20
[通用] 标题 → y=44 ; 界面语言 row → y=82 ; 分隔线 → y=96
[语音识别] 标题 → y=120 ; 服务方 →158 ; API Key →196 ; 资源ID →234 ; 热词块 →340 ; 分隔线 →354
[文本精修] 标题 → y=378 ; Base URL →416 ; LLM Key →454 ; 模型 →492 ; 精修cb →530 ; 提示词块 →600 ; 分隔线 →614
[输出] 标题 → y=638 ; 输出目标 →676 ; 微信热键 →714 ; 微信麦克风 →752 ; 分隔线 →766
[设备交互] 标题 → y=790 ; 拿起灵敏度 →828 ; 双击方向键 →866 ; 敲击灵敏度 →904 ; 体感X →942 ; 体感Y →980 ; 分隔线 →994
[系统] 标题 → y=1018 ; 开机自启 →1056 ; 调试音频 →1094 ; IMU调试 →1132 ; 音频文件夹 →1170
按钮行 y=1190（高30）→ 底 1220
```

### 6. 窗口高度

`settings_dialog.h` 的 `kClientHeight` 由 `1160` 调整为 `1240`（容纳 6 标题 + 5 分隔线，净增约 80 Dp；删 5 空 label 抵消部分）。`kClientWidth` 保持 `640`。

### 7. 条件显示控件保持现有行为

- 资源 ID（仅火山引擎）、微信热键/麦克风（仅微信输入法）的 `ShowWindow` 显隐逻辑不变，仍占位推进 `y`，布局稳定不跳动。`UpdateProviderVisibility` / `UpdateOutputTargetVisibility` 无需改动。

## 改动文件清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/dpi_util.h` | 新增 `CreateUiFontBold(UINT dpi)` |
| `desktop/windows/src/localization.h` | 新增 6 个 `kSettingsSection*` 枚举 |
| `desktop/windows/src/localization.cc` | 英/中两表各加 6 条 |
| `desktop/windows/src/settings_dialog.h` | 新增 `title_font_`、`title_controls_` 成员；`kClientHeight` 改 1240 |
| `desktop/windows/src/settings_dialog.cc` | 重写 `BuildControls()`；新增 2 个辅助函数；`DestroyControls` 释放 `title_font_`；字体应用循环区分标题/普通控件 |

## 验证

1. **构建**：`build_win.bat`（自动关旧进程、重建 `build-x64`），核对 `desktop\windows\build-x64\VoiceStick.exe` 时间戳与体积。
2. **回归测试**：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`——本次改动不触及 `voicestick_core`，全部测试应保持通过。
3. **视觉验证**（真机）：运行 `VoiceStick.exe` 打开设置，确认：
   - 6 个分组标题加粗显示、5 条分隔线水平蚀刻；
   - 组内行距统一、组间间距一致、无多余空白行；
   - 切换服务提供方（资源 ID 显隐）、切换输出目标（微信两项显隐）布局不跳动；
   - 高 DPI（150%/200%）下标题、分隔线、控件缩放正确（`WM_DPICHANGED` 走 `RebuildUi`）。

## 风险与注意

- **`SS_ETCHEDHORZ` 分隔线与 `WM_CTLCOLORSTATIC`**：分隔线不加入 `label_controls_`，避免被设透明背景导致蚀刻线消失。这是实现中需核对的关键点。
- **加粗字体生命周期**：`title_font_` 必须在 `DestroyControls` 中释放并在置 `nullptr`，与 `ui_font_` 对称，防止 DPI 切换重建时句柄泄漏。
- **提交**：`desktop/windows/` 被 `.gitignore` 忽略，提交时用 `git add -f`。
- **TDD 适用性**：本次为纯 Win32 手动定位 UI 布局，不在 `voicestick_core` 可测范围内，无对应单元测试可写；遵循项目对 Windows UI 改动的验证约定（构建 + 真机视觉验证），ctest 仅作回归保护。
