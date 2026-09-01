#include "xiaomi_atvv_protocol.h"

namespace voicestick {

ByteVector XiaomiAtvvProtocol::GetCapsCommand() {
    return ByteVector{0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
}

ByteVector XiaomiAtvvProtocol::MicOpenAckCommand(bool legacy_layout) {
    ByteVector data{0x0C, 0x00};
    if (legacy_layout) data.push_back(codec_mask_16khz);  // 旧版补所选 codec（只接受 16kHz）
    return data;
}

ByteVector XiaomiAtvvProtocol::MicCloseCommand(bool legacy_layout, std::uint8_t session_id) {
    ByteVector data{0x0D};
    if (!legacy_layout) data.push_back(session_id);
    return data;
}

std::optional<XiaomiAtvvProtocol::CapsInfo> XiaomiAtvvProtocol::ParseCaps(
    std::span<const std::uint8_t> data) {
    if (data.size() < 3 || data[0] != control_caps) return std::nullopt;

    CapsInfo caps;
    caps.version = static_cast<std::uint16_t>((data[1] << 8) | data[2]);
    if (caps.IsV1OrLater()) {
        // v1.0 布局：[3]=codec 掩码、[4]=interaction、[5:7]=帧长 BE16（可选）。
        if (data.size() < 5) return std::nullopt;
        caps.codecs = data[3];
        caps.interaction = data[4];
        // 兼容分支：报 v1 但 codecs==0，按旧版双字节 codec 布局重读（[4] 实为 codec）。
        // 旧版布局未定义帧长字段，[5:7] 是垃圾值，不读、保持默认 120
        //（与 scripts/e2e_test/atvv_capture.py 的解析对齐）。
        bool legacy_layout = false;
        if (caps.codecs == 0 && data.size() >= 9 && (data[4] & 0x03) != 0) {
            caps.codecs = data[4];
            caps.interaction = 0x03;
            legacy_layout = true;
        }
        if (!legacy_layout && data.size() >= 7) {
            const std::size_t frame_bytes =
                (static_cast<std::size_t>(data[5]) << 8) | data[6];
            if (frame_bytes > 0) caps.frame_bytes = frame_bytes;
        }
    } else {
        // 旧版布局：需 len≥9，codecs=[4]、interaction=0，帧长取默认值。
        if (data.size() < 9) return std::nullopt;
        caps.codecs = data[4];
        caps.interaction = 0;
    }
    return caps;
}

} // namespace voicestick
