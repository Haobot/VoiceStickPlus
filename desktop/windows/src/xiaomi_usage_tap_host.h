#pragma once

#include <optional>
#include <cstdint>

#include <windows.h>

namespace voicestick {

// 定位承载小米遥控器 HID 服务的 WUDFHost 进程 PID（方案
// Doc/Plan/xiaomi-remote-usage-tap.md §3.2.3）。
//
// 遍历 HKLM\SYSTEM\CurrentControlSet\Enum\BTHLEDevice 下
// {00001812-…}(HID 服务)前缀容器，键名含小米 VID/PID（复用
// XiaomiRawInputNameIsRemote 的低 16 位比对，兼容 6 位/4 位十六进制格式），
// 读 <instance>\Device Parameters\WUDFDiagnosticInfo 的 HostPid。
// 注册表路径与 HostPid 值为 MiVibe-Remote 真机验证事实，本项目按事实重实现。
//
// 返回：已配对且 HID 服务有活跃 WUDF 宿主 → PID；否则 nullopt。
// 主程序用于注入监视（PID 变化 = 宿主重启需重注入），注入器用于注入前校验。
std::optional<DWORD> FindXiaomiHidHostPid();

// WUDFDiagnosticInfo\HostPid 注册表值 → PID（纯逻辑可单测）。真机实测该值
// 为 REG_QWORD（Win11 26200），MiVibe 参考实现（Python winreg 整型读）与
// REG_DWORD 兼容均按低 32 位取值；类型不符/字节数不足返回 nullopt。
std::optional<DWORD> ParseHostPidValue(DWORD type, const uint8_t* data,
                                       DWORD size);

} // namespace voicestick
