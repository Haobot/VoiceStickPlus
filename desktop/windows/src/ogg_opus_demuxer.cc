// Copyright (c) 2026 Voice Stick contributors. All rights reserved.

#include "ogg_opus_demuxer.h"

#include <cstring>

namespace voicestick {

namespace {

// 解析 OpusHead packet，填充 sample_rate/channels/preskip/gain。失败返回 false。
// OpusHead 布局：[0..7]="OpusHead" [8]=version [9]=channels [10..11]=preskip(LE16)
// [12..15]=input_sample_rate(LE32) [16..17]=gain(LE16) [18]=channel_mapping_family。
bool ParseOpusHead(const ByteVector& p, OggOpusStream& out) {
    if (p.size() < 19) return false;
    if (std::memcmp(p.data(), "OpusHead", 8) != 0) return false;
    out.channels = p[9];
    out.preskip = ReadLe16(std::span<const std::uint8_t>(p.data() + 10, 2));
    out.sample_rate = ReadLe32(std::span<const std::uint8_t>(p.data() + 12, 4));
    out.gain = ReadLe16(std::span<const std::uint8_t>(p.data() + 16, 2));
    return true;
}

bool IsOpusTags(const ByteVector& p) {
    return p.size() >= 8 && std::memcmp(p.data(), "OpusTags", 8) == 0;
}

}  // namespace

bool ParseOggOpus(std::span<const std::uint8_t> data, OggOpusStream& out) {
    out = OggOpusStream{};
    const std::size_t size = data.size();
    std::size_t offset = 0;
    bool header_parsed = false;
    bool error = false;
    ByteVector pending;  // 跨页累积的进行中 packet（lacing 255 续段）。

    while (offset < size) {
        // Ogg 页头固定 27 字节：magic(4)+version(1)+header_type(1)+granule(8)+
        // serial(4)+sequence(4)+crc(4)+page_segments(1)。
        if (offset + 27 > size) {
            error = true;
            break;
        }
        if (data[offset] != 'O' || data[offset + 1] != 'g' ||
            data[offset + 2] != 'g' || data[offset + 3] != 'S') {
            error = true;
            break;
        }
        const std::size_t seg_table_off = offset + 27;
        const std::uint8_t page_segments = data[offset + 26];
        if (seg_table_off + page_segments > size) {
            error = true;
            break;
        }

        // 数据区紧接 segment_table 之后；按 lacing 规则切分 packet：
        // 累加 segment 长度，遇 <255 结束一个 packet，==255 表示续段（跨段/跨页）。
        std::size_t data_off = seg_table_off + page_segments;
        for (std::size_t seg_i = 0; seg_i < page_segments; ++seg_i) {
            const std::uint8_t seg = data[seg_table_off + seg_i];
            if (data_off + seg > size) {
                error = true;
                break;
            }
            pending.insert(pending.end(), data.begin() + data_off,
                           data.begin() + data_off + seg);
            data_off += seg;
            if (seg < 255) {
                if (!header_parsed) {
                    // 第一个完成的 packet 必须是 OpusHead。
                    if (!ParseOpusHead(pending, out)) {
                        error = true;
                        break;
                    }
                    header_parsed = true;
                } else if (!IsOpusTags(pending)) {
                    out.packets.push_back(pending);
                }
                pending.clear();
            }
        }
        if (error) break;
        offset = data_off;
    }

    return header_parsed && !error;
}

}  // namespace voicestick
