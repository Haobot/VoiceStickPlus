// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "virtual_mic_renamer.h"

#include "app_config.h"
#include "ble_protocol.h"
#include "log.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

namespace voicestick {

using Microsoft::WRL::ComPtr;

namespace {

// UTF-8（配置存储）↔ 宽字符（命令行/设备 API）转换。
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), len);
    return out;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<std::size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// IPolicyConfig COM 接口声明（未公开，与 default_audio_device_controller.cc 同源
// 移植自 SoundSwitch dev 分支；两处声明需保持 vtable 一致）。此处为
// SetPropertyValue 写设备属性（改名）所需。
// 来源：github.com/Belphemur/SoundSwitch @ dev
//   SoundSwitch.Audio.Manager/Interop/Interface/Policy/IPolicyConfig.cs
// CLSID_CPolicyConfigClient = 870AF99C-171D-4F9E-AF0D-E63DF40C2BC9
// IID_IPolicyConfig         = F8679F50-850A-41CF-9C72-430F290290C8
// vtable 顺序严格对应 C# 接口方法声明顺序，不可调换（错误顺序会调到错误方法崩溃）。
class DECLSPEC_UUID("870AF99C-171D-4F9E-AF0D-E63DF40C2BC9") PolicyConfigClient;

MIDL_INTERFACE("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfigRenamer : public IUnknown {
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
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(LPCWSTR, BOOL) = 0;
};

