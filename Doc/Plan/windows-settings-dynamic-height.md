# Windows 设置界面动态高度方案

## 背景

上一轮把设置界面整理为 6 分组（标题 + 分隔线）布局，消除了空 label 占位符。但仍有「为条件选项预留的空白」：三行条件控件在隐藏时仅 `ShowWindow(SW_HIDE)`，仍占位推进 `y`，留下空白行。

| 条件行 | 显示条件 | 隐藏时留白位置 |
|---|---|---|
| 资源 ID（label + combo） | 服务提供方 = Volcengine | 语音识别组内，API Key 与热词之间 |
| 微信语音热键（label + edit） | 输出目标 = 微信输入法 | 输出组内，输出目标之后 |
| 微信虚拟麦克风（label + edit） | 输出目标 = 微信输入法 | 同上，热键之后 |

默认配置（VoiceStick Cloud + 当前应用）下共 3 处空白行。用户要求动态调整高度消除空白。

`apply_trial_button` 是**行内**条件（与 `api_key_edit` 同行），不占独立行，不影响行高——保持现有 resize 逻辑，不纳入行级动态布局。

## 目标

- 条件行隐藏时不占位，下方内容上移。
- 窗口高度随可见行数动态收缩（顶部不动，底部伸缩）。
- 切换服务提供方 / 输出目标时实时重排，无闪烁。
- 不改变配置读写语义，不改变 `apply_trial_button` 行内显隐与 `api_key_edit` 宽度逻辑。

## 设计：声明式布局表 + Relayout()

### 1. 布局模型（`settings_dialog.h`）

引入布局条目，把"行/块"抽象为可独立显隐的单元：

```cpp
// 一个控件在行内的相对定位：x、相对行基线的 y 偏移、宽、高。
struct LayoutPart {
    HWND control;
    int x;
    int y_off;
    int w;
    int h;
};

// 一个布局条目 = 一行或一个多行块，含若干控件与可见性谓词。
struct LayoutEntry {
    int advance;                         // 该项可见时推进的 y（Dp 换算后）
    std::vector<LayoutPart> parts;       // 该项的控件
    std::function<bool()> visible;       // 空 = 始终可见
};

std::vector<LayoutEntry> layout_;
```

新增成员：`layout_`、按钮区 `save_button_`/`cancel_button_` 句柄（用于 Relayout 末尾定位）。

### 2. BuildControls 改造

控件创建逻辑不变，但创建时位置传占位（x=y=0），定位统一交给 `Relayout()`。每个逻辑行/块创建完控件后，用辅助 lambda 注册进 `layout_`：

```cpp
auto add_entry = [&](int advance, std::vector<LayoutPart> parts,
                     std::function<bool()> vis = {}) {
    layout_.push_back({advance, std::move(parts), std::move(vis)});
};
```

示例（语言行，始终可见）：
```cpp
HWND lang_label = remember_label(CreateLabel(hwnd_, ..., 0, 0, label_w, Dp(20), instance_));
language_combo_ = remember(CreateCombo(hwnd_, 0, 0, ctrl_w, Dp(140), kIdLanguageCombo, instance_));
// 填充选项...
add_entry(row_h + Dp(10), {
    {lang_label,   Dp(10),  Dp(3), label_w, Dp(20)},
    {language_combo_, ctrl_x, 0,    ctrl_w,  Dp(140)},
});
```

示例（资源 ID 行，条件可见）：
```cpp
add_entry(row_h + Dp(10), {
    {resource_label_, Dp(10), Dp(3), label_w, Dp(20)},
    {resource_combo_, ctrl_x, 0, ctrl_w, Dp(200)},
}, [this]() {
    int idx = static_cast<int>(SendMessageW(provider_combo_, CB_GETCURSEL, 0, 0));
    return idx == 1;  // Volcengine
});
```

分组标题、分隔线也注册为始终可见的 entry（`advance` 分别为 `title_h+Dp(4)`、`sep_h+Dp(12)`），这样条件行隐藏时其后的标题/分隔线自然上移。

热词块（label + 多行 edit + hint）作为一个 entry：`advance = Dp(80)+Dp(26)`，parts 含三个控件（label y_off=Dp(3) h20、edit y_off=0 h74、hint y_off=Dp(80) h16）。
精修提示词块（label + edit）作为一个 entry：`advance = Dp(70)`。

按钮（保存/取消）**不进 layout_**，由 Relayout 末尾单独定位在内容下方。

BuildControls 末尾调用 `Relayout()` 做初始定位（此时 combo 为默认选择，随后 LoadConfigIntoControls 会再次触发 Relayout 修正）。

### 3. Relayout() 实现

```cpp
void SettingsDialog::Relayout() {
    if (!hwnd_) return;
    int y = Dp(20);  // 顶部起始
    for (const auto& entry : layout_) {
        const bool vis = !entry.visible || entry.visible();
        const int cmd = vis ? SW_SHOW : SW_HIDE;
        for (const auto& p : entry.parts) {
            if (!p.control) continue;
            ShowWindow(p.control, cmd);
            if (vis) {
                SetWindowPos(p.control, nullptr, p.x, y + p.y_off, p.w, p.h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        if (vis) y += entry.advance;
    }
    // 按钮区
    const int btn_w = Dp(80), btn_h = Dp(30);
    const int btn_y = y + Dp(20);
    SetWindowPos(save_button_, nullptr, Dp(kClientWidth - 200), btn_y, btn_w, btn_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(cancel_button_, nullptr, Dp(kClientWidth - 105), btn_y, btn_w, btn_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    const int client_h = btn_y + btn_h + Dp(20);
    ResizeWindow(client_h);
}
```

