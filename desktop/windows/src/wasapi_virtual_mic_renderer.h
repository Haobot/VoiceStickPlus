// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 通过 WASAPI 把 PCM 渲染到指定虚拟麦克风播放端（Playback endpoint）。
// 用于 wechat_input_method 输出模式：Opus 解码后的音频经此模块进入虚拟麦克风。

#ifndef VOICESTICK_WASAPI_VIRTUAL_MIC_RENDERER_H_
#define VOICESTICK_WASAPI_VIRTUAL_MIC_RENDERER_H_

#include <memory>
#include <string>

#include "virtual_mic_renderer.h"

namespace voicestick {

class PcmRingBuffer;

// WASAPI 渲染器：将 PcmRingBuffer 中的 16-bit PCM 持续写入指定的播放设备。
// 典型使用场景：写入 VB-CABLE / Virtual Audio Cable 的 Playback 端，
// 使其 Recording 端作为系统麦克风被微信输入法取音。
class WasapiVirtualMicRenderer : public IVirtualMicRenderer {
 public:
  // Options 复用基类 IVirtualMicRenderer::Options。
  explicit WasapiVirtualMicRenderer(const Options& options);
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
