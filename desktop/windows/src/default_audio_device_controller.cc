// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "default_audio_device_controller.h"

#include "log.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

namespace voicestick {

using Microsoft::WRL::ComPtr;

namespace {

// 大小写不敏感子串匹配（friendly name 匹配，ASCII 足够匹配 "CABLE Output"）。
bool ContainsWide(std::wstring_view haystack, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto lower = [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c | 0x20) : c;
    };
    for (std::size_t i = 0; i <= haystack.size() - needle.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// DeviceRole(本接口) -> ERole(Windows SDK)。注意 ERole 顺序与 DeviceRole 不同。
ERole ERoleFromDeviceRole(DeviceRole role) {
    switch (role) {
        case DeviceRole::kConsole: return eConsole;
        case DeviceRole::kCommunications: return eCommunications;
        case DeviceRole::kMultimedia: return eMultimedia;
    }
    return eConsole;
}

std::string HrToHex(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

// IMMDevice -> {id, friendly_name}。
std::optional<AudioDeviceInfo> DeviceToInfo(IMMDevice* device) {
    if (device == nullptr) return std::nullopt;
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) return std::nullopt;
    std::wstring friendly;
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf())) && props != nullptr) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            friendly = value.pwszVal;
        }
        PropVariantClear(&value);
    }
    AudioDeviceInfo info{id, friendly};
    CoTaskMemFree(id);
    return info;
}

// IPolicyConfig COM 接口声明（未公开，按 SoundSwitch dev 分支源码移植）。
// 来源：github.com/Belphemur/SoundSwitch @ dev
//   SoundSwitch.Audio.Manager/Interop/Interface/Policy/IPolicyConfig.cs
//   SoundSwitch.Audio.Manager/Interop/Interface/ComGuid.cs
//   SoundSwitch.Audio.Manager/Interop/Client/PolicyClient.cs（CLSID 用法）
// CLSID_CPolicyConfigClient = 870AF99C-171D-4F9E-AF0D-E63DF40C2BC9
// IID_IPolicyConfig         = F8679F50-850A-41CF-9C72-430F290290C8
// vtable 顺序严格对应 C# 接口方法声明顺序，不可调换（错误顺序会调到错误方法崩溃）。
// 仅 SetDefaultEndpoint 用真实参数类型，其余方法参数用 void* 占位（不调用，仅占 vtable 槽）。
class DECLSPEC_UUID("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9") PolicyConfigClient;

MIDL_INTERFACE("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(LPCWSTR, BOOL, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(LPCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(LPCWSTR, BOOL, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(LPCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(LPCWSTR, BOOL, const PROPERTYKEY*, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(LPCWSTR, BOOL, const PROPERTYKEY*, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, BOOL) = 0;
};

}  // namespace

DefaultAudioDeviceController::DefaultAudioDeviceController() {
    // 协调器线程可能已初始化 COM（UI 线程通常 STA）。MTA 尝试，RPC_E_CHANGED_MODE
    // 表示已用其他模式初始化，COM 仍可用，不重复 CoUninitialize。
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized_ = (hr == S_OK || hr == S_FALSE);
}

DefaultAudioDeviceController::~DefaultAudioDeviceController() {
    if (com_initialized_) {
        CoUninitialize();
    }
}

std::optional<AudioDeviceInfo> DefaultAudioDeviceController::GetDefaultCapture(DeviceRole role) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr)) {
        LogApp("DefaultAudioDeviceController: CoCreateInstance enumerator failed " + HrToHex(hr));
        return std::nullopt;
    }
    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, ERoleFromDeviceRole(role), &device);
    if (FAILED(hr) || device == nullptr) return std::nullopt;
    return DeviceToInfo(device.Get());
}

std::optional<AudioDeviceInfo> DefaultAudioDeviceController::FindCaptureByName(
    std::wstring_view name_substring) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr)) return std::nullopt;
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return std::nullopt;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        auto info = DeviceToInfo(device.Get());
        if (info && ContainsWide(info->friendly_name, name_substring)) return info;
    }
    return std::nullopt;
}

bool DefaultAudioDeviceController::SetDefaultCapture(const std::wstring& device_id,
                                                   std::vector<DeviceRole> roles) {
    ComPtr<IPolicyConfig> policy;
    HRESULT hr = CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig),
                                  reinterpret_cast<void**>(policy.GetAddressOf()));
    if (FAILED(hr)) {
        LogApp("DefaultAudioDeviceController: CoCreateInstance IPolicyConfig failed " + HrToHex(hr));
        return false;
    }
    bool all_ok = true;
    for (DeviceRole role : roles) {
        hr = policy->SetDefaultEndpoint(device_id.c_str(), ERoleFromDeviceRole(role));
        if (FAILED(hr)) {
            LogApp("DefaultAudioDeviceController: SetDefaultEndpoint failed " + HrToHex(hr));
            all_ok = false;
        }
    }
    return all_ok;
}

}  // namespace voicestick
