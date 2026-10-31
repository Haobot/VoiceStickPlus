#pragma once

#include <optional>
#include <string>

namespace voicestick {

// 读 HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid；失败（权限/无键）返回 nullopt。
// 返回值已 trim 并转小写 UTF-8（绑定键计算在 license.cc 内统一按此规格）。
std::optional<std::string> ReadMachineGuid();

}  // namespace voicestick
