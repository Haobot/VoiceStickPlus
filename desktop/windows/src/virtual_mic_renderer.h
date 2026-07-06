// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 虚拟麦克风渲染器抽象接口。
// 用于 wechat_input_method 输出模式：将 PCM 渲染到指定播放设备（如 VB-CABLE），
// 使其 Recording 端作为系统麦克风被微信输入法取音。
//
// 生产实现为 WasapiVirtualMicRenderer；测试可注入 fake 以解耦真实 WASAPI 设备，
// 使 wechat 模式的录音/落盘流程可在无虚拟麦环境下单元测试。

#ifndef VOICESTICK_VIRTUAL_MIC_RENDERER_H_
#define VOICESTICK_VIRTUAL_MIC_RENDERER_H_

#include <string>

namespace voicestick {

class PcmRingBuffer;

class IVirtualMicRenderer {
 public:
  struct Options {
    int sample_rate = 16000;
    int channels = 1;
    int bits_per_sample = 16;
    // 用于匹配播放设备名称的子串（大小写不敏感）。
    std::wstring device_name_substring;
    // WASAPI 缓冲区长度（毫秒）。
    int buffer_duration_ms = 100;
    // 每次从 ring buffer 读取的帧长（毫秒）。
    int render_period_ms = 10;
  };

  virtual ~IVirtualMicRenderer() = default;

  // 启动渲染线程。source 必须在 Stop() 之前保持有效。
  // 返回 false 表示找不到匹配设备或初始化失败。
  virtual bool Start(PcmRingBuffer* source) = 0;

  // 停止渲染线程并释放资源。
  virtual void Stop() = 0;

  virtual bool IsRunning() const = 0;

  // 返回实际打开的设备名称；未启动时为空。
  virtual std::wstring ActiveDeviceName() const = 0;
};

} // namespace voicestick

#endif // VOICESTICK_VIRTUAL_MIC_RENDERER_H_
