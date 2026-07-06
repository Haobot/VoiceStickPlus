# 微信输入法虚拟麦克风事件驱动渲染（方案 A）

## 背景

走笔记本自带麦克风阵列时，微信输入法文字识别流畅；走本项目的虚拟音频（VB-CABLE）路径时识别明显卡顿。

## 根因

`desktop/windows/src/wasapi_virtual_mic_renderer.cc` 渲染线程用固定 `sleep_for(render_period_ms=10ms)` 驱动，且每次提交被 `frames_per_period`（=10ms=160 帧）硬限制：

```cpp
frames_to_render = std::min(available, frames_per_period);  // 上限锁死 10ms
...
render_client_->ReleaseBuffer(frames_to_render, 0);
std::this_thread::sleep_for(std::chrono::milliseconds(options_.render_period_ms));
```

- Windows 默认定时器粒度 ~15.6ms（全仓无 `timeBeginPeriod`、无事件驱动），`sleep_for(10ms)` 实际睡 15–16ms。
- 提交 10ms / 消费 15.6ms = 0.64× < 1.0×，WASAPI buffer 稳态欠载（underrun），输出被静音间隙切断 → ASR 卡顿。
- 生产 1.0× > 消费 0.64×，`PcmRingBuffer`（8192，drop-oldest）约 1.4s 后溢出丢数据 → 长录音恶化。

非瓶颈：Opus 解码（微秒级）、16k→48k 重采样（WASAPI 内核 `AUTOCONVERTPCM`）。前者保留不动，后者必须保留（见 [[wasapi-shared-mode-format-pitfall]]）。

## 方案

改事件驱动渲染，让提交速率跟随 WASAPI 实际消费（1.0×）。

### 文件改动

1. **新增 `desktop/windows/src/wasapi_render_sink.h`**：抽象接口 `WasapiRenderSink`，解耦渲染循环与真实 COM，使渲染逻辑可注入 fake 单测。
2. **改 `wasapi_virtual_mic_renderer.h/cc`**：
   - 新增可测组件 `RenderPump`：持有 sink + ring buffer 引用，`PumpOnce()` 执行单次迭代（`CurrentPadding` → `frames_to_render = buffer_frame_count - padding`，**去掉 `frames_per_period` 上限** → `GetBuffer` → ring 读不足补静音 → `ReleaseBuffer`）。
   - `ComWasapiRenderSink`：搬现有 COM 代码（`OpenDevice`/`InitializeStream`/`GetService`），`Initialize` 增加 `AUDCLNT_STREAMFLAGS_EVENTCALLBACK` + `CreateEvent`（auto-reset）+ `SetEventHandle`。
   - `WasapiVirtualMicRenderer` 渲染线程：`WaitForSingleObject(NotifyEvent, INFINITE)` 唤醒 → `PumpOnce()`。`buffer_duration_ms` 默认 100→50ms。
   - `Stop`：先 `SetEvent(NotifyEvent)` 唤醒阻塞线程，再 `join`，最后 `sink->Stop()` + Cleanup。避免死等。
3. **改 `desktop/windows/tests/core_tests.cc`**：`FakeWasapiRenderSink` + `TestRenderPump*` 系列 + `TestWasapiRendererStopsCleanlyWakingBlockedThread`，注册进 `main()`。

### TDD 顺序

1. 🔴 `TestRenderPumpSubmitsFullAvailableNoCap`：padding=0、buffer=800 帧、ring 有 800 帧 → 断言提交 800 帧（锁住去掉 10ms 上限的核心修复）。当前无 sink/pump 入口 → 编译失败 = 红灯。
2. 🟢 加 `WasapiRenderSink` 接口 + `RenderPump` + `ComWasapiRenderSink` + 事件驱动循环 → 转绿。
3. 🔴 `TestRenderPumpFillsSilenceWhenRingEmpty`、`TestRenderPumpZeroWhenBufferFull`、`TestWasapiRendererStopsCleanlyWakingBlockedThread`。
4. 🟢 转绿。
5. 🔵 重构。

### 不改

Opus 解码、`AUTOCONVERTPCM`+`SRC_DEFAULT_QUALITY`、ring buffer 容量 8192、coordinator 调用时序（hotkey 先于 Start）、`IVirtualMicRenderer` 接口。

### 验证

1. `ctest --test-dir desktop/windows/build-x64 --output-on-failure -R voicestick_windows_tests` 全绿。
2. `build_win.bat` 构建通过，核对 `VoiceStick.exe` 时间戳/体积（[[windows-build-fake-success]]）。
3. 真机长按录音对比流畅度（事件驱动时序无法 CI 覆盖，与现有 wechat 渲染器验证现状一致）。临时 underrun 计数日志核实后移除（[[firmware-serial-log-capture]]）。

### 风险

- 事件句柄生命周期：`ComWasapiRenderSink` 析构 `CloseHandle`，`SetEventHandle` 前创建 auto-reset event。
- `buffer_duration_ms=50`：事件驱动下安全；真机仍卡可回退 100ms（不影响逻辑）。
- Stop 死等：靠 `TestWasapiRendererStopsCleanlyWakingBlockedThread` 防回归。
