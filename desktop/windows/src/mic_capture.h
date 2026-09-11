// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本机麦克风采集接口（本机麦克风模式迭代二）：WASAPI 采集的抽象层。
// 协调器只依赖本接口，经 SetLocalMicRuntime 注入生产实现或测试 fake；
// 生产实现见 wasapi_mic_capture.h。
//
// 线程契约（对齐 wasapi_virtual_mic_renderer 的注入模式）：
// - Start/Stop 在协调器会话线程（主线程）调用；
// - on_pcm 在采集线程触发，回调内不得再进入协调器音频锁以外的阻塞调用；
// - Stop 返回后保证不再触发 on_pcm（实现须 join 采集线程），因此释放路径
//   可以安全地在其后访问会话私有的编码器/组帧器状态。

#ifndef VOICESTICK_MIC_CAPTURE_H_
#define VOICESTICK_MIC_CAPTURE_H_

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace voicestick {

class IMicCapture {
public:
    virtual ~IMicCapture() = default;

    // 钉扎采集端点（endpoint id）：wechat 方案 A 在 auto_switch 把默认录音设备切到
    // 虚拟麦后才启动采集，须钉住切换前的真实麦克风端点，否则按"默认设备"解析会
    // 采到虚拟麦回环（ring buffer → 渲染 → 虚拟麦 → 采集 → ring buffer 自旋，无
    // 真实输入）。空串/未调用 = 按当前默认设备打开。须在 Start 前调用。
    virtual void SetPreferredEndpointId(const std::string& endpoint_id) { (void)endpoint_id; }
    // 打开默认麦克风并开始采集。失败返回 false，LastStartError 给出原因。
    virtual bool Start() = 0;
    // 停止采集并释放设备。幂等：未 Start 时为空操作。
    virtual void Stop() = 0;
    virtual std::string LastStartError() const { return {}; }

    // 单声道 16kHz PCM16 采样回调（采集线程；生命周期见类注释）。
    std::function<void(std::span<const std::int16_t>)> on_pcm;
};

}  // namespace voicestick

#endif  // VOICESTICK_MIC_CAPTURE_H_
