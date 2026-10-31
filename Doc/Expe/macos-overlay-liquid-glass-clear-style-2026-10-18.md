# macOS 悬浮窗液态玻璃定稿：.clear 控件级通透折射（含 `_subvariant` KVC 反噬、锁屏 backdrop 门控、双层窗 probe 方法）

- 日期：2026-10-18 定稿（实验跨度 2026-10-16 ~ 10-18，三轮用户验收迭代）
- 相关文件：`desktop/macos/Sources/VoiceStickApp/OverlayController.swift`、`StatusController.swift`、`desktop/macos/Sources/VoiceStickCore/OverlayEnums.swift`
- 相关 commit：功能分支 `feat/overlay-liquid-glass` 六个提交（ecf200c / 3a1d021 / d4f594f / fbf0129 / dbd0f8f / 19edf68），squash 进 main = `5d90aa8`

## 症状（三轮验收反馈演进）

悬浮窗（borderless NSPanel，.screenSaver level、ignoresMouseEvents、clear 背景）要做 macOS 26 原生「液态玻璃」背景：

1. 初版（ecf200c，NSGlassEffectView + 白雾 tint）→ 「不像系统菜单」。
2. 改 `NSVisualEffectView(material: .menu)`（3a1d021）→ 「通透感不够」，滑杆拉到 3% 仍然闷。
3. 回归 NSGlassEffectView 零配置（fbf0129）+ 修 `_subvariant` 反噬后 → 「有改善，但参考图（Apple Park 控件级液态玻璃：高通透+折射+边光）那种系统原生效果还不够」。

## 判据（下次同类问题快速识别）

- **「不通透」先怀疑材质本身而不是参数**：VEV 的 `.menu`/`.popover`/`.hudWindow` 都是固定性格磨砂（深 slate 色块，壁纸色透不出）；真系统菜单压浅色壁纸时壁纸色透出为柔和色斑。同屏对拍一眼可判。
- ** backdrop 模糊强度量化**：玻璃层 CABackdropLayer 的 `glassBackground` filter 有 `scale` 属性，菜单实测 0.5（强模糊）；若发现自己 app 里掉到 0.25（纹理保留率 ~68%、压文字隐约可读）＝有人 KVC 写了 `_subvariant`。
- **锁屏下一切玻璃观感评估无效**：WindowServer 门控 backdrop 采样，玻璃渲染成全黑色块。判据：截图里玻璃条是纯黑底而不是透出背景。

## 根因（三层叠加）

1. **材质选错**：NSVisualEffectView 任何 material 都无背景自适应（第一轮「不像菜单」的根源）。
2. **`_subvariant="menu"` KVC 反噬**（第二轮「发虚不够糊」的根源）：真菜单的 NSGlassEffectView `_subvariant=menu` 是 **SwiftUI 渲染管线自设的结果**，不是外部开关。外部 KVC 写入触发错误重建路径，backdrop 采样 `scale` 从 0.5 掉到 0.25。variantprobe 单变量分项实测：零配置 0.5、`_adaptiveAppearance=1` 单设 0.5、`_subvariant=menu` 单设 **0.25**、设回 nil 恢复 0.5。`_scrimState=1` 同样掉 0.25（排除）。
3. **`style` 默认 `.regular` 是菜单性格不是控件性格**（第三轮「不够参考图」的根源）：glasslab3 双层窗受控亮背景矩阵对拍——`.regular` 把 Sonoma 壁纸糊成柔斑色块；`style = .clear` 才是高通透+边光折射的控件性格。`_contentLensing`、`_tintOpacityReduced`、圆角变化等其余私有/公开旋钮逐项实测均无肉眼可见收益。

## 修复（定稿配置，main `5d90aa8`）

方案 A `nativeLiquid` 定稿（macOS 26+ 路径）：

- `NSGlassEffectView` + **`effect.style = .clear`**（公开 API，控件级通透折射的关键）。
- 内容宿主迁入 `NSGlassEffectView.contentView`（官方契约；真 NSMenu 解剖证实菜单项也在玻璃 contentView 内，同构）。
- **窗口级 WindowServer 投影**（`window.hasShadow = true`，真 NSPopupMenuWindow 实测 hasShadow=true），替代手绘投影层；尺寸步进动画后 `invalidateShadow()`。
- 圆角 **12**（真菜单玻璃实测 cornerRadius=12；浮窗原 20）。
- KVC `_adaptiveAppearance = 1`（真菜单实测值，默认实例为 2；逐项探 setter 存在才写）。
- tint 霜化默认 **0.10**：同明暗族基底（白字主题叠黑霜/黑字主题叠白霜/彩色主题叠主题色霜）——修「调透明度像调亮度」（白霜叠深玻璃会被读成调亮度）。
- macOS <26 回退：VEV `.menu` 材质 + tintView 霜化层 + 手绘投影。
- 实验 UI（托盘三选一的子菜单 + 不透明度滑杆）下线；B/C 方案（specular/darkMirror）保留代码路径，仅 UserDefaults 隐藏键 `overlay.glassStyle`/`overlay.glassTintAlpha` 兜底。