### 4. 窗口高度动态调整

新增 `ResizeWindow(int client_h)`，复用 `RebuildUi` 既有的 `AdjustWindowRectExForDpi` 模式：

```cpp
void SettingsDialog::ResizeWindow(int client_h) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    RECT desired{0, 0, Dp(kClientWidth), client_h};
    AdjustWindowRectExForDpi(&desired, style, FALSE, ex_style, dpi_);
    SetWindowPos(hwnd_, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}
```

- `SWP_NOMOVE`：顶部不动，底部随高度伸缩。
- `kClientHeight` 常量保留为「全部显示时的最大高度」参考，但实际高度由 Relayout 动态计算。`RebuildUi`（DPI 切换时）仍用它作初始尺寸，随后 `BuildControls`→`Relayout` 收敛到实际高度。

### 5. 条件显隐触发点改造

| 函数 | 原逻辑 | 新逻辑 |
|---|---|---|
| `UpdateProviderVisibility` | 直接 `ShowWindow(resource_*)` + apply_trial 显隐 + api_key 宽度 | 移除对 `resource_*` 的 ShowWindow（交 Relayout）；保留 apply_trial 显隐 + api_key 宽度；末尾调 `Relayout()` |
| `UpdateOutputTargetVisibility` | 直接 `ShowWindow(wechat_*)` | 移除全部 ShowWindow（交 Relayout）；末尾调 `Relayout()` |

Relayout 内部读 combo 当前选择判定可见性，因此 `Update*Visibility` 不再需要手动 ShowWindow 条件行，只保留行内 apply_trial/api_key 逻辑。`api_key_edit` 的宽度调整在 Relayout 之后执行（`UpdateProviderVisibility` 先调 `Relayout()` 再 resize api_key），确保位置已更新。

### 6. 资源释放

`DestroyControls` / `WM_DESTROY` 中 `layout_.clear()`，与 `all_controls_` 等同步。

## 调用时序

```
WM_INITDIALOG
  → RebuildUi
    → DestroyControls (清空 layout_)
    → BuildControls (创建控件 + 注册 layout_ + Relayout 初始定位)
    → LoadConfigIntoControls (设各 combo 选择 → Update*Visibility → Relayout 修正)
  → 居中窗口（取 Relayout 后的真实尺寸）
运行时切换 provider/output → HandleMessage → Update*Visibility → Relayout 实时重排
WM_DPICHANGED → RebuildUi → 同上重建
```

## 改动文件清单

| 文件 | 改动 |
|---|---|
| `settings_dialog.h` | 新增 `LayoutPart`/`LayoutEntry` 结构、`layout_` 成员、`save_button_`/`cancel_button_` 句柄、`Relayout()`/`ResizeWindow()` 声明 |
| `settings_dialog.cc` | BuildControls 改为创建+注册；新增 `Relayout()`/`ResizeWindow()`；改造 `UpdateProviderVisibility`/`UpdateOutputTargetVisibility`；`DestroyControls`/`WM_DESTROY` 清空 `layout_` |

## 验证

1. **构建**：`build_win.bat`，核对 `VoiceStick.exe` 时间戳与体积（7.32 MB 量级）。
2. **回归**：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`，`voicestick_core` 测试保持通过（本改动不触及 core）。
3. **视觉验证**（真机）：
   - 默认配置（Cloud + 当前应用）：语音识别组无资源 ID 空白行，输出组无微信两空白行，窗口高度较前收缩；
   - 切换服务提供方到 Volcengine：资源 ID 行出现，下方内容下移，窗口增高；切回 Cloud：资源 ID 行消失，上方收拢，窗口收缩；
   - 切换输出目标到微信输入法：微信热键/虚拟麦克风两行出现；切回当前应用：两行消失收拢；
   - 切换过程中无闪烁、无重叠、按钮始终在内容下方；
   - 高 DPI（150%）下切换正常，`WM_DPICHANGED` 重建后布局正确。

## 风险与注意

- **combo 下拉高度**：`CreateCombo` 的 h 参数（Dp(140)/Dp(200)）是下拉列表高度，Relayout 时 SetWindowPos 用同值，不影响显示。保持一致即可。
- **api_key_edit 宽度时序**：必须 `Relayout()` 之后再 resize api_key，否则 Relayout 的 SetWindowPos 会覆盖宽度。`UpdateProviderVisibility` 内顺序：先 Relayout，再 resize api_key。
- **窗口顶部固定**：`SWP_NOMOVE` 保证高度变化时顶部不动，避免窗口整体跳动；初始居中后底部伸缩。
- **首次 Relayout 时机**：BuildControls 末尾调一次（combo 默认选择），LoadConfig 设 combo 后 Update*Visibility 再调一次修正，两次都在 WM_INITDIALOG 内完成，用户不可见中间态。
- **TDD 适用性**：纯 Win32 手动定位 UI，不在 `voicestick_core` 可测范围，无单测可写；遵循项目对 Windows UI 改动的验证约定（构建 + 真机视觉验证），ctest 仅作回归保护。
