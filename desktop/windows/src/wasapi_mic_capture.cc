// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "wasapi_mic_capture.h"

#include "log.h"

#include <windows.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
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

// 窄字符串（endpoint id 为 ASCII 形态的 "{guid}.{hex}"）转宽字符。
std::wstring ToWide(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

}  // namespace

WasapiMicCapture::~WasapiMicCapture() {
    Stop();
}

void WasapiMicCapture::SetPreferredEndpointId(const std::string& endpoint_id) {
    preferred_endpoint_id_ = ToWide(endpoint_id);
}

bool WasapiMicCapture::Start() {
    if (capture_thread_.joinable()) return true;  // 已在采集：幂等
    last_start_error_.clear();
    stop_requested_.store(false);
    // 设备打开挪到采集线程（CaptureThreadMain 内 COM 初始化与设备激活同线程），
    // Start 本身只负责起线程；打开失败经 last_start_error_ 暴露，由协调器会话
    // 收尾路径消费。为让失败同步可见，这里 join 等待首帧或失败信号。
    std::atomic_bool started_ok{false};
    std::atomic_bool open_done{false};
    capture_thread_ = std::thread([this, &started_ok, &open_done] {
        CaptureThreadMain(&started_ok, &open_done);
    });
    while (!open_done.load()) {
        std::this_thread::yield();
    }
    if (!started_ok.load()) {
        capture_thread_.join();
        capture_thread_ = std::thread{};
        return false;
    }
    return true;
}

void WasapiMicCapture::Stop() {
    if (!capture_thread_.joinable()) return;
    stop_requested_.store(true);
    // CaptureThreadMain 在事件/包处理间隙检查 stop_requested_，最长一个包周期内退出。
    capture_thread_.join();
    capture_thread_ = std::thread{};
}

void WasapiMicCapture::CaptureThreadMain(std::atomic_bool* started_ok,
                                         std::atomic_bool* open_done) {
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
    if (SUCCEEDED(hr) && !preferred_endpoint_id_.empty()) {
        // 钉扎端点：wechat 方案 A 场景，默认设备已被切到虚拟麦，必须按保存的
        // 真实麦克风端点 id 打开。失败不回退默认设备（那会采到虚拟麦回环），
        // 如实报错让会话回滚。
        hr = enumerator->GetDevice(preferred_endpoint_id_.c_str(),
                                   device.GetAddressOf());
        if (FAILED(hr)) {
            last_start_error_ = "IMMDeviceEnumerator::GetDevice(preferred) failed " + HrToHex(hr);
        }
    } else if (SUCCEEDED(hr)) {
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
    LogApp(preferred_endpoint_id_.empty()
               ? "WasapiMicCapture: capturing from default microphone (16kHz mono)"
               : "WasapiMicCapture: capturing from pinned endpoint (16kHz mono)");

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
