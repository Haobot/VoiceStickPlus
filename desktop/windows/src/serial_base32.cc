#include "serial_base32.h"
#include <array>
namespace voicestick {
namespace {
constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr signed char kInvalid = -1;
constexpr std::array<signed char, 128> BuildDecodeTable() {
    std::array<signed char, 128> table;
    table.fill(kInvalid);
    for (int i = 0; i < 32; ++i) {
        const auto c = static_cast<unsigned char>(kAlphabet[i]);
        table[c] = static_cast<signed char>(i);
        // 只对字母做大小写归一；数字字符若套用 'A'..'Z' 位移会污染 'P'..'Y' 槽位
        // （如 '5' - 'A' + 'a' == 'U'，会使非法字符 U 被当成合法解码）。
        if (c >= 'A' && c <= 'Z') table[static_cast<unsigned char>(c - 'A' + 'a')] = static_cast<signed char>(i);
    }
    return table;
}
const std::array<signed char, 128>& DecodeTable() {
    static const auto kDecode = BuildDecodeTable();
    return kDecode;
}
}  // namespace
std::string SerialBase32Encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve((data.size() * 8 + 4) / 5);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const std::uint8_t byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            out.push_back(kAlphabet[(buffer >> (bits - 5)) & 0x1f]);
            bits -= 5;
        }
    }
    if (bits > 0) out.push_back(kAlphabet[(buffer << (5 - bits)) & 0x1f]);
    return out;
}
std::optional<std::vector<std::uint8_t>> SerialBase32Decode(const std::string& text) {
    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char ch : text) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc == '-' || uc == '_' || uc == ' ') continue;
        signed char value = uc < 128 ? DecodeTable()[uc] : kInvalid;
        if (value == kInvalid) return std::nullopt;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>((buffer >> (bits - 8)) & 0xff));
            bits -= 8;
        }
    }
    if (bits >= 5) return std::nullopt;  // 尾部不足一字节的碎片
    return out;
}
}  // namespace voicestick
