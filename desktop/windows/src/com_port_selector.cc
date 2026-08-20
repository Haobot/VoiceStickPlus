#include "com_port_selector.h"

#include <Windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")  // RegQueryValueExW（串口名读取）
#endif

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <string>

namespace voicestick {

namespace {

// 宽字符转小写（原地）。
std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// 描述关键字（与 scripts/idf_cli.yaml serial_detection.description_keywords 一致）。
const std::vector<std::wstring>& DescriptionKeywords() {
    static const std::vector<std::wstring> kKeywords = {
        L"esp32", L"usb serial", L"usb-serial", L"jtag", L"uart",
        L"cp210", L"ch340", L"ch343", L"wch", L"ftdi",
    };
    return kKeywords;
}

// 制造商关键字（与 idf_cli.yaml serial_detection.manufacturer_keywords 一致）。
const std::vector<std::wstring>& ManufacturerKeywords() {
    static const std::vector<std::wstring> kKeywords = {
        L"espressif", L"wch", L"silicon labs", L"ftdi",
    };
    return kKeywords;
}

// 已知 VID 列表（与 idf_cli.yaml serial_detection.hwid_keywords 的 VID 前缀一致）。
const std::vector<int>& KnownVids() {
    static const std::vector<int> kVids = {
        0x303A, 0x1A86, 0x2BDF, 0x10C4, 0x0403,
    };
    return kVids;
}

// 从硬件 ID 字符串解析 VID 和 PID。
// 格式如 "USB\\VID_303A&PID_1001\\ABC123"，返回 {0x303A, 0x1001}。
struct VidPid {
    int vid = -1;
    int pid = -1;
};

VidPid ParseVidPid(const std::wstring& hardware_id) {
    VidPid result;
    const std::wstring lower = ToLower(hardware_id);

    // 查找 "vid_" 并解析后续十六进制。
    const auto vid_pos = lower.find(L"vid_");
    if (vid_pos != std::wstring::npos) {
        std::wstring hex;
        for (std::size_t i = vid_pos + 4; i < lower.size(); ++i) {
            wchar_t c = lower[i];
            if (std::iswxdigit(c)) {
                hex.push_back(c);
            } else {
                break;
            }
        }
        if (!hex.empty()) {
            result.vid = static_cast<int>(std::wcstoul(hex.c_str(), nullptr, 16));
        }
    }

    // 查找 "pid_" 并解析后续十六进制。
    const auto pid_pos = lower.find(L"pid_");
    if (pid_pos != std::wstring::npos) {
        std::wstring hex;
        for (std::size_t i = pid_pos + 4; i < lower.size(); ++i) {
            wchar_t c = lower[i];
            if (std::iswxdigit(c)) {
                hex.push_back(c);
            } else {
                break;
            }
        }
        if (!hex.empty()) {
            result.pid = static_cast<int>(std::wcstoul(hex.c_str(), nullptr, 16));
        }
    }

    return result;
}

} // namespace

int ScoreComPort(const ComPortInfo& port,
                 const std::vector<std::pair<int, int>>& preferred_vid_pid) {
    int score = 0;

    // 描述关键字：每个命中 +30。
    const std::wstring desc = ToLower(port.description);
    for (const auto& kw : DescriptionKeywords()) {
        if (desc.find(kw) != std::wstring::npos) score += 30;
    }

    // 制造商关键字：每个命中 +20。
    const std::wstring mfr = ToLower(port.manufacturer);
    for (const auto& kw : ManufacturerKeywords()) {
        if (mfr.find(kw) != std::wstring::npos) score += 20;
    }

    // 硬件 ID VID 命中：+40。
    const VidPid vp = ParseVidPid(port.hardware_id);
    for (int known_vid : KnownVids()) {
        if (vp.vid == known_vid) {
            score += 40;
            break;
        }
    }

    // preferred VID:PID 精确命中：+160。
    if (vp.vid >= 0 && vp.pid >= 0) {
        for (const auto& pp : preferred_vid_pid) {
            if (vp.vid == pp.first && vp.pid == pp.second) {
                score += 160;
                break;
            }
        }
    }

    return score;
}

const std::vector<std::pair<int, int>>& DefaultPreferredVidPid() {
    static const std::vector<std::pair<int, int>> kDefault = {{0x303A, 0x1001}};
    return kDefault;
}

int ComPortNumber(const std::wstring& device) {
    // 检查是否以 "COM" 开头（大小写不敏感）。
    if (device.size() < 4) return INT_MAX;
    if (std::towlower(device[0]) != L'c' ||
        std::towlower(device[1]) != L'o' ||
        std::towlower(device[2]) != L'm') {
        return INT_MAX;
    }
    // 解析 "COM" 后的数字。
    int num = 0;
    bool any_digit = false;
    for (std::size_t i = 3; i < device.size(); ++i) {
        if (!std::iswdigit(device[i])) break;
        num = num * 10 + (device[i] - L'0');
        any_digit = true;
    }
    return any_digit ? num : INT_MAX;
}

const ComPortInfo* SelectBestComPort(
    const std::vector<ComPortInfo>& ports,
    const std::vector<std::pair<int, int>>& preferred) {
    const ComPortInfo* best = nullptr;
    int best_score = 0;
    int best_com = INT_MAX;

    for (const auto& port : ports) {
        const int score = ScoreComPort(port, preferred);
        if (score <= 0) continue;
        const int com = ComPortNumber(port.device);
        // 选高分者；同分取 COM 编号最小者。
        if (score > best_score ||
            (score == best_score && com < best_com)) {
            best = &port;
            best_score = score;
            best_com = com;
        }
    }

    return best;  // 全部 0 分时为 nullptr
}

std::vector<ComPortInfo> EnumerateComPorts() {
    std::vector<ComPortInfo> ports;

    // 串口设备类 GUID：{4D36E978-E325-11CE-BFC1-08002BE10318}
    static const GUID kPortsClassGuid = {
        0x4D36E978, 0xE325, 0x11CE,
        {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

    HDEVINFO dev_info = SetupDiGetClassDevsW(&kPortsClassGuid, nullptr, nullptr,
                                             DIGCF_PRESENT);
    if (dev_info == INVALID_HANDLE_VALUE) return ports;

    SP_DEVINFO_DATA dev_data;
    dev_data.cbSize = sizeof(dev_data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data); ++i) {
        ComPortInfo port;
        WCHAR buffer[512] = {};
        DWORD prop_type = 0;

        // 友好名（含 COM 端口号，如 "USB JTAG/serial debug unit (COM5)"）；
        // 无友好名时回退到设备描述。
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_FRIENDLYNAME, &prop_type,
                reinterpret_cast<BYTE*>(buffer), sizeof(buffer), nullptr) ||
            SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_DEVICEDESC, &prop_type,
                reinterpret_cast<BYTE*>(buffer), sizeof(buffer), nullptr)) {
            port.description = buffer;
        }

