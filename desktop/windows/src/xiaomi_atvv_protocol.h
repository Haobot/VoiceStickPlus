#pragma once

#include "byte_utils.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace voicestick {

// 小米蓝牙遥控器 2 Pro 的 Google ATVV 协议常量与编解码（纯逻辑，不碰 WinRT）。
// 协议事实见 Doc/Plan/xiaomi-remote-2-pro-support.md §3：遥控器主导会话，
// 主机应答；只接受 16 kHz 档。
class XiaomiAtvvProtocol {
public:
    static constexpr const wchar_t* service_uuid = L"ab5e0001-5a21-4f05-bc7d-af01f617b664";
    static constexpr const wchar_t* tx_uuid = L"ab5e0002-5a21-4f05-bc7d-af01f617b664";
    static constexpr const wchar_t* audio_uuid = L"ab5e0003-5a21-4f05-bc7d-af01f617b664";
    static constexpr const wchar_t* control_uuid = L"ab5e0004-5a21-4f05-bc7d-af01f617b664";

    // Control 特征 opcode（遥控器 → 主机 notify 的首字节）。
    static constexpr std::uint8_t control_stop = 0x00;         // 语音键松开
    static constexpr std::uint8_t control_stream_start = 0x04; // 音频流开始
    static constexpr std::uint8_t control_mic_open = 0x08;     // 语音键按下（请求开麦）
    static constexpr std::uint8_t control_audio_sync = 0x0a;   // ADPCM 解码器同步
    static constexpr std::uint8_t control_caps = 0x0b;         // GET_CAPS 应答

    // codec 位掩码（CAPS 应答 bytes[3]，或旧版布局 bytes[4]）。
    static constexpr std::uint8_t codec_mask_8khz = 0x01;
    static constexpr std::uint8_t codec_mask_16khz = 0x02;

    // CAPS 未协商帧长（或协商值 0）时的缺省 ADPCM 帧长：120 字节 = 240 采样。
    static constexpr std::size_t default_frame_bytes = 120;

    struct CapsInfo {
        std::uint16_t version = 0;  // BE16，0x0100 即 v1.0
        std::uint8_t codecs = 0;    // codec 位掩码
        std::uint8_t interaction = 0;
        std::size_t frame_bytes = default_frame_bytes;

        bool IsV1OrLater() const { return version >= 0x0100; }
        bool Supports16kHz() const { return (codecs & codec_mask_16khz) != 0; }
    };

    // 连接后主机写 TX 的能力查询：0A 01 00 00 03 03（GET_CAPS v1.0）。
    static ByteVector GetCapsCommand();
    // MIC_OPEN 应答：v≥1.0 写 0C 00；旧版补一字节所选 codec（0x02=16kHz）。
    static ByteVector MicOpenAckCommand(bool legacy_layout);
    // 退出/断开时写 TX：v≥1.0 为 0D <sessionID>，旧版仅 0D。
    static ByteVector MicCloseCommand(bool legacy_layout, std::uint8_t session_id);
    // 解析 CAPS 应答（首字节须为 control_caps）。布局分支：
    // - v≥1.0：[3]=codec 掩码、[4]=interaction、[5:7]=帧长 BE16（0 → 默认 120）；
    //   兼容「报 v1 但用旧版双字节 codec 布局」：codecs==0 且 len≥9 且 [4]&0x03≠0
    //   时取 codecs=[4]、interaction=0x03；该分支旧版布局未定义帧长字段，
    //   [5:7] 不读、frame_bytes 保持默认 120；
    // - v<1.0：需 len≥9，codecs=[4]、interaction=0，帧长取默认值。
    // 短包/缺字段返回 nullopt；是否支持 16kHz 由调用方按 Supports16kHz() 判定。
    static std::optional<CapsInfo> ParseCaps(std::span<const std::uint8_t> data);
};

} // namespace voicestick