// 路径 1：IPolicyConfig::SetPropertyValue 经音频服务写 endpoint 属性（友好名）。
// bFxStore=FALSE 写设备属性存储（非 FX 特效存储）。同步调用，PROPVARIANT 的
// pwszVal 指向调用方内存（调用期间存活即可），不 PropVariantClear。
bool RenameViaPolicyConfig(const std::wstring& endpoint_id, const std::wstring& new_name) {
    ComPtr<IPolicyConfigRenamer> policy;
    HRESULT hr = CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfigRenamer),
                                  reinterpret_cast<void**>(policy.GetAddressOf()));
    if (FAILED(hr)) {
        LogApp("RenameVirtualMic: CoCreateInstance IPolicyConfig failed hr=0x" +
               std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_LPWSTR;
    value.pwszVal = const_cast<wchar_t*>(new_name.c_str());
    hr = policy->SetPropertyValue(endpoint_id.c_str(), FALSE, &PKEY_Device_FriendlyName, &value);
    if (FAILED(hr)) {
        LogApp("RenameVirtualMic: SetPropertyValue failed hr=0x" +
               std::to_string(static_cast<unsigned long>(hr)));
        return false;
    }
    return true;
}

// 路径 2 回退辅助：重启音频服务刷新 endpoint 属性缓存（Audiosrv 依赖
// AudioEndpointBuilder，先停 audiosrv 再停 builder，反序启动）。尽力而为：
// 每步独立执行不中断（某步失败后续仍尝试），返回全部步骤是否成功。
bool RunNetCommand(const wchar_t* args) {
    wchar_t cmd[64];
    swprintf_s(cmd, L"net.exe %s", args);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool RestartAudioServices() {
    const bool ok = RunNetCommand(L"stop Audiosrv") &&
                    RunNetCommand(L"stop AudioEndpointBuilder") &&
                    RunNetCommand(L"start AudioEndpointBuilder") &&
                    RunNetCommand(L"start Audiosrv");
    if (!ok) LogApp("RenameVirtualMic: audio service restart partially failed");
    return ok;
}

}  // namespace

std::wstring PickVirtualMicDisplayName(const AppConfig& config) {
    for (const auto& entry : config.paired_devices) {
        // 小米遥控器不做虚拟麦克风直通（仅 Windows ATVV 接入），跳过取 StickS3。
        if (entry.hardware == kHardwareXiaomiRemote2Pro) continue;
        if (!entry.name.empty()) return Utf8ToWide(entry.name);
        if (!entry.device_id.empty()) return L"VS-" + Utf8ToWide(entry.device_id);
    }
    return L"VoiceStick";
}

std::wstring EndpointRegistryKeyName(const std::wstring& endpoint_id) {
    // endpoint id 形如 "{0.0.1.00000000}.{aaaaaaaa-bbbb-...}"：
    // 取第一个 '}' 之后 '.' 的后缀，且后缀须为 "{...}" 形态。
    const auto brace_end = endpoint_id.find(L'}');
    if (brace_end == std::wstring::npos) return {};
    const auto dot = endpoint_id.find(L'.', brace_end);
    if (dot == std::wstring::npos) return {};
    std::wstring key = endpoint_id.substr(dot + 1);
    if (key.size() < 2 || key.front() != L'{' || key.back() != L'}') return {};
    return key;
}

bool WriteEndpointFriendlyNameToRegistry(HKEY root, const std::wstring& endpoint_id,
                                         const std::wstring& new_name) {
    const std::wstring key_name = EndpointRegistryKeyName(endpoint_id);
    if (key_name.empty() || new_name.empty()) return false;
    const std::wstring subkey =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\"
        + key_name + L"\\Properties";
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                     &key, nullptr);
    if (status != ERROR_SUCCESS) {
        LogApp("RenameVirtualMic: RegCreateKeyEx failed status=" +
               std::to_string(static_cast<unsigned long>(status)));
        return false;
    }
    status = RegSetValueExW(key, kEndpointFriendlyNameValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(new_name.c_str()),
                            static_cast<DWORD>((new_name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        LogApp("RenameVirtualMic: RegSetValueEx failed status=" +
               std::to_string(static_cast<unsigned long>(status)));
        return false;
    }
    return true;
}

int RunRenameVirtualMicElevated(const std::wstring& endpoint_id,
                                const std::wstring& new_name) {
    LogApp("RenameVirtualMic: begin endpoint=" + WideToUtf8(endpoint_id) +
           " name=" + WideToUtf8(new_name));
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_hr);  // S_OK/S_FALSE；RPC_E_CHANGED_MODE 时 COM 仍可用

    bool ok = RenameViaPolicyConfig(endpoint_id, new_name);
    const char* method = "policy_config";
    if (!ok) {
        // 回退：直接写注册表 + 重启音频服务（刷新 audiosrv 的 endpoint 属性缓存）。
        ok = WriteEndpointFriendlyNameToRegistry(HKEY_LOCAL_MACHINE, endpoint_id, new_name);
        if (ok) {
            RestartAudioServices();
            method = "registry+restart";
        }
    }

    if (com_initialized) CoUninitialize();
    LogApp(std::string("RenameVirtualMic: ") + (ok ? "ok" : "failed") + " via " + method);
    return ok ? 0 : 1;
}

bool LaunchRenameVirtualMicElevated(const std::wstring& exe_path,
                                    const std::wstring& endpoint_id,
                                    const std::wstring& new_name,
                                    std::string* error) {
    // 参数整体加引号：name 可能含空格，CommandLineToArgvW 按引号还原。
    const std::wstring params = L"--rename-virtual-mic \"" + endpoint_id + L"\" \"" +
                                new_name + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";  // 触发 UAC 提权
    sei.lpFile = exe_path.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) {
        const DWORD code = GetLastError();
        if (code == ERROR_CANCELLED) {
            if (error) *error = "uac cancelled";
        } else {
            if (error) *error = "launch failed " + std::to_string(code);
        }
        return false;
    }
    if (sei.hProcess != nullptr) {
        // 提权子进程改名 + 可能的音频服务重启，放宽超时避免误杀。
        const DWORD wait = WaitForSingleObject(sei.hProcess, 120000);
        DWORD exit_code = 1;
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(sei.hProcess, &exit_code);
        }
        CloseHandle(sei.hProcess);
        if (exit_code != 0) {
            if (error) *error = "exit code " + std::to_string(exit_code);
            return false;
        }
    }
    return true;
}

}  // namespace voicestick
