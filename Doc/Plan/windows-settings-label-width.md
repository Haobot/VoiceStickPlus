# Windows 设置对话框标签列加宽

## 背景

设置对话框标签列宽度固定 `label_w = Dp(110)`（英文遗留值），标签用 `SS_RIGHT` 右对齐。
中英文长标签文本宽度超过 110 Dp，右对齐下超长部分向左延伸被控件矩形裁剪，显示不全。

实测文本宽度（@96dpi ≈ Dp，含结尾冒号）：

| 标签 | 中文宽 | 英文宽 | 110 够吗 |
|---|---|---|---|
| 拿起灵敏度 | 78 | 95 | ✅ |
| 双击设备按下方向键↓ | 134 | 224 | ❌ |
| 敲击灵敏度（1=重，10=轻） | 180 | 188 | ❌ |
| 体感鼠标左右灵敏度（1=慢，10=快） | 229 | 276 | ❌ |
| 体感鼠标上下灵敏度（1=慢，10=快） | 229 | 274 | ❌ |

最长标签（英文体感鼠标）需约 276 Dp。

## 目标

- 加宽标签列使所有中英文标签完整显示，用户已确认目标 `label_w = Dp(290)`。
- 保持控件右边界与分组标题/分隔线右边界对齐（x=600 Dp 不变）。
- 一处常量改动联动所有行，不破坏现有列对齐与滚动/钉底逻辑。

## 设计

### 布局常量（`settings_dialog.cc` BuildControls，474-476 行）

保持标签 x=10、标签到控件间隙 10、控件右边界 600：

```cpp
const int label_w = Dp(290);                  // 标签列宽，容纳最长标签(体感鼠标~276)+余量
const int ctrl_x = Dp(310);                   // = 10(label_x) + 290 + 10(gap)
const int ctrl_w = Dp(kClientWidth - 350);    // = 640-350=290，控件区 x∈[310,600]
```

联动验证：
- 标签 x∈[10, 300]，间隙 10，控件 x∈[310, 600]。
- `section_title`/`separator` 宽 = `ctrl_x + ctrl_w - Dp(10) = Dp(590)`，与改前一致 ✅。
- `save/cancel` 按钮 x 用 `kClientWidth - 200` / `kClientWidth - 105`，不依赖 ctrl_w，不受影响 ✅。

### ApplyApiKeyLayout 同步（985 行）

`ApplyApiKeyLayout` 内部重新定义 `ctrl_w = Dp(kClientWidth - 170)`，需同步：

```cpp
const int ctrl_w = Dp(kClientWidth - 350);
```

否则 API Key 行的 edit/试用按钮宽度与其他行不一致。

### 控件区变窄影响评估

控件区宽度 470 -> 290 Dp：
- trackbar（`ctrl_w - Dp(50)` = 240）：10 档刻度仍清晰可用 ✅
- combo/edit（`ctrl_w` = 290）：略窄但可接受 ✅
- `debug_dir_edit`（`ctrl_w - Dp(80)` = 210）+ choose_btn（`ctrl_x + ctrl_w - Dp(75)`）✅
- `api_key_edit` 显示试用按钮时 290-102-8=180 ✅

## 改动文件清单

| 文件 | 改动 |
|---|---|
| `desktop/windows/src/settings_dialog.cc` | BuildControls 三常量（474-476）；ApplyApiKeyLayout 的 ctrl_w（985） |

## 验证

1. **构建**：`build_win.bat`，核对 exe 时间戳与体积（防假成功）。
2. **回归**：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`。
3. **视觉验证**（真机）：
   - 中英文下"敲击灵敏度""体感鼠标左右/上下灵敏度""双击设备按下方向键↓"标签完整显示。
   - 滑块/combo/edit 在变窄的控件区仍可用，右边界与标题对齐。
   - 100% 与高 DPI 下标签均不截断。

## 风险

- **控件区变窄**：470->290 Dp，对滑块和 combo 可用度略降但可接受。若真机发现控件过窄，可下调 label_w（如 260）平衡。
- **SS_RIGHT 裁剪特性**：STATIC `SS_RIGHT` 在控件矩形内绘制，超长向左裁剪。加宽控件矩形宽度即可容纳，已确认。
