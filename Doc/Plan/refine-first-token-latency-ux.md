# 精修首字延迟：消除悬浮窗"卡住"感

## 背景

开启 `refine_enabled` 后，用户反馈悬浮窗在 ASR 结束后会"卡住 1~2 秒"才出现文字。经核查
（见下方根因），这不是渲染卡顿——流式逐字渲染已在 `overlay-render-streaming-refine.md` 优化过；
真正原因是 **ASR final 到达后到精修首字到达之间，悬浮窗冻结在最后一条 ASR partial 上、毫无变化**，
让用户误以为卡死。

LLM 服务端首 token 时间（TTFT）+ 每次精修重建 HTTPS/TLS 握手构成 1~2s 首字延迟，其中服务端
TTFT 不可压缩（实测短句 2.5s、长段 12.8s，见会话记忆）。本 RFC 的目标不是压缩 LLM 总时间，而是
**把这段不可避免的等待变成可感知、不卡顿的交互**。

## 根因（已核查）

1. `FinishWithFinalText` 拿到 final 文本后，**不把原文刷上悬浮窗**，直接进 `TransformText` →
   `RefineStream`（`voice_stick_coordinator.cc:868-875`、`1079`）。
2. `RefineStream` → `ChatStream` 每次精修都新建 WinHTTP session + TCP + TLS 握手
   （`llm_chat_client.cc:173-218`，无连接复用），叠加服务端 TTFT，构成首字延迟。
3. 首字到达前，悬浮窗 `mode_` 仍是 `kListening`（`overlay_window.h:43` 无 refining 模式），
   指示器是音浪条动画，看起来和"正在听"一样；`SetStatus("Refining")` 只进托盘 tooltip
   （`win32_app.cc:415-420`），悬浮窗完全无感。
4. 首个精修 token 到达后才经 `AppendPartial` 更新文字（`voice_stick_coordinator.cc:1098`）。
   `on_token` 节流逻辑里 `last_update` 初值为 epoch，首个 token 立即触发更新，无额外 60ms 等待——
   即延迟完全来自建连 + TTFT，节流不是瓶颈。

## 设计

### 改动1：final 到达后立即显示 ASR 原文

`FinishWithFinalText` 的精修分支（`voice_stick_coordinator.cc:868-875`）在 `TransformText` 之前，
先把 ASR final 原文刷上悬浮窗，让用户立刻看到识别结果：

```c
if (config_.refine_enabled) {
    ui_->SetStatus("Refining");
    if (active_device_id_.has_value()) {
        ui_->AppendPartial(text, *active_device_id_);  // 立即显示 ASR 原文
    }
}
TransformText(text, profile, ...);
```

用 `AppendPartial` 而非 `ShowPartial`：前者跳过文字滚动过渡动画（`overlay_window.cc` 的
`skip_text_transition` 路径），避免与随后到达的精修 token 抢动画。`AppendPartial` 接收完整文本
（非增量），所以这里直接传 `text` 即可把悬浮窗设为原文。

精修首字到达时，`on_token` 回调里的 `AppendPartial(current, device_id)` 会把悬浮窗从"原文"
替换为"精修累积文本"——这是一次跳变，但用户已在看正确的识别结果，跳变到精修版可接受。
（可选增强：见改动3的光标过渡。）

**注意**：`AppendPartial` 当前语义是"流式追加累积文本"。这里复用它显示原文，不改变其接口；
只是调用时机提前。若 `UiDelegate`/`Win32App` 转发链对"非精修期调用 AppendPartial"有假设，需核查
（调研未发现限制，但实现时需验证 `device_id` 一致性）。

### 改动2：加 kRefining 模式，精修期指示器区别于 kListening

给 `OverlayWindow::Mode` 增加 `kRefining`（`overlay_window.h:43`），精修期间用该模式，
让用户明确知道"在精修"而非"还在听"：

- 指示器样式：kRefining 时绘制"三点跳动"或"光标闪烁"，区别于 kListening 的音浪条。
  复用 `PaintIndicator`（`overlay_window.cc:751` 附近）按 `mode_` 分支。
