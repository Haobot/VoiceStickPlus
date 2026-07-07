# Windows 设置界面垂直滚动条方案

## 背景

上一轮实现动态高度后，窗口高度 = 内容高度。当内容全展开（勾选精修 + 微信输入法 + Volcengine 资源 ID）或高 DPI 时，内容高度超过屏幕工作区高度，窗口底部被截断，无法访问保存/取消按钮。

估算（96dpi 全展开）：通用 62 + 语音识别 244 + 文本精修 246 + 输出 138 + 设备交互 214 + 系统 186 + 按钮 70 ≈ 1050 Dp。125% DPI 下 ≈ 1312px，150% 下 ≈ 1575px，均超 1080p 工作区（~1040px）。

## 目标

- 窗口高度上限 = 屏幕工作区高度 - 边距，保证完整可见。
- 内容超过可视高度时启用垂直滚动条，滚动浏览全部设置。
- 保存/取消按钮钉在窗口底部，始终可点（不随内容滚动）。
- 内容不超高时窗口紧凑（无禁用滚动条占位以外的额外空间），滚动条自动禁用。
- 不破坏现有动态高度与条件显隐逻辑。

## 设计

### 1. 窗口样式（`BuildDialogTemplate`）

style 加 `WS_VSCROLL`：

```cpp
dialog_template.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | WS_VSCROLL;
```

`WS_VSCROLL` 的标准滚动条位于非客户区右侧，**客户区宽度恒定 = kClientWidth（640）**，现有所有控件 x 定位（10..600）无需改动，不与滚动条重叠。

### 2. 新增成员（`settings_dialog.h`）

```cpp
int scroll_pos_ = 0;  // 垂直滚动位置（像素，Dp 换算后）
```

### 3. Relayout 改造：计算高度上限 + 滚动范围 + 偏移定位

```cpp
void SettingsDialog::Relayout() {
    if (!hwnd_) return;

    // 1. 累加可见条目得到逻辑内容高度（不含按钮区）。
    int content_h = Dp(20);  // 顶部起始
    for (const auto& entry : layout_) {
        if (!entry.visible || entry.visible()) content_h += entry.advance;
    }

    // 2. 按钮区高度（顶部间距 + 按钮 + 底部间距）。
    const int btn_h = Dp(30);
    const int btn_area = Dp(20) + btn_h + Dp(20);  // Dp(70)

    // 3. 窗口高度上限 = 屏幕工作区高度 - 边距；自然高度 = 内容 + 按钮区。
    RECT work = GetWorkAreaForWindow(hwnd_);
    const int max_visible = (work.bottom - work.top) - Dp(40);
    const int natural_h = content_h + btn_area;
    const int client_h = std::min(natural_h, max_visible);

    // 4. 内容可视高度 = 客户区 - 按钮区；滚动范围。
    const int content_area_h = client_h - btn_area;
    const int scroll_range = std::max(0, content_h - content_area_h);
    scroll_pos_ = std::clamp(scroll_pos_, 0, scroll_range);

    // 5. 设置滚动条信息：nPage>=nMax+1 时滚动条自动禁用但仍占位。
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = content_h - 1;       // 超高时真实范围；不超高时 nPage 覆盖使其禁用
    si.nPage = static_cast<UINT>(content_area_h);
    si.nPos = scroll_pos_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);

    // 6. 窗口高度。
    ResizeWindow(client_h);

    // 7. 内容控件按滚动偏移定位。
    int y = Dp(20) - scroll_pos_;
    for (const auto& entry : layout_) {
        const bool vis = !entry.visible || entry.visible();
        for (const auto& p : entry.parts) {
            if (!p.control) continue;
            if (vis) {
                SetWindowPos(p.control, nullptr, p.x, y + p.y_off, p.w, p.h,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (!p.defer_visibility) ShowWindow(p.control, SW_SHOW);
            } else if (!p.defer_visibility) {
                ShowWindow(p.control, SW_HIDE);
            }
        }
        if (vis) y += entry.advance;
    }

    // 8. 按钮钉底：y = client_h - btn_h - Dp(20)。
    //    不超高时 client_h=natural_h=content_h+btn_area，代入得 y=content_h+Dp(20)，
    //    即紧跟内容下方；超高时钉在窗口底部。两种情况统一。
    const int btn_w = Dp(80);
    const int btn_y = client_h - btn_h - Dp(20);
    if (save_button_) {
        SetWindowPos(save_button_, nullptr, Dp(kClientWidth - 200), btn_y, btn_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(save_button_, SW_SHOW);
    }
    if (cancel_button_) {
        SetWindowPos(cancel_button_, nullptr, Dp(kClientWidth - 105), btn_y, btn_w, btn_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(cancel_button_, SW_SHOW);
    }
    ApplyApiKeyLayout();
}
```

要点：
- `SIF_DISABLENOSCROLL`：内容不超高时滚动条显示为禁用态仍占空间，避免显示/隐藏切换导致客户区宽度跳变（WS_VSCROLL 标准滚动条在非客户区，宽度恒定，但禁用态更明确）。
- 按钮 y 公式统一覆盖两种情形（钉底 vs 紧跟内容），无需分支。
- `scroll_pos_` 在 Relayout 内 clamp 到新范围，DPI 切换或条件显隐导致内容变矮时自动收敛。

