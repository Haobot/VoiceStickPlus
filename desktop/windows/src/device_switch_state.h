// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 默认录音设备切换状态持久化：供崩溃自愈用。
// Start 切换前写入 switched=true + 原设备 ID；Stop 切回后清除。
// 启动时若 switched=true 且无活跃会话（上次崩溃未切回）→ Restore + 清除。

#ifndef VOICESTICK_DEVICE_SWITCH_STATE_H_
#define VOICESTICK_DEVICE_SWITCH_STATE_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace voicestick {

// wstring ↔ UTF-8 转换（CP_UTF8，供状态文件持久化与协调器交换 endpoint id/name）。
std::string WStringToUtf8(std::wstring_view w);
std::wstring Utf8ToWString(std::string_view s);

struct DeviceSwitchState {
    bool switched = false;
    // UTF-8。endpoint id（IMMDevice::GetId 返回，形如 "{0.0.1.00000000}.{xxx}"，内容为 ASCII）。
    std::string saved_default_capture_id;
    // UTF-8。friendly name（仅日志参考，可能含中文）。
    std::string saved_default_capture_name;
};

// 读写 default_device_switch_state.json。纯函数，path 参数，可单测。
// Load：文件不存在视为未切换（out 清空后 switched=false，返回 true）。解析失败返回 false。
bool LoadDeviceSwitchState(const std::filesystem::path& path, DeviceSwitchState& out);
// Save：覆盖写入。失败返回 false。
bool SaveDeviceSwitchState(const std::filesystem::path& path, const DeviceSwitchState& state);
// Clear：删除状态文件（切回后调用）。文件不存在视为成功。
bool ClearDeviceSwitchState(const std::filesystem::path& path);

}  // namespace voicestick

#endif  // VOICESTICK_DEVICE_SWITCH_STATE_H_
