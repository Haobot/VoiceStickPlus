# Windows 悬浮窗玻璃态毛玻璃显示优化方案

## Context

当前 Windows 端悬浮窗背景偏不透明，识别文字虽然清晰，但视觉上不像系统级玻璃态浮层。用户希望背景具备毛玻璃透明效果，同时识别文字仍然保持清晰、稳定、可读。

现有实现位于：

- `desktop/windows/src/overlay_window.cc`
- `desktop/windows/src/overlay_window.h`
- `desktop/windows/src/subtitle_window.cc`
- `desktop/windows/src/subtitle_window.h`

当前 `OverlayWindow` 和 `SubtitleWindow` 都使用 `WS_EX_LAYERED` + `UpdateLayeredWindow`，通过 GDI+ 绘制圆角背景/阴影，通过 Direct2D/DirectWrite 绘制文字。这个路径适合像素级 alpha 和清晰文字，但与 DWM Acrylic/Blur 直接叠加兼容性不稳定，尤其在 Windows 10/11、远程桌面、不同显卡驱动和透明效果开关下表现可能不一致。

目标不是一次性重写 UI，而是在保留现有文字绘制、动画、DPI、点击穿透逻辑的基础上，引入可降级的玻璃态背景。

## 推荐方案

采用“双窗口分层”方案：

1. **玻璃背景窗口**：新增一个专门负责毛玻璃/亚克力背景的 Win32 窗口。
2. **内容绘制窗口**：保留现有 `OverlayWindow` layered window，只绘制文字、指示器、提示文本和必要高光/边框。

这样可以避免 DWM blur 影响文字渲染；文字仍由现有 DirectWrite 离屏绘制并通过 `UpdateLayeredWindow` 输出，保证清晰度。

## 设计细节

### 1. 新增玻璃背景窗口封装

新增一个轻量类，例如：

- `desktop/windows/src/glass_backdrop_window.h`
- `desktop/windows/src/glass_backdrop_window.cc`

职责：

- 创建 `WS_POPUP` 窗口，扩展样式使用：
  - `WS_EX_TOPMOST`
  - `WS_EX_TOOLWINDOW`
  - `WS_EX_NOACTIVATE`
  - `WS_EX_TRANSPARENT`
- 不使用 `WS_EX_LAYERED` 作为首选路径，避免与 DWM backdrop/blur 冲突。
- 跟随内容窗口的位置、大小、显示/隐藏、DPI 和动画尺寸变化。
- 提供接口：
  - `Show(const RECT& bounds, BYTE opacity_or_mode)`
  - `Move(const RECT& bounds)`
  - `Hide()`
  - `SetCornerRadius(int radius)`
  - `SetTheme(OverlayThemeColor color)`

### 2. Windows 11 优先使用 DWM System Backdrop

在 Windows 11 上优先尝试：

- `DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ...)`
- `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, ...)`
- 必要时配合 `DWMWA_BORDER_COLOR` / `DWMWA_TEXT_COLOR` / `DWMWA_CAPTION_COLOR` 设置透明或不抢眼的边框/标题色。

推荐 backdrop 类型：

- 优先 `DWMSBT_TRANSIENTWINDOW` 或接近浮层语义的 backdrop。
- 若视觉效果不符合预期，再尝试 acrylic 类常量，但要保持运行时能力检测和失败回退。

要求：

- DWM attribute 常量要做运行时兼容，避免在旧 Windows SDK/Windows 10 上硬崩。
- API 失败时不能影响悬浮窗显示，应回退到现有半透明背景。

### 3. Windows 10 使用 Accent Blur/Acrylic 作为可选路径

在 Windows 10 上可尝试 `SetWindowCompositionAttribute`：

- `ACCENT_ENABLE_BLURBEHIND`
- 或 `ACCENT_ENABLE_ACRYLICBLURBEHIND`

注意：

- `SetWindowCompositionAttribute` 是未正式文档化 API，只能作为 best-effort。
- 必须动态加载 `user32.dll` 中的函数地址，失败即回退。
- 不要把它和现有内容 layered window 合并使用；只应用在新增背景窗口上。

### 4. 内容窗口保留现有 DirectWrite 清晰文字路径

保留 `OverlayWindow::PaintText()` 的 DirectWrite 渲染方式。

调整 `OverlayWindow::PaintContent()`：

- 背景填充从当前高 alpha 实心圆角底板改为更轻的 scrim/高光层。
- `kBackgroundAlpha = 219` 不再作为主背景不透明度使用。
- 建议新增视觉常量：
  - `kGlassScrimAlpha = 52–88`
  - `kGlassBorderAlpha = 48–72`
  - `kGlassHighlightAlpha = 32–56`
  - `kTextShadowAlpha = 80–120`
