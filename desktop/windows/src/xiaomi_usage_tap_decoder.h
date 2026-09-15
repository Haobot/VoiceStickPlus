#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace voicestick {

// tap 管道帧（hid_tap_dll.cc 侧变长协议）：
//   0x01 ……………………………… 心跳（1 字节，5s 周期）
//   0x02 + 9 字节原始报文 …… 按键数据（10 字节）
struct XiaomiTapFrame {
    bool is_data = false;  // false = 心跳
    uint8_t report[9] = {};  // is_data 时的 HID 报文（01 00 00 + 3×LE16 usage）
};

// 字节流 → 完整帧解码器（管道读循环用，纯逻辑可单测）。
//
// 语义：任意分片喂入，凑齐即产出；粘包按序解出多帧；非法前导字节逐字节
// 丢弃重同步（前导字节自带帧型，比定长协议更抗错位）。半帧驻留缓冲等待
// 后续字节；Reset 清残留（连接重建立场用）。
class XiaomiTapFrameDecoder {
public:
    std::vector<XiaomiTapFrame> OnBytes(const uint8_t* data, size_t size);
    void Reset() { buffer_.clear(); }

private:
    std::vector<uint8_t> buffer_;
};

} // namespace voicestick
