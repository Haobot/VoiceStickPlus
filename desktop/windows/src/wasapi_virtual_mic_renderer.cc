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

// WASAPI 渲染端的真实 COM 实现：枚举设备、Initialize（AUTOCONVERTPCM 重采样 +
// EVENTCALLBACK 事件驱动）、暴露 GetBuffer/ReleaseBuffer/CurrentPadding 与通知事件。
// 事件驱动下渲染线程阻塞于 NotifyEvent，由 WASAPI 在 buffer 需填充时唤醒，
// 提交速率跟随设备实际消费，消除 sleep 轮询导致的稳态 underrun。
class ComWasapiRenderSink : public WasapiRenderSink {
 public:
  ComWasapiRenderSink() = default;
  ~ComWasapiRenderSink() override { Cleanup(); }

  bool OpenAndInitialize(const IVirtualMicRenderer::Options& options) override {
    // renderer 在 session 间复用，Stop 不销毁 sink，第二次 Start 会重入此方法。
    // 先释放上次会话的 COM 资源（audio_client/event/CoUninit），再重建，避免泄漏与
    // 引用计数失衡。复刻改造前 Impl::Stop→Cleanup 的释放语义。
    if (audio_client_ != nullptr) {
      Cleanup();
    }
    options_ = options;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      return false;
    }
    com_initialized_ = (hr == S_OK || hr == S_FALSE);

    if (!OpenDevice() || !InitializeStream()) {
      Cleanup();
      return false;
    }
    return true;
  }

  UINT32 BufferFrameCount() const override { return buffer_frame_count_; }

  bool CurrentPadding(UINT32* out) override {
    return audio_client_ != nullptr &&
           SUCCEEDED(audio_client_->GetCurrentPadding(out));
  }

  bool GetBuffer(UINT32 frames, BYTE** out) override {
    return render_client_ != nullptr &&
           SUCCEEDED(render_client_->GetBuffer(frames, out));
  }

  void ReleaseBuffer(UINT32 frames) override {
    if (render_client_ != nullptr) {
      render_client_->ReleaseBuffer(frames, 0);
    }
  }

  void Start() override {
    if (audio_client_ != nullptr) {
      audio_client_->Start();
    }
  }

  void Stop() override {
    if (audio_client_ != nullptr) {
      audio_client_->Stop();
    }
  }

  HANDLE NotifyEvent() override { return event_; }

  // 设备名仅 COM 实现有意义；WasapiVirtualMicRenderer 通过 dynamic_cast 取用。
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
    // EVENTCALLBACK：事件驱动，WASAPI 在 buffer 需填充时触发 NotifyEvent，
    // 渲染线程阻塞等待而非 sleep 轮询，提交速率与设备消费解耦。
    const DWORD stream_flags = AUDCLNT_STREAMFLAGS_NOPERSIST
                               | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
                               | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
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

    // auto-reset event：WASAPI 触发后自动复位，渲染线程 WaitForSingleObject 阻塞。
    event_ = CreateEventW(nullptr, /*manual_reset=*/FALSE, /*initial=*/FALSE, nullptr);
    if (event_ == nullptr) {
      LogApp("WASAPI InitializeStream: CreateEventW failed " + HrToHex(hr));
      return false;
    }
    hr = audio_client_->SetEventHandle(event_);
    if (FAILED(hr)) {
      LogApp("WASAPI InitializeStream: SetEventHandle failed " + HrToHex(hr));
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

    // 不在此 Start；交给 WasapiVirtualMicRenderer::Start 在渲染线程起好前调用，
    // 避免 Start 后事件先于线程就绪触发被丢弃。
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
    if (event_ != nullptr) {
      CloseHandle(event_);
      event_ = nullptr;
    }
    if (com_initialized_) {
      CoUninitialize();
      com_initialized_ = false;
    }
  }

  IVirtualMicRenderer::Options options_;
  ComPtr<IMMDevice> device_;
  ComPtr<IAudioClient> audio_client_;
  ComPtr<IAudioRenderClient> render_client_;
  HANDLE event_ = nullptr;
  UINT32 buffer_frame_count_ = 0;
  std::wstring active_device_name_;
  bool com_initialized_ = false;
};