### 4. 滚动消息处理（`HandleMessage` 新增 case）

**WM_VSCROLL**：

```cpp
case WM_VSCROLL: {
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    GetScrollInfo(hwnd_, SB_VERT, &si);
    const int prev = si.nPos;
    switch (LOWORD(w_param)) {
        case SB_LINEUP:       si.nPos -= Dp(20); break;
        case SB_LINEDOWN:     si.nPos += Dp(20); break;
        case SB_PAGEUP:       si.nPos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN:     si.nPos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: si.nPos = si.nTrackPos; break;
        case SB_TOP:          si.nPos = si.nMin; break;
        case SB_BOTTOM:       si.nPos = si.nMax; break;
    }
    si.fMask = SIF_POS;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    GetScrollInfo(hwnd_, SB_VERT, &si);
    if (si.nPos != prev) {
        scroll_pos_ = si.nPos;
        Relayout();
    }
    return TRUE;
}
```

**WM_MOUSEWHEEL**（滚轮 → 像素滚动）：

```cpp
case WM_MOUSEWHEEL: {
    const int delta = GET_WHEEL_DELTA_WPARAM(w_param);
    // 系统滚轮行数（默认 3），一格 = 行数 × 一行高度（row_h + 行距 = Dp(38)）。
    UINT wheel_lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheel_lines, 0);
    const int dy = (delta * static_cast<int>(wheel_lines) * Dp(38)) / WHEEL_DELTA;
    // delta 正值 = 向前滚 = 内容上移 = scroll_pos 减少。
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    GetScrollInfo(hwnd_, SB_VERT, &si);
    const int prev = si.nPos;
    si.nPos -= dy;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    GetScrollInfo(hwnd_, SB_VERT, &si);
    if (si.nPos != prev) {
        scroll_pos_ = si.nPos;
        Relayout();
    }
    return TRUE;
}
```

滚动实现选型：用 `Relayout()` 重定位所有控件（~40 个 SetWindowPos），而非 `ScrollWindowEx`。原因：控件数少，重定位简单可靠、无 invalidate 闪烁；STATIC 控件透明背景在 BTNFACE 上重定位不产生残留。性能在设置对话框场景完全够用。

### 5. 需要包含/已有的依赖

- `GetWorkAreaForWindow`（dpi_util.h，已有）
- `SystemParametersInfoW`、`GET_WHEEL_DELTA_WPARAM`、`SCROLLINFO`/`SetScrollInfo`/`GetScrollInfo`（Windows.h，已包含）
- `std::clamp`/`std::min`/`std::max`（`<algorithm>`，已包含）

## 改动文件清单

| 文件 | 改动 |
|---|---|
| `settings_dialog.h` | 新增 `scroll_pos_` 成员 |
| `settings_dialog.cc` | `BuildDialogTemplate` style 加 `WS_VSCROLL`；`Relayout` 改为算上限+滚动范围+偏移定位+钉底按钮；`HandleMessage` 新增 `WM_VSCROLL`/`WM_MOUSEWHEEL` |

## 验证

1. **构建**：`build_win.bat`，核对 exe 时间戳与体积。
2. **回归**：`ctest --test-dir desktop\windows\build-x64 --output-on-failure`，core 测试保持通过。
3. **视觉验证**（真机，优先 150% DPI 复现超高）：
   - 默认状态：窗口高度 = min(内容, 工作区-边距)；若内容不超高，滚动条禁用态、窗口紧凑；
   - 全展开（勾选精修 + 输出→微信输入法 + 服务方→Volcengine）：窗口高度 = 工作区-边距，滚动条激活，可拖动/滚轮/翻页滚动浏览全部设置，按钮始终钉底可见；
   - 滚动到底：内容底部紧贴按钮上方；
   - 滚轮方向正确（向前滚=向上看上方内容）；
   - 切换条件显隐（如取消精修勾选）后内容变矮，scroll_pos 自动收敛，窗口收缩；
   - 100% DPI 下默认内容不超高时无禁用滚动条外的多余空白。

## 风险与注意

- **客户区宽度恒定**：WS_VSCROLL 滚动条在非客户区，客户区宽度恒 = kClientWidth，控件 x 定位不受影响。验证时确认控件右边界（~600）不被滚动条（~640 右侧）遮挡。
- **`SIF_DISABLENOSCROLL`**：保证滚动条始终占位，避免显示/隐藏切换时客户区宽度跳变。若不希望禁用态滚动条可见，可改用动态 ShowScrollBar，但会引入宽度跳变，不采用。
- **按钮钉底 vs 随内容滚**：选钉底（保存/取消始终可点）。代价是超高时内容区底部与按钮间距固定，但内容最后一行 advance 已含尾部间距，视觉不贴紧。
- **WM_MOUSEWHEEL 精度**：用 `delta * wheel_lines * Dp(38) / WHEEL_DELTA`，高精度触控板连续小 delta 也能平滑滚动。`wheel_lines` 取系统设置，尊重用户偏好。
- **DPI 切换**：`WM_DPICHANGED` → `RebuildUi` → `Relayout`，scroll_pos 已 clamp 到新范围，无需额外处理。
- **TDD 适用性**：纯 Win32 UI 滚动行为，不在 core 可测范围，遵循项目 Windows UI 改动验证约定（构建 + 真机视觉验证）。
