#include "xiaomi_usage_tap_host.h"

#include <cstdint>
#include <string>

#include "xiaomi_keymap_interceptor.h"

namespace voicestick {
namespace {

constexpr wchar_t kBthleEnumKey[] =
    L"SYSTEM\\CurrentControlSet\\Enum\\BTHLEDevice";
// HID over GATT 服务 GUID 前缀（BTHLE 容器键名形如
// "{00001812-0000-1000-8000-00805f9b34fb}_Dev_VID&012717_PID&32b8_REV&00a4_…"）。
constexpr wchar_t kHidServicePrefix[] = L"{00001812-0000-1000-8000-00805f9b34fb}";
constexpr wchar_t kWudfDiagnosticSuffix[] =
    L"Device Parameters\\WUDFDiagnosticInfo";

} // namespace

std::optional<DWORD> ParseHostPidValue(DWORD type, const uint8_t* data,
                                       DWORD size) {
    if (data == nullptr) return std::nullopt;
    uint64_t value = 0;
    if (type == REG_QWORD && size >= sizeof(uint64_t)) {
        memcpy(&value, data, sizeof(value));  // 注册表整型均小端
    } else if (type == REG_DWORD && size >= sizeof(DWORD)) {
        uint32_t low = 0;
        memcpy(&low, data, sizeof(low));
        value = low;
    } else {
        return std::nullopt;
    }
    if (value == 0) return std::nullopt;
    return static_cast<DWORD>(value);  // PID 恒 < 2^32，取低 32 位
}

std::optional<DWORD> FindXiaomiHidHostPid() {
    HKEY enum_root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kBthleEnumKey, 0, KEY_READ,
                      &enum_root) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::optional<DWORD> host_pid;
    // 外层：BTHLEDevice\<service 容器>（HID 服务前缀 + 小米 VID/PID）。
    for (DWORD service_index = 0;; ++service_index) {
        wchar_t service_name[512];
        DWORD service_name_len = 512;
        const LSTATUS service_status =
            RegEnumKeyExW(enum_root, service_index, service_name,
                          &service_name_len, nullptr, nullptr, nullptr, nullptr);
        if (service_status == ERROR_NO_MORE_ITEMS) break;
        if (service_status != ERROR_SUCCESS) continue;
        const std::wstring service(service_name, service_name_len);
        if (service.compare(0, wcslen(kHidServicePrefix), kHidServicePrefix) != 0) {
            continue;
        }
        if (!XiaomiRawInputNameIsRemote(service)) continue;
        // 内层：<service>\<instance> 的 WUDFDiagnosticInfo\HostPid。
        HKEY service_key = nullptr;
        if (RegOpenKeyExW(enum_root, service.c_str(), 0, KEY_READ,
                          &service_key) != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD instance_index = 0;; ++instance_index) {
            wchar_t instance_name[512];
            DWORD instance_name_len = 512;
            const LSTATUS instance_status =
                RegEnumKeyExW(service_key, instance_index, instance_name,
                              &instance_name_len, nullptr, nullptr, nullptr,
                              nullptr);
            if (instance_status == ERROR_NO_MORE_ITEMS) break;
            if (instance_status != ERROR_SUCCESS) continue;
            const std::wstring diagnostic =
                service + L"\\" +
                std::wstring(instance_name, instance_name_len) + L"\\" +
                kWudfDiagnosticSuffix;
            HKEY diagnostic_key = nullptr;
            if (RegOpenKeyExW(enum_root, diagnostic.c_str(), 0, KEY_READ,
                              &diagnostic_key) != ERROR_SUCCESS) {
                continue;
            }
            // 真机实测 HostPid 为 REG_QWORD（Win11 26200）；缓冲按 8 字节给足，
            // 由 ParseHostPidValue 统一按类型取值。
            uint8_t pid_data[8] = {};
            DWORD pid_size = sizeof(pid_data);
            DWORD pid_type = 0;
            if (RegQueryValueExW(diagnostic_key, L"HostPid", nullptr,
                                 &pid_type, pid_data,
                                 &pid_size) == ERROR_SUCCESS) {
                const auto pid = ParseHostPidValue(pid_type, pid_data, pid_size);
                if (pid.has_value()) host_pid = pid;
            }
            RegCloseKey(diagnostic_key);
            if (host_pid.has_value()) break;
        }
        RegCloseKey(service_key);
        if (host_pid.has_value()) break;
    }
    RegCloseKey(enum_root);
    return host_pid;
}

} // namespace voicestick
