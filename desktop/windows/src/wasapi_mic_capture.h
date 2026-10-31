// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// WASAPI 本机麦克风采集（本机麦克风模式迭代二）：默认捕获设备共享模式采集，
// 引擎自动重采样到 16kHz 单声道 PCM16（AUTOCONVERTPCM），事件驱动采集线程
// 逐 packet 回调 on_pcm。接口契约见 mic_capture.h。

#ifndef VOICESTICK_WASAPI_MIC_CAPTURE_H_
#define VOICESTICK_WASAPI_MIC_CAPTURE_H_

#include "mic_capture.h"

#include <atomic>
#include <memory>
#include <thread>

namespace voicestick {

class WasapiMicCapture : public IMicCapture {
public:
    WasapiMicCapture() = default;
    ~WasapiMicCapture() override;

    WasapiMicCapture(const WasapiMicCapture&) = delete;
    WasapiMicCapture& operator=(const WasapiMicCapture&) = delete;

    bool Start() override;
    void Stop() override;
    std::string LastStartError() const override { return last_start_error_; }

private:
    // 采集线程：COM 初始化/设备打开在本线程完成后经 open_done 通知 Start
    //（opened 成败见 started_ok）。用 shared_ptr 而非栈引用：Start 超时返回后
    // 线程可能仍在收尾，指向栈对象的引用会悬垂。
    void CaptureThreadMain(std::shared_ptr<std::atomic_bool> started_ok,
                           std::shared_ptr<std::atomic_bool> open_done);

    std::thread capture_thread_;
    std::atomic_bool stop_requested_{false};
    // 是否处于成功采集态（Start 幂等与超时后拒绝重复启动的判据）。
    std::atomic_bool running_{false};
    // 最近一次启动的 open_done 标志；线程结束后由 Start/Stop 回收 joinable 线程。
    std::shared_ptr<std::atomic_bool> open_done_;
    // Start 失败原因（用户可见），空表示尚未失败。
    std::string last_start_error_;
};

}  // namespace voicestick

#endif  // VOICESTICK_WASAPI_MIC_CAPTURE_H_
