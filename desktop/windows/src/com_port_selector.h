#pragma once

#include <string>
#include <utility>
#include <vector>

namespace voicestick {

// 串口信息（Windows SetupAPI 枚举结果）。
struct ComPortInfo {
    std::wstring device;         // 设备名，如 L"COM5"
    std::wstring description;     // 友好名，如 L"USB JTAG/serial debug unit (COM5)"
    std::wstring manufacturer;    // 制造商，如 L"Espressif Systems"
    std::wstring hardware_id;    // 硬件 ID，如 L"USB\\VID_303A&PID_1001\\ABC123"
};

// 按评分规则给 COM 端口打分。与设备无关的端口返回 0。
// 评分权重（与 scripts/idf_cli.yaml serial_detection 一致）：
//   - 描述关键字命中：每个 +30
//   - 制造商关键字命中：每个 +20
//   - 硬件 ID VID 命中：+40
//   - preferred_vid_pid 精确命中：+160
int ScoreComPort(const ComPortInfo& port,
                 const std::vector<std::pair<int, int>>& preferred_vid_pid = {});

// 默认优先 VID:PID 列表（ESP32-S3 原生 USB）。
const std::vector<std::pair<int, int>>& DefaultPreferredVidPid();

// 从候选端口中选择得分最高者。全部 0 分时返回 nullptr。
// 同分时取 COM 编号最小者。
const ComPortInfo* SelectBestComPort(
    const std::vector<ComPortInfo>& ports,
    const std::vector<std::pair<int, int>>& preferred = {});

// 从设备名提取 COM 端号（如 L"COM5" → 5）。非 COM 名返回 INT_MAX。
int ComPortNumber(const std::wstring& device);

// 用 Windows SetupAPI 枚举当前可用 COM 端口（需链接 setupapi.lib）。
std::vector<ComPortInfo> EnumerateComPorts();

} // namespace voicestick
