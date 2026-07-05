// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "wasapi_virtual_mic_renderer.h"

#include "log.h"
#include "pcm_ring_buffer.h"

#include <windows.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <thread>
#include <vector>

namespace voicestick {

using Microsoft::WRL::ComPtr;

namespace {

// 宽字符串大小写不敏感子串匹配。
bool ContainsCaseInsensitive(std::wstring_view haystack, std::wstring_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  auto lower = [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); };
  for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
    bool match = true;
    for (std::size_t j = 0; j < needle.size(); ++j) {
      if (lower(haystack[i + j]) != lower(needle[j])) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

std::wstring GetDeviceFriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> props;
  if (FAILED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
    return {};
  }

  PROPVARIANT value;
  PropVariantInit(&value);
  if (FAILED(props->GetValue(PKEY_Device_FriendlyName, &value)) ||
      value.vt != VT_LPWSTR || value.pwszVal == nullptr) {
    PropVariantClear(&value);
    return {};
  }

  std::wstring name(value.pwszVal);
  PropVariantClear(&value);
  return name;
}

std::string HrToHex(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

std::string Narrow(std::wstring_view s) {
  std::string out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    out.push_back(static_cast<char>(c));
  }
  return out;
}

}  // namespace

class WasapiVirtualMicRenderer::Impl {
 public:
  explicit Impl(const Options& options) : options_(options) {}
  ~Impl() { Stop(); }

  bool Start(PcmRingBuffer* source) {
    if (running_.exchange(true)) {
      return false;  // 已启动。
    }

    if (source == nullptr) {
      running_ = false;
      return false;
    }
    source_ = source;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      running_ = false;
      return false;
    }
    com_initialized_ = (hr == S_OK || hr == S_FALSE);

    if (!OpenDevice() || !InitializeStream()) {
      Cleanup();
      running_ = false;
      return false;
    }

    render_thread_ = std::thread(&Impl::RenderThreadFunc, this);
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) {
      return;
    }

    if (audio_client_ != nullptr) {
      audio_client_->Stop();
    }

    if (render_thread_.joinable()) {
      render_thread_.join();
    }

