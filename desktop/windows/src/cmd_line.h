#pragma once

#include <optional>
#include <string>

namespace voicestick {

// 命令行触发的 BLE OTA 请求。
struct OtaCliRequest {
    std::string file_path;                   // 本地固件 bin 路径（UTF-8）
    std::optional<std::string> device_id;    // 可选目标设备 ID（4 位十六进制）
};

// 解析命令行 argv 中的 --ota <path> [--device <id>]。
// argv[0] 视为程序名，从 argv[1] 起扫描；--ota 与 --device 顺序无关。
// 无 --ota 或 --ota 缺路径时返回 nullopt；--device 缺值时忽略该选项。
std::optional<OtaCliRequest> ParseOtaCliArgs(int argc, const wchar_t* const argv[]);

// 命令行触发的网关目标选择请求（P1 切换器）。
// self=true 表示"把本机设为网关目标"，false 表示清除选择（回到不限制）。
struct GatewayTargetCliRequest {
    bool self = true;
};

// 解析命令行 argv 中的 --gateway-target <self|clear>。argv[0] 视为程序名。
// 无该选项或取值非法时返回 nullopt。
std::optional<GatewayTargetCliRequest> ParseGatewayTargetCliArgs(int argc,
                                                                 const wchar_t* const argv[]);

} // namespace voicestick
