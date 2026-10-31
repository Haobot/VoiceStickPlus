#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace voicestick {
// Crockford Base32：字母表 0-9 A-Z 去掉 I L O U；解码大小写不敏感，
// 忽略 '-' '_' 空白；输入须为整数个字符对应整数字节数（调用方定长 127→79）。
std::string SerialBase32Encode(std::span<const std::uint8_t> data);
std::optional<std::vector<std::uint8_t>> SerialBase32Decode(const std::string& text);
}  // namespace voicestick