// 渲染迭代：读 padding → 算可提交（buffer_frame_count - padding，不再受 render_period
// 上限约束）→ GetBuffer → 从 ring 读，不足补静音 → ReleaseBuffer。提交量严格跟随 WASAPI
// 实际腾出的空间，保证稳态消费 1.0×，消除 underrun 静音。
UINT32 RenderPump::PumpOnce() {
  if (sink_ == nullptr) {
    return 0;
  }

  UINT32 padding = 0;
  if (!sink_->CurrentPadding(&padding)) {
    return 0;
  }
  const UINT32 total = sink_->BufferFrameCount();
  const UINT32 available = (padding < total) ? (total - padding) : 0;
  if (available == 0) {
    return 0;
  }

  BYTE* buffer = nullptr;
  if (!sink_->GetBuffer(available, &buffer) || buffer == nullptr) {
    return 0;
  }

  const std::size_t samples_to_read =
      static_cast<std::size_t>(available) * samples_per_frame_;
  if (read_buffer_.size() < samples_to_read) {
    read_buffer_.resize(samples_to_read);
  }
  std::size_t read = 0;
  if (source_ != nullptr) {
    read = source_->Read(read_buffer_.data(), samples_to_read, 0);
  }
  // ring 数据不足时补静音，避免播放残留或 pop 噪声。
  if (read < samples_to_read) {
    std::fill(read_buffer_.begin() + read,
              read_buffer_.begin() + samples_to_read,
              static_cast<int16_t>(0));
  }
  std::memcpy(buffer, read_buffer_.data(), samples_to_read * sizeof(int16_t));
  sink_->ReleaseBuffer(available);
  return available;
}

class WasapiVirtualMicRenderer::Impl {
 public:
  Impl(const Options& options, std::unique_ptr<WasapiRenderSink> sink)
      : options_(options) {
    if (sink != nullptr) {
      sink_ = std::move(sink);
    } else {
      sink_ = std::make_unique<ComWasapiRenderSink>();
    }
  }
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

    if (!sink_->OpenAndInitialize(options_)) {
      running_ = false;
      return false;
    }

    // 先 Start 设备消费，再起渲染线程阻塞等事件。Start 后首帧事件可能立即触发，
    // 此时线程已就绪可处理。OpenAndInitialize 内已 Initialize 但未 Start（见注释）。
    sink_->Start();
    render_thread_ = std::thread(&Impl::RenderThreadFunc, this);
    return true;
  }

  void Stop() {
    if (!running_.exchange(false)) {
      return;
    }

    // 先唤醒阻塞在 WaitForSingleObject(INFINITE) 的渲染线程，再 join，避免死等。
    // WASAPI Stop 后不再触发事件，必须显式 SetEvent。
    HANDLE ev = sink_ ? sink_->NotifyEvent() : nullptr;
    if (ev != nullptr) {
      SetEvent(ev);
    }

    if (render_thread_.joinable()) {
      render_thread_.join();
    }

    if (sink_ != nullptr) {
      sink_->Stop();
      // 不 reset sink：renderer 在 session 间复用，下次 Start 会重入
      // sink_->OpenAndInitialize（内部先 Cleanup 旧 COM 资源再重建）。销毁 sink 会
      // 使第二次 Start 解引用 nullptr 崩溃（WER 0xc0000005）。
    }
    source_ = nullptr;
  }

  bool IsRunning() const { return running_; }

  std::wstring ActiveDeviceName() const {
    auto* com = dynamic_cast<ComWasapiRenderSink*>(sink_.get());
    return com != nullptr ? com->ActiveDeviceName() : std::wstring();
  }

 private:
  void RenderThreadFunc() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool thread_com_initialized = SUCCEEDED(hr);

    RenderPump pump(sink_.get(), source_, options_.channels);

    while (running_) {
      // 阻塞等 WASAPI 通知 render buffer 可填充。auto-reset event 触发后自动复位。
      WaitForSingleObject(sink_->NotifyEvent(), INFINITE);
      if (!running_) {
        break;
      }
      pump.PumpOnce();
    }

    if (thread_com_initialized) {
      CoUninitialize();
    }
  }

  const Options options_;
  std::atomic<bool> running_{false};
  PcmRingBuffer* source_ = nullptr;
  std::thread render_thread_;
  std::unique_ptr<WasapiRenderSink> sink_;
};

WasapiVirtualMicRenderer::WasapiVirtualMicRenderer(const Options& options)
    : impl_(std::make_unique<Impl>(options, nullptr)) {}

WasapiVirtualMicRenderer::WasapiVirtualMicRenderer(
    const Options& options, std::unique_ptr<WasapiRenderSink> sink)
    : impl_(std::make_unique<Impl>(options, std::move(sink))) {}

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

RenderPump::RenderPump(WasapiRenderSink* sink, PcmRingBuffer* source, int channels)
    : sink_(sink), source_(source), samples_per_frame_(channels) {}

RenderPump::~RenderPump() = default;

}  // namespace voicestick
