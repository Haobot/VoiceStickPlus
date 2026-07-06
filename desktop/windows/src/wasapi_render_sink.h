// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// WASAPI 渲染端操作抽象，解耦渲染循环与真实 COM 调用，便于注入 fake 单测。
// 用于 wechat_input_method 输出模式：事件驱动渲染线程阻塞于 NotifyEvent，
// 唤醒后按 CurrentPadding 计算可提交量并填充。

#ifndef VOICESTICK_WASAPI_RENDER_SINK_H_
#define VOICESTICK_WASAPI_RENDER_SINK_H_

#include <windows.h>  // UINT32 / BYTE / HANDLE

#include <cstdint>

#include "virtual_mic_renderer.h"

namespace voicestick {

// 抽象 WASAPI render endpoint 的最小操作集，供 RenderPump 与渲染线程使用。
// 生产实现 ComWasapiRenderSink 封装真实 COM 调用；测试可注入 FakeWasapiRenderSink。
class WasapiRenderSink {
 public:
  virtual ~WasapiRenderSink() = default;

  // 完成设备枚举、InitializeStream（含 AUDCLNT_STREAMFLAGS_EVENTCALLBACK +
  // SetEventHandle）、GetBufferSize、GetService。成功后 BufferFrameCount() 与
  // NotifyEvent() 可用。返回 false 表示找不到匹配设备或初始化失败。
  virtual bool OpenAndInitialize(const IVirtualMicRenderer::Options& options) = 0;

  // WASAPI render buffer 总帧数（GetBufferSize 结果）。
  virtual UINT32 BufferFrameCount() const = 0;

  // 当前已填充（尚未被设备消费）的帧数。失败返回 false。
  virtual bool CurrentPadding(UINT32* out_padding) = 0;

  // 取一段可写缓冲。返回的指针在 ReleaseBuffer 调用前有效。失败返回 false。
  virtual bool GetBuffer(UINT32 frames, BYTE** out) = 0;

  // 提交 frames 帧已写入的数据到 WASAPI。frames 为 0 表示丢弃本次缓冲（写静音）。
  virtual void ReleaseBuffer(UINT32 frames) = 0;

  // 启动/停止 WASAPI 设备消费（audio_client Start/Stop）。
  virtual void Start() = 0;
  virtual void Stop() = 0;

  // WASAPI 事件句柄：render buffer 需要填充时触发。必须为 auto-reset event。
  // 渲染线程 WaitForSingleObject 阻塞于此；Stop 时 SetEvent 唤醒以避免死等。
  virtual HANDLE NotifyEvent() = 0;
};

}  // namespace voicestick

#endif  // VOICESTICK_WASAPI_RENDER_SINK_H_