- 文字颜色建议调整为更高对比：
  - 主浮窗文字：接近黑色或白色由主题决定，但 alpha 接近 240–255。
  - 如果背景复杂，给文字加 1px 柔和阴影或轻描边。

对于当前白色主题，建议：

- 背景玻璃偏浅，文字继续使用深色 `kInkRgb`，提高 `kTextAlpha` 到 `235–245`。
- 对深色桌面或字幕模式，保留深色 scrim 和白色文字。

### 5. `OverlayWindow` 集成方式

在 `OverlayWindow` 中新增成员：

- `std::unique_ptr<GlassBackdropWindow> backdrop_;`

生命周期：

- 构造时创建 backdrop。
- `Reposition()` / `ApplyAnimatedWindowBounds()` 更新内容窗口位置时，同步移动 backdrop。
- `Show()` / `StartFadeIn()` 显示 backdrop。
- `StartFadeOut()` / `Hide()` 隐藏 backdrop。
- `OnDpiChanged()` 后同步圆角、bounds。

Z-order：

- backdrop 在内容 layered window 下方。
- 两者都保持 topmost/noactivate/transparent。
- 显示时先 show backdrop，再 show 内容窗口，避免内容被盖住。

动画：

- 第一版可以不做 backdrop alpha 动画，只跟随内容窗口 bounds。
- 如果视觉上突兀，再增加 `SetLayeredWindowAttributes` 或 DWM tint 参数的渐变；不要先引入复杂动画。

### 6. `SubtitleWindow` 集成策略

字幕窗比主浮窗更强调可读性，建议第二阶段再做或采用保守玻璃化：

- 保留每条 lane 的半透明深色底板，但降低不透明度。
- 可在整个字幕窗口后方添加一个共享 `GlassBackdropWindow`。
- 每条 lane 继续绘制轻量 scrim，保证多行文字在复杂背景上可读。

第一阶段优先改 `OverlayWindow`，确认视觉和兼容性后再推广到 `SubtitleWindow`。

## 边界与降级

必须保留当前 GDI+ 半透明圆角背景作为 fallback：

- DWM 不可用。
- 远程桌面或透明效果关闭。
- `DwmSetWindowAttribute` / `SetWindowCompositionAttribute` 失败。
- 用户显卡/系统版本表现异常。

建议增加内部枚举：

- `GlassBackdropMode::kSystemBackdrop`
- `GlassBackdropMode::kAccentBlur`
- `GlassBackdropMode::kFallbackLayeredScrim`

第一版不必暴露配置项；如果兼容性反馈不好，再在设置里增加“启用玻璃态悬浮窗”开关。

## 实施步骤

1. 新增 `GlassBackdropWindow` 类，封装 DWM/system backdrop/accent blur 初始化和 show/move/hide。
2. 在 `OverlayWindow` 中持有并同步 backdrop 的生命周期、bounds、DPI、显示隐藏。
3. 调整 `OverlayWindow::PaintContent()` 的背景绘制：从不透明底板改为轻量 scrim、边框、高光和阴影。
4. 调整 `OverlayWindow::PaintText()` 的文字对比度：提高 alpha，必要时添加轻量文字阴影。
5. 手动验证主浮窗效果稳定后，再决定是否把同一 backdrop 方案推广到 `SubtitleWindow`。

## 验证计划

### 构建与测试

```powershell
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cmake --build desktop\windows\build-x64 --target VoiceStickApp voicestick_windows_tests'
cmd /c 'call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && ctest --test-dir desktop\windows\build-x64 --output-on-failure -R voicestick_windows_tests'
```

### 手动 UI 验证

1. 启动 `desktop\windows\build-x64\VoiceStick.exe`。
2. 在浅色桌面、深色桌面、复杂壁纸、视频背景前分别触发：
   - listening
   - partial text
   - final countdown
   - paused final
   - error
3. 检查：
   - 背景有毛玻璃/透明质感。
   - 文字不被模糊。
   - 文字在复杂背景上仍清晰。
   - 圆角边缘无黑边、白边、残影。
   - 淡入淡出和尺寸动画无明显错位。
   - 点击穿透仍正常。
   - 多 DPI / 缩放切换后位置和圆角正常。
4. 在 Windows 10 与 Windows 11 分别验证。
5. 如能覆盖，额外验证远程桌面或关闭系统透明效果后的 fallback。

## 风险

- DWM Acrylic/Blur 与传统 Win32 window 组合存在系统版本差异，必须 runtime fallback。
- 双窗口方案需要仔细维护 Z-order 和同步移动，否则可能出现背景与文字错位。
- 毛玻璃过强会降低文字可读性；第一版应偏保守，先用轻玻璃 + scrim，而不是追求极强模糊。
- `SubtitleWindow` 文本常驻时间更长，过度透明可能影响阅读；建议在主浮窗验证后再推广。