- 文字仍由 `AppendPartial` 更新（精修流式 token），kRefinding 与文字更新路径正交。
- `OnTimer` 的 kRefining 分支：与 kListening 类似，文本未变时不重建 static layer（沿用
  `overlay-render-streaming-refine.md` 改动1的优化），仅重绘动态指示器。

入口：精修开始时（`FinishWithFinalText` 的 `if (config_.refine_enabled)` 分支）切到 kRefining；
精修完成（`on_complete`）或失败回退时切回 kListening / 进入倒计时。

`UiDelegate`/`Win32App` 需要新增一个转发方法（如 `ShowRefining(device_id)` 或给 `ShowPartial`
加模式参数），具体形式实现时定，保持与现有 `AppendPartial`/`ShowPartial` 转发链一致。

### 改动3（可选增强）：原文→精修的过渡光标

若改动1的"原文跳变到精修首字"仍显突兀，可在精修首字到达前，在原文末尾显示一个闪烁光标
（`▍` 或 `…`），暗示"正在改写"；首字到达后光标消失、替换为精修流式文本。这是纯视觉打磨，
非必需，可作为后续迭代。

## 边界与不变量

- **translate 路径**：`profile.transform == kTranslate` 走 `translator_.Translate`（`voice_stick_coordinator.cc:855-866`），
  不经过精修分支，本 RFC 不影响。但 translate 同样有 LLM 等待延迟，若体验一致可后续同法处理
  （先显示原文 + Translating 提示），本次不纳入。
- **subtitle 路径**：走 `ShowSubtitleText`，不显示精修流式（`windows-only-no-macos-streaming`），
  本 RFC 不影响。
- **精修失败 fallback**：`on_complete` 的 `ok=false` 用原始 `text` 回退（`voice_stick_coordinator.cc:1110`）。
  改动1已提前显示原文，失败时悬浮窗已是原文，回退一致；`ShowPartial(result)` 在 `ok=false` 时
  不调用（`if (ok && !result.empty())`），不会用空 result 覆盖原文。✓
- **录音中 ASR partial**：仍走 `ShowPartial`（带滚动动画），`on_partial` 路径不变。✓
- **CancelStreamingRefinement**：精修被取消时（如侧键取消识别），已显示的原文需被清理——
  取消路径会 `HideOverlay` 或进入 ready，核查 `CancelStreamingRefinement` 调用点是否覆盖。✓ 需验证。
- **空文本**：`text.empty()` 在 847 行已提前 return，精修分支不会收到空文本。✓

## 不做的事

- **不压缩 LLM 总时间**：服务端 TTFT 不可压缩，definite 并行 / 倒计时并行已否决
  （见 `streaming-refine-render-optimization` 记忆）。
- **不动 LLM 连接层**：连接复用/预热可省几百 ms TLS 握手，但 WinHTTP 跨调用连接复用工程量大、
  回归风险高，性价比低于本 RFC 的体验优化。若后续仍需压缩首字延迟，再单独立项。
- **不加精修专用超时**：当前仅 WinHTTP receive 60s，服务端 hang 时等待过长，但属另一问题，
  本 RFC 不纳入。

## 验证

Windows UI 无单测覆盖此路径，靠编译 + CTest 不回归 + 真机：

1. **编译**：`build_win.bat` 通过。
2. **CTest**：`voicestick_windows_tests` 全过（不破坏现有协调器/协议/mux 测试）。
3. **真机**：
   - 开启精修，做中长段语音识别。预期：录音结束后悬浮窗**立即显示 ASR 原文** + 精修中指示器，
     随后精修 token 流式覆盖原文，无"卡住空白"。
   - 关闭精修：行为不变（直接显示 ASR final，无精修）。
   - 精修失败（断网/LLM 报错）：悬浮窗保留原文，不闪空。
   - 录音中 partial、音浪条动画、倒计时均不受影响。

## 后续可选

- LLM 连接复用/预热（压缩 TLS 握手，单独立项）。
- translate 路径同法处理先显示原文。
- 精修专用超时（如 8s 后回退原文）。