## 验证

- 每轮：`swift build` + `swift run VoiceStickTests` 1033/1033 绿 → 重启 app → AX 触发预览实拍。
- 第一轮对齐后：真 NSMenu 同屏对拍（AX 点开真托盘菜单 vs 浮窗），菜单解剖 ground truth 落盘。
- 第二轮修复后：backdrop scale 回 0.5，浮窗压文字糊到不可读（与菜单行为一致）。
- 第三轮定稿：glasslab3 矩阵（行 2–7 clear 系 vs 行 1/8 regular 系差距一目了然）+ 浮窗实拍（深色 Finder 上文件列表行透出玻璃、上缘边光、白字清晰）。
- 收尾：AX 枚举托盘菜单确认「悬浮窗玻璃方案」已消失。
- 未验证项：**亮背景下白字可读性未实测**（probe 里标签未渲染，浮窗实拍均压深色窗口）。

## 长期技术记忆 / 经验

1. **NSGlassEffectView 风格速查**（macOS 26+）：`.regular`（默认）= 菜单/面板性格，强磨砂；`.clear` = 控件性格，高通透+边缘折射。要「Apple 宣传图那种玻璃」一律先切 `.clear`。
2. **私有属性 KVC 纪律**：`_subvariant`/`_scrimState` 会反噬 backdrop scale；菜单/控件的私有属性读数大多是**渲染结果而非配置入口**，外部写入走另一条重建路径。纪律：只写实测过单项无害的（本次只有 `_adaptiveAppearance=1` 过关），写前 `responds(to:)` 探测。
3. **真菜单解剖 ground truth**（macOS 26 darwin27 实测，时效性声明：引用前以当前系统为准）：NSPopupMenuWindow 属 NSPopover 框架、level=101、hasShadow=true、DarkAqua；contentView=NSGlassEffectView、r=12、tint=nil、`_adaptiveAppearance=1`、`_subvariant="menu"`、`_contentLensing=0`、`_subduedState=0`、`_scrimState=0`；内部 SwiftUI SDF 管线（CASDFLayer/CASDFElementLayer/CABackdropLayer，`filters=[glassBackground]`、scale=0.5、groupName="SwiftUI:5.00.0"、SDF smoothness=8、gaussianRadius=0）。`glassBackground` CAFilter 参数挖不出来（DLCAFilter 无暴露 objc 属性，CIFilter 注册表无此名）。
4. **`app.appearance` override 对液态玻璃渲染零影响**（截图字节级相同）——玻璃是背景自适应的，别在外观位上浪费时间。
5. **锁屏门控**：锁屏下 WindowServer 不给 backdrop 采样，玻璃全黑；`caffeinate -u -t N` 只能唤醒显示器，**解不了 backdrop 门控**。判锁用 CGSessionCopyDictionary（无 OnConsoleKey 且 ScreenIsLocked=1）；无人值守场景用 cron 轮询解锁后再跑截图评估。
6. **probe 设计**：玻璃 backdrop 只采样「窗口之后的屏幕内容」，**不采样同一窗口内的下层视图**——背景图必须放独立的下层窗口（双层窗 probe）。自建长命 harness（accessory 进程 + NSWindow）会撞 `NSViewIsCurrentlyBuildingLayerTreeForDisplay` 断言直接死，probe 做成 60s 自退的一次性进程；ground truth 一律 AX 点真托盘菜单（`perform action "AXPress"` 选叶子项，`click` 只悬停；AX 派发有 ~2s 延迟，截图时点按 t+3.6s 起）。
7. **浮窗贴 NSScreen.main 底部居中，main 跟随用户活跃屏**——多屏截图前先 CGWindowList 探针拿浮窗 bounds 定位屏，别假定固定屏（两次乌龙教训）。
8. **效果类调整纪律**：不臆断，一切结论以「真系统组件同屏对拍截图」为准；矩阵 probe 一次并排 7~8 个变体比逐轮改 app 快一个数量级。

## 遗留 / 观察项

- 亮背景下白字可读性未实测；若发虚，候选：`_adaptiveAppearance` 自适应压暗已开，或文字加柔影。
- 圆角现为 12（菜单对齐）；胶囊形 r=42（参考图控件气质）为备选，一行改动。
- `overlay.glassStyle`/`overlay.glassTintAlpha` 仍是 UserDefaults 隐藏键，未迁 config.toml（Windows 端无对应渲染能力，迁移时需在 `Doc/Ref/desktop-config.md` 注明 macOS-only）。
- Windows 端悬浮窗无液态玻璃对应能力，未做对齐。