        // 制造商。
        buffer[0] = L'\0';
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_MFG, &prop_type,
                reinterpret_cast<BYTE*>(buffer), sizeof(buffer), nullptr)) {
            port.manufacturer = buffer;
        }

        // 硬件 ID（如 "USB\\VID_303A&PID_1001\\ABC123"）。
        buffer[0] = L'\0';
        if (SetupDiGetDeviceRegistryPropertyW(
                dev_info, &dev_data, SPDRP_HARDWAREID, &prop_type,
                reinterpret_cast<BYTE*>(buffer), sizeof(buffer), nullptr)) {
            port.hardware_id = buffer;
        }

        // 从注册表设备键读取 PortName（如 "COM5"）。
        HKEY hkey = SetupDiOpenDevRegKey(dev_info, &dev_data, DICS_FLAG_GLOBAL, 0,
                                         DIREG_DEV, KEY_READ);
        if (hkey != nullptr && hkey != INVALID_HANDLE_VALUE) {
            WCHAR port_name[256] = {};
            DWORD port_name_size = sizeof(port_name);
            DWORD value_type = 0;
            if (RegQueryValueExW(hkey, L"PortName", nullptr, &value_type,
                                 reinterpret_cast<BYTE*>(port_name),
                                 &port_name_size) == ERROR_SUCCESS) {
                port.device = port_name;
            }
            RegCloseKey(hkey);
        }

        ports.push_back(std::move(port));
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return ports;
}

} // namespace voicestick
