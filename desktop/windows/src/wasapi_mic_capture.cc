// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "wasapi_mic_capture.h"

#include "log.h"

#include <windows.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

namespace voicestick {

using Microsoft::WRL::ComPtr;

namespace {

std::string HrToHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

WasapiMicCapture::~WasapiMicCapture() {
    Stop();
}

bool WasapiMicCapture::Start() {
    // 幂等：只有上一次启动确实成功（running_）才算已在采集。上次 Start 超时后
    // 线程可能仍在退出，此时不能再报成功；若线程已结束则回收后允许重新启动。
    if (capture_thread_.joinable()) {
        if (running_.load()) return true;
        if (open_done_ && open_done_->load()) {
            capture_thread_.join();
            capture_thread_ = std::thread{};
            open_done_.reset();
        } else {
            return false;
        }
    }
    last_start_error_.clear();
    stop_requested_.store(false);
    // 设备打开挪到采集线程（CaptureThreadMain 内 COM 初始化与设备激活同线程），
    // Start 本身只负责起线程；打开失败经 last_start_error_ 暴露，由协调器会话
    // 收尾路径消费。
    // 有界等待：音频服务/驱动挂起时旧实现 while(!open_done) yield() 会永久占满
    // UI 线程；这里 3s 超时后置 stop_requested_ 并返回 false，线程自行收尾。
    auto started_ok = std::make_shared<std::atomic_bool>(false);
    auto open_done = std::make_shared<std::atomic_bool>(false);
    open_done_ = open_done;
    capture_thread_ = std::thread([this, started_ok, open_done] {
        CaptureThreadMain(started_ok, open_done);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!open_done->load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            last_start_error_ = "audio capture open timeout";
            stop_requested_.store(true);
            LogApp("WasapiMicCapture: open timeout (audio service/driver hung), Start aborted");
            // 给线程 500ms 收尾窗口；能收就收，收不了也不能在这里无限等。
            for (int i = 0; i < 50 && !open_done->load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (open_done->load()) {
                capture_thread_.join();
                capture_thread_ = std::thread{};
                open_done_.reset();
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!started_ok->load()) {
        capture_thread_.join();
        capture_thread_ = std::thread{};
        open_done_.reset();
        running_.store(false);
        return false;
    }
    running_.store(true);
    return true;
}

void WasapiMicCapture::Stop() {
    running_.store(false);
    if (!capture_thread_.joinable()) {
        open_done_.reset();
        return;
    }
    stop_requested_.store(true);
    // CaptureThreadMain 在事件/包处理间隙检查 stop_requested_，最长一个包周期内退出。
    capture_thread_.join();
    capture_thread_ = std::thread{};
    open_done_.reset();
}

void WasapiMicCapture::CaptureThreadMain(std::shared_ptr<std::atomic_bool> started_ok,
                                         std::shared_ptr<std::atomic_bool> open_done) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(hr);
    if (!com_initialized && hr != RPC_E_CHANGED_MODE) {
        last_start_error_ = "CoInitializeEx failed " + HrToHex(hr);
        started_ok->store(false);
        open_done->store(true);
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(enumerator.GetAddressOf()));
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                 device.GetAddressOf());
    } else {
        last_start_error_ = "CoCreateInstance(MMDeviceEnumerator) failed " + HrToHex(hr);
    }
    ComPtr<IAudioClient> audio_client;
    if (SUCCEEDED(hr)) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(audio_client.GetAddressOf()));
    } else if (last_start_error_.empty()) {
        last_start_error_ = "GetDefaultAudioEndpoint failed " + HrToHex(hr);
    }

    // 16kHz 单声道 PCM16 + AUTOCONVERTPCM：shared mode 下由音频引擎从设备
    // mix format 自动重采样（对齐 wasapi_virtual_mic_renderer 的做法），
    // 采集端无需自写重采样。EVENTCALLBACK：包就绪即触发事件驱动读取。
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = 16000 * 2;
    format.cbSize = 0;

    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
                               | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    // 缓冲 100ms：兼顾唤醒频率（约 10Hz）与容错余量，按住说话延迟无感。
    const REFERENCE_TIME buffer_duration = 100 * 10000i64;
    HANDLE event = nullptr;
    ComPtr<IAudioCaptureClient> capture_client;
    if (SUCCEEDED(hr)) {
        hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                      buffer_duration, 0, &format, nullptr);
    } else if (last_start_error_.empty()) {
        last_start_error_ = "IAudioClient Activate failed " + HrToHex(hr);
    }
    if (SUCCEEDED(hr)) {
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        hr = (event != nullptr) ? audio_client->SetEventHandle(event) : E_FAIL;
    } else if (last_start_error_.empty()) {
        last_start_error_ = "IAudioClient Initialize failed " + HrToHex(hr);
    }
    if (SUCCEEDED(hr)) {
        hr = audio_client->GetService(__uuidof(IAudioCaptureClient),
                                      reinterpret_cast<void**>(capture_client.GetAddressOf()));
    } else if (last_start_error_.empty()) {
        last_start_error_ = "SetEventHandle failed " + HrToHex(hr);
    }
    if (SUCCEEDED(hr)) {
        hr = audio_client->Start();
    } else if (last_start_error_.empty()) {
        last_start_error_ = "GetService(IAudioCaptureClient) failed " + HrToHex(hr);
    }
    if (FAILED(hr)) {
        if (last_start_error_.empty()) {
            last_start_error_ = "IAudioClient Start failed " + HrToHex(hr);
        }
        LogApp("WasapiMicCapture: open failed: " + last_start_error_);
        if (event != nullptr) CloseHandle(event);
        started_ok->store(false);
        open_done->store(true);
        if (com_initialized) CoUninitialize();
        return;
    }

    started_ok->store(true);
    open_done->store(true);
    LogApp("WasapiMicCapture: capturing from default microphone (16kHz mono)");

    std::vector<std::int16_t> pcm;
    while (!stop_requested_.load()) {
        // 事件最长等 50ms：既是设备无输入（静音包也可能不频繁）时的退出检查节拍，
        // 也覆盖事件句柄异常不触发（AUDCLNT_S_BUFFER_EMPTY 空转）的兜底轮询。
        if (WaitForSingleObject(event, 50) != WAIT_OBJECT_0 && !stop_requested_.load()) {
            continue;
        }
        while (!stop_requested_.load()) {
            UINT32 packet_frames = 0;
            if (FAILED(capture_client->GetNextPacketSize(&packet_frames)) ||
                packet_frames == 0) {
                break;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_client->GetBuffer(&data, &frames, &flags,
                                                 nullptr, nullptr))) {
                break;
            }
            if (frames > 0) {
                // 静音包补零喂入（不丢包）：与 StickS3 设备路径行为对齐——保持
                // 帧时间线连续，全程无声的按住会话由 ASR 返回空文本干净收尾，
                // 而不是 0 帧触发 "No audio frames" 错误。
                if (data != nullptr && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    const auto* samples = reinterpret_cast<const std::int16_t*>(data);
                    pcm.assign(samples, samples + frames);
                } else {
                    pcm.assign(frames, 0);
                }
                if (on_pcm) on_pcm(pcm);
            }
            capture_client->ReleaseBuffer(frames);
        }
    }

    audio_client->Stop();
    if (event != nullptr) CloseHandle(event);
    if (com_initialized) CoUninitialize();
    LogApp("WasapiMicCapture: stopped");
}

}  // namespace voicestick