    Cleanup();
  }

  bool IsRunning() const { return running_; }

  std::wstring ActiveDeviceName() const { return active_device_name_; }

 private:
  bool OpenDevice() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
      LogApp("WASAPI OpenDevice: CoCreateInstance failed " + HrToHex(hr));
      return false;
    }

    // 如果未指定名称子串，则使用默认播放设备。
    if (options_.device_name_substring.empty()) {
      hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                  device_.GetAddressOf());
      if (SUCCEEDED(hr) && device_ != nullptr) {
        active_device_name_ = GetDeviceFriendlyName(device_.Get());
      } else {
        LogApp("WASAPI OpenDevice: GetDefaultAudioEndpoint failed " + HrToHex(hr));
      }
      return SUCCEEDED(hr) && device_ != nullptr;
    }

    ComPtr<IMMDeviceCollection> devices;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE,
                                          devices.GetAddressOf());
    if (FAILED(hr)) {
      LogApp("WASAPI OpenDevice: EnumAudioEndpoints failed " + HrToHex(hr));
      return false;
    }

    UINT count = 0;
    devices->GetCount(&count);
    std::string enumerated = "WASAPI OpenDevice: eRender/ACTIVE count="
                             + std::to_string(count);
    for (UINT i = 0; i < count; ++i) {
      ComPtr<IMMDevice> device;
      if (FAILED(devices->Item(i, device.GetAddressOf()))) {
        continue;
      }

      std::wstring name = GetDeviceFriendlyName(device.Get());
      enumerated += " [";
      enumerated += Narrow(name);
      enumerated += "]";
      if (ContainsCaseInsensitive(name, options_.device_name_substring)) {
        device_ = std::move(device);
        active_device_name_ = name;
        LogApp("WASAPI OpenDevice: matched \"" + Narrow(name) + "\"");
        return true;
      }
    }
    LogApp(enumerated);
    LogApp("WASAPI OpenDevice: no match for substring \""
           + Narrow(options_.device_name_substring) + "\"");
    return false;
  }

  bool InitializeStream() {
    assert(device_ != nullptr);

    HRESULT hr = device_
                     ->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(audio_client_.GetAddressOf()));
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: Activate failed " + HrToHex(hr));
      return false;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(options_.channels);
    format.nSamplesPerSec = static_cast<DWORD>(options_.sample_rate);
    format.wBitsPerSample = static_cast<WORD>(options_.bits_per_sample);
    format.nBlockAlign =
        static_cast<WORD>(options_.channels * options_.bits_per_sample / 8);
    format.nAvgBytesPerSec =
        format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    const REFERENCE_TIME buffer_duration =
        static_cast<REFERENCE_TIME>(options_.buffer_duration_ms) * 10000i64;
    // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY：shared mode 下让 WASAPI 自动把
    // 调用方 PCM 重采样到设备 mix format，避免 16kHz 在 48kHz 设备上被拒。
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_NOPERSIST
                               | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = audio_client_
             ->Initialize(AUDCLNT_SHAREMODE_SHARED,
                          stream_flags,
                          buffer_duration,
                          0,
                          &format,
                          nullptr);
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: Initialize failed " + HrToHex(hr) +
             " (format=" + std::to_string(options_.sample_rate) + "Hz/" +
             std::to_string(options_.channels) + "ch/" +
             std::to_string(options_.bits_per_sample) + "bit)");
      return false;
    }

    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: GetBufferSize failed " + HrToHex(hr));
      return false;
    }

    hr = audio_client_->GetService(
        IID_PPV_ARGS(render_client_.GetAddressOf()));
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: GetService failed " + HrToHex(hr));
      return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: Start failed " + HrToHex(hr));
      return false;
    }

    return true;
  }

  void Cleanup() {
    if (audio_client_ != nullptr) {
      audio_client_->Stop();
      audio_client_.Reset();
    }
    render_client_.Reset();
    device_.Reset();
    active_device_name_.clear();
    source_ = nullptr;

    if (com_initialized_) {
      CoUninitialize();
      com_initialized_ = false;
    }
  }

  void RenderThreadFunc() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool thread_com_initialized = SUCCEEDED(hr);

    const int samples_per_frame = options_.channels;
    const int frames_per_period =
        (options_.sample_rate * options_.render_period_ms) / 1000;
    const std::size_t read_buffer_size =
        static_cast<std::size_t>(frames_per_period * samples_per_frame);
    std::vector<int16_t> read_buffer(read_buffer_size);

    while (running_) {
      UINT32 padding = 0;
      if (audio_client_ != nullptr) {
        audio_client_->GetCurrentPadding(&padding);
      }

      const UINT32 available =
          (padding < buffer_frame_count_) ? (buffer_frame_count_ - padding) : 0;
      const UINT32 frames_to_render =
          std::min(available, static_cast<UINT32>(frames_per_period));

      if (frames_to_render > 0 && render_client_ != nullptr) {
        BYTE* buffer = nullptr;
        hr = render_client_->GetBuffer(frames_to_render, &buffer);
        if (SUCCEEDED(hr) && buffer != nullptr) {
          const std::size_t samples_to_read =
              static_cast<std::size_t>(frames_to_render * samples_per_frame);
          std::size_t read = 0;
          if (source_ != nullptr) {
            read = source_->Read(read_buffer.data(), samples_to_read, 0);
          }
          if (read < samples_to_read) {
            std::fill(read_buffer.begin() + read, read_buffer.begin() + samples_to_read, 0);
          }
          std::memcpy(buffer, read_buffer.data(),
                      samples_to_read * sizeof(int16_t));
          render_client_->ReleaseBuffer(frames_to_render, 0);
        }
      }

      std::this_thread::sleep_for(
          std::chrono::milliseconds(options_.render_period_ms));
    }

    if (thread_com_initialized) {
      CoUninitialize();
    }
  }

  const Options options_;
  std::atomic<bool> running_{false};
  PcmRingBuffer* source_ = nullptr;
  std::thread render_thread_;

  ComPtr<IMMDevice> device_;
  ComPtr<IAudioClient> audio_client_;
  ComPtr<IAudioRenderClient> render_client_;
  UINT32 buffer_frame_count_ = 0;
  std::wstring active_device_name_;
  bool com_initialized_ = false;
};

WasapiVirtualMicRenderer::WasapiVirtualMicRenderer(const Options& options)
    : impl_(std::make_unique<Impl>(options)) {}

WasapiVirtualMicRenderer::~WasapiVirtualMicRenderer() = default;

bool WasapiVirtualMicRenderer::Start(PcmRingBuffer* source) {
  return impl_->Start(source);
}

void WasapiVirtualMicRenderer::Stop() {
  impl_->Stop();
}

bool WasapiVirtualMicRenderer::IsRunning() const {
  return impl_->IsRunning();
}

std::wstring WasapiVirtualMicRenderer::ActiveDeviceName() const {
  return impl_->ActiveDeviceName();
}

}  // namespace voicestick
