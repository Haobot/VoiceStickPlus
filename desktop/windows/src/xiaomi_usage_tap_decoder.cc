#include "xiaomi_usage_tap_decoder.h"

namespace voicestick {

namespace {

constexpr uint8_t kHeartbeatType = 0x01;
constexpr uint8_t kDataType = 0x02;
constexpr size_t kDataFrameSize = 10;  // type + 9 字节报文

} // namespace

std::vector<XiaomiTapFrame> XiaomiTapFrameDecoder::OnBytes(const uint8_t* data,
                                                           size_t size) {
    buffer_.insert(buffer_.end(), data, data + size);
    std::vector<XiaomiTapFrame> frames;
    size_t offset = 0;
    while (offset < buffer_.size()) {
        const uint8_t lead = buffer_[offset];
        if (lead == kHeartbeatType) {
            XiaomiTapFrame frame;
            frame.is_data = false;
            frames.push_back(frame);
            offset += 1;
        } else if (lead == kDataType) {
            if (buffer_.size() - offset < kDataFrameSize) break;  // 半帧等待
            XiaomiTapFrame frame;
            frame.is_data = true;
            for (size_t i = 0; i < 9; ++i) {
                frame.report[i] = buffer_[offset + 1 + i];
            }
            frames.push_back(frame);
            offset += kDataFrameSize;
        } else {
            // 非法前导字节：流错位/协议漂移，丢弃一字节重同步。
            offset += 1;
        }
    }
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    return frames;
}

} // namespace voicestick
