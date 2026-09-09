#pragma once

#include <cstddef>
#include <cstdint>

namespace voicestick {
namespace pinyin_data {

// 单字拼音条目：声母/韵母为逗号分隔的多读音并集（零声母为空串）。
// 由 scripts/gen_pinyin_table.py 生成，按码点升序，可二分查找。
struct Entry {
    std::uint32_t cp;
    const char* initials;
    const char* finals;
};

extern const Entry kTable[];
extern const std::size_t kTableSize;

} // namespace pinyin_data
} // namespace voicestick
