// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 通过 WASAPI 把 PCM 渲染到指定虚拟麦克风播放端（Playback endpoint）。
// 用于 wechat_input_method 输出模式：Opus 解码后的音频经此模块进入虚拟麦克风。

#ifndef VOICESTICK_WASAPI_VIRTUAL_MIC_RENDERER_H_
#define VOICESTICK_WASAPI_VIRTUAL_MIC_RENDERER_H_

#include <memory>
#include <string>
#include <vector>

#include "virtual_mic_renderer.h"
#include "wasapi_render_sink.h"

namespace voicestick {

class PcmRingBuffer;

// 可单测的渲染迭代逻辑，不含线程/COM 生命周期。持有 sink + ring buffer 引用，
// PumpOnce 执行一次"读 padding→算可提交→GetBuffer→ring 读不足补静音→ReleaseBuffer"。
// WasapiVirtualMicRenderer 渲染线程事件唤醒后调用，从而把提交速率与 WASAPI 实际
// 消费解耦 sleep 周期，消除稳态 underrun。
class RenderPump {
 public:
  RenderPump(WasapiRenderSink* sink, PcmRingBuffer* source, int channels);
  ~RenderPump();
  RenderPump(const RenderPump&) = delete;
  RenderPump& operator=(const RenderPump&) = delete;

  // 执行一次渲染迭代。返回本次提交到 WASAPI 的帧数；0 表示 buffer 已满或读取失败。
  UINT32 PumpOnce();

 private:
  WasapiRenderSink* sink_;
  PcmRingBuffer* source_;
  int samples_per_frame_;
  std::vector<int16_t> read_buffer_;
};

// WASAPI 渲染器：将 PcmRingBuffer 中的 16-bit PCM 持续写入指定的播放设备。
// 典型使用场景：写入 VB-CABLE / Virtual Audio Cable 的 Playback 端，
// 使其 Recording 端作为系统麦克风被微信输入法取音。
class WasapiVirtualMicRenderer : public IVirtualMicRenderer {
 public:
  // Options 复用基类 IVirtualMicRenderer::Options。
  explicit WasapiVirtualMicRenderer(const Options& options);
  // 测试注入：用自定义 sink（如 FakeWasapiRenderSink）替换真实 COM 实现。
  WasapiVirtualMicRenderer(const Options& options,
                           std::unique_ptr<WasapiRenderSink> sink);
  ~WasapiVirtualMicRenderer() override;

  WasapiVirtualMicRenderer(const WasapiVirtualMicRenderer&) = delete;
  WasapiVirtualMicRenderer& operator=(const WasapiVirtualMicRenderer&) = delete;

  // 启动渲染线程。source 必须在 Stop() 之前保持有效。
  // 返回 false 表示找不到匹配设备或 WASAPI 初始化失败。
  bool Start(PcmRingBuffer* source) override;

  // 停止渲染线程并释放 WASAPI 资源。
  void Stop() override;

  bool IsRunning() const override;

  // 返回实际打开的设备名称；未启动时为空。
  std::wstring ActiveDeviceName() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voicestick

#endif  // VOICESTICK_WASAPI_VIRTUAL_MIC_RENDERER_H_
