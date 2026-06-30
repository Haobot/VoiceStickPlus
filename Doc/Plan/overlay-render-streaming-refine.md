# 优化 overlay 渲染，恢复流式精修逐字显示

## 背景

v1.7.1 之前流式精修（`RefineStream`）开启后悬浮窗持续闪动、程序卡死，已在阶段1回退为非流式
`Refine`（走 `ChatAsync` 后台线程，提交 `090fed5`）止血。但非流式精修期间悬浮窗只显示静态
"Refining"，失去逐字流动的视觉效果——用户反馈该流动效果体验很好，希望保留并更流畅。

精修耗时实测（腾讯云 ASR + LLM 精修）：短句 2.5s、中长 4.3s、长段 12.8s。LLM 延迟无法压缩，
"压缩总时间"方向（definite 并行 / 倒计时并行）均不可行（见会话讨论）。因此优化重心转为
"让这段不可避免的精修等待变得可感知且不卡顿"——即恢复流式逐字显示，同时消除卡死闪动。

## 根因（阶段1已定位）

1. **卡死**：`OverlayWindow::OnTimer` 在 `kListening` 模式下每 `kAnimationStepMs=16ms` 无条件
   `InvalidateStaticLayer() + UpdateLayeredBitmap()`（`overlay_window.cc:377-380`）。`InvalidateStaticLayer`
   置 `static_layer_dirty_=true` 后，`UpdateLayeredBitmap` 调 `BuildStaticLayer` 全量重建，其中
   `PaintText` **每次都新建 D2D render target + `CreateTextLayout`（不缓存）**重排整段长文本
   （`overlay_window.cc:841`、`977-1018`）+ GDI+ 圆角阴影 + 大位图 `memset`/`memcpy`。流式精修
   每 ~60ms 到一个 token 调 `ShowPartial` 重置文本，叠加 16ms 动画全量重建，UI 线程渲染过载、
   消息队列堵塞 → 卡死。
2. **闪动**：`Show()` 文本变化时设 `text_transition_started_at_ms_`（`overlay_window.cc:397-399`），
   文字滚动动画时长 `kTextTransitionMs=140ms`。流式精修每 60ms 一个 token，动画 140ms 永远跑
   不完就被重置，`scroll_offset` 在 from→to 中途反复跳动 → 闪动。

关键冗余：`OnTimer` 在 kListening **文本未变**时也 `InvalidateStaticLayer`，让昂贵的文本布局
每 16ms 重建一次。而指示器音浪条在 `UpdateLayeredBitmap` 的 `PaintIndicator` 里独立绘制
（`overlay_window.cc:751`），本不需要重建 static layer。

## 设计

### 改动1：OnTimer 不在文本未变时重建 static layer

`OnTimer` 的 `kAnimationTimerId` 分支（`overlay_window.cc:373-384`）改为：kListening 模式下，
仅当文字过渡动画进行中（`text_transition_started_at_ms_ != 0`）或窗口正在缩放
（`StepWindowAnimation` 返回 true）时才 `InvalidateStaticLayer`；否则跳过重建，仅
`UpdateLayeredBitmap`（用缓存 static layer + 重绘指示器）。

```c
} else if (timer_id == kAnimationTimerId) {
    animation_frame_++;
    const bool window_moved = StepWindowAnimation();
    const bool text_transitioning = text_transition_started_at_ms_ != 0;
    if (window_moved || text_transitioning) {
        InvalidateStaticLayer();
    }
    // 始终 UpdateLayeredBitmap：static layer 命中缓存时只重绘动态指示器
    UpdateLayeredBitmap();
    if (!window_moved && mode_ != Mode::kListening && mode_ != Mode::kCountdown && !text_transitioning) {
        KillTimer(hwnd_, kAnimationTimerId);
    }
}
```

效果：kListening 静态文本时，`BuildStaticLayer` 只在文本/窗口变化时执行一次，16ms tick 只做
廉价的 `memcpy` 缓存 + `PaintIndicator` + `UpdateLayeredWindow`，消除 D2D 文本布局每帧重建。

### 改动2：流式精修更新禁用文字滚动动画

流式 token 高频到达时，文字滚动过渡动画（140ms）会被反复重置导致闪动。给 `Show` 增加一个
"静默更新"入口，流式精修追加文本时不触发 `text_transition_started_at_ms_`，直接显示当前累积
文本。

新增 `OverlayWindow::AppendPartial(const std::string& text)`：与 `ShowPartial` 类似但不设置
文字过渡动画时间戳，直接 `InvalidateStaticLayer + Reposition + UpdateLayeredBitmap`。或更简单：
给 `Show(Mode, text, hint)` 增加布尔参数 `skip_text_transition`，流式精修路径传 true。

`Show` 中（`overlay_window.cc:397-405`）：
```c
if (!skip_text_transition && mode_ != Mode::kHidden && next_text != text_) {
    text_scroll_from_offset_ = last_text_scroll_offset_;
    text_transition_started_at_ms_ = GetTickCount64();
} else if (mode_ == Mode::kHidden) {
    text_transition_started_at_ms_ = 0;
    ...
}
```

流式精修 `on_token` 回调改调 `AppendPartial`（或 `Show(..., skip=true)`），文字直接跳到当前累积值，
无滚动动画 → 不闪动。

### 改动3：恢复 RefineStream

`TransformText` 精修分支（`voice_stick_coordinator.cc:1073-1087`）从 `refiner_.Refine` 改回
`refiner_.RefineStream`，`on_token` 节流式调 `AppendPartial`（保留 ~60ms 节流，但改用静默更新
避免闪动），`on_complete` 调 `ShowPartial` 做最终一次显示。

## 边界与不变量

- **录音中 ASR partial 路径不受影响**：`on_partial` 仍走 `ShowPartial`（带滚动动画），录音中
  partial 是增量短文本，D2D 布局快，且文本频繁变化本就需要重建——改动1只优化"文本未变"的
  16ms tick，partial 文本变化时仍正常重建。
- **指示器动画不受影响**：kListening 模式下 `kAnimationTimerId` 仍持续运行（音浪条需要动），
  只是不再每帧重建 static layer。
- **文字过渡动画保留**：非流式场景（ASR partial、final 倒计时）仍用滚动动画，体验不变；仅
  流式精修高频追加时跳过。
- **窗口缩放动画不受影响**：`StepWindowAnimation` 返回 true 时仍 `InvalidateStaticLayer`。
- **static layer 缓存一致性**：`text_`/`animated_window_width_`/`animated_window_height_`/
  `theme_*` 变化时均经 `Show`/`SetTheme*`/`StepWindowAnimation` 触发 `InvalidateStaticLayer`，
  缓存不会失效漏更新。

## 验证

Windows UI 无单测覆盖此路径，靠编译 + CTest 不回归 + 真机验证：

1. **编译**：`build_win.bat` 通过。
2. **CTest**：`voicestick_windows_tests.exe` 全过（不破坏现有协调器/协议/mux 测试）。
3. **真机**：
   - 开启精修，做长段语音识别（60 字以上）。预期：录音中显示 ASR partial（带滚动动画），
     录音结束后悬浮窗进入精修，逐字流式显示精修结果，**不卡死、不闪动**。
   - 对照阶段1（非流式）：精修期间应有逐字流动效果，而非静态 "Refining"。
   - 关闭精修：行为与 v1.7.1 一致（直接显示 ASR final，无精修）。
   - 录音中 partial 显示正常，指示器音浪条动画正常。
