// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// WASAPI 本机麦克风采集（本机麦克风模式迭代二）：默认捕获设备共享模式采集，
// 引擎自动重采样到 16kHz 单声道 PCM16（AUTOCONVERTPCM），事件驱动采集线程
// 逐 packet 回调 on_pcm。接口契约见 mic_capture.h。

#ifndef VOICESTICK_WASAPI_MIC_CAPTURE_H_
#define VOICESTICK_WASAPI_MIC_CAPTURE_H_

#include "mic_capture.h"

#include <atomic>
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
    // 见 mic_capture.h：钉扎端点（wechat 方案 A 用，须在 Start 前调用）。
    void SetPreferredEndpointId(const std::string& endpoint_id) override;

private:
    // 采集线程：COM 初始化/设备打开在本线程完成后经 open_done 通知 Start
    //（opened 成败见 started_ok；两指针仅在 open_done 置位前有效）。
    void CaptureThreadMain(std::atomic_bool* started_ok, std::atomic_bool* open_done);

    std::thread capture_thread_;
    std::atomic_bool stop_requested_{false};
    // Start 失败原因（用户可见），空表示尚未失败。
    std::string last_start_error_;
    // 钉扎的捕获端点 id（空 = 默认设备）。Start 前设置，采集线程读取。
    std::wstring preferred_endpoint_id_;
};

}  // namespace voicestick

#endif  // VOICESTICK_WASAPI_MIC_CAPTURE_H_
