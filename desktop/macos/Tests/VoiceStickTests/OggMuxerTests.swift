import Foundation
import VoiceStickCore

// N1 第二刀：Ogg/Opus 复用器下沉 Core 的覆盖——页结构（OggS/版本/头型/granule/
// serial/段表）、CRC 不变量（存储值==就地重算）、48kHz granule 口径（40ms@16k
// → 1920，历史 960 口径曾致时长虚高 50%）、eos 空页、reset 后头页重发。
func runOggMuxerTests() {
    // 逐页切分（v1 单 lacing 段假设下的通用段表求长）
    func oggPages(_ data: Data) -> [Data] {
        var pages: [Data] = []
        var pos = 0
        while pos + 27 <= data.count,
              Data(data[pos..<pos + 4]) == Data("OggS".utf8) {
            let segCount = Int(data[pos + 26])
            var payloadLen = 0
            for s in 0..<segCount where pos + 27 + s < data.count {
                payloadLen += Int(data[pos + 27 + s])
            }
            let total = 27 + segCount + payloadLen
            guard pos + total <= data.count else { break }
            // Data 切片保留父索引：必须 Data(...) 重包把索引基复位到 0
            //（否则 pages[i][5] 按父索引 5 取 → 越界 SIGTRAP，61 行曾中招）。
            pages.append(Data(data[pos..<pos + total]))
            pos += total
        }
        return pages
    }
    func u64(_ page: Data, _ at: Int) -> UInt64 {
        (0..<8).reduce(UInt64(0)) { acc, i in
            acc | (UInt64(page[at + i]) << (8 * UInt64(i)))
        }
    }
    func storedCRC(_ page: Data) -> UInt32 {
        (0..<4).reduce(UInt32(0)) { acc, i in
            acc | (UInt32(page[22 + i]) << (8 * UInt32(i)))
        }
    }
    func recomputedCRC(_ page: Data) -> UInt32 {
        var zeroed = page
        zeroed.replaceSubrange(22..<26, with: [0, 0, 0, 0])
        return OggCRC.checksum(zeroed)
    }

    let mux = OggOpusMuxer(sampleRate: 16000, channels: 1)
    let payload = Data([0xDE, 0xAD, 0xBE, 0xEF])
    let first = mux.append(opusPayload: payload, isLast: false)
    let pages = oggPages(first)
    check(pages.count == 3, "N1ogg: 首帧输出 = 头页+标签页+音频页 共 3 页")

    let head = pages[0]
    check(Data(head.prefix(4)) == Data("OggS".utf8), "N1ogg: 捕获符 OggS")
    check(head[4] == 0, "N1ogg: Ogg 流版本 0")
    check(head[5] == 0x02, "N1ogg: OpusHead 页 b0s 头型")
    check(Data(head[27 + 1..<27 + 9]) == Data("OpusHead".utf8),
          "N1ogg: 负载魔数 OpusHead")
    // OpusHead 负载自页载荷（28）起：魔数 8B + version 1B + channels 1B + preSkip 2B
    // + inputSampleRate 4B → channels@37、sampleRate@40。
    check(head[37] == 1, "N1ogg: OpusHead 声道数 1")
    let inputRate = head[40..<44].withUnsafeBytes { $0.load(as: UInt32.self) }
    check(inputRate == 16000, "N1ogg: OpusHead 输入采样率 16000（LE）")

    let tags = pages[1]
    check(tags[5] == 0x00, "N1ogg: OpusTags 页头型 0")
    check(Data(tags[27 + 1..<27 + 9]) == Data("OpusTags".utf8),
          "N1ogg: 标签页魔数 OpusTags")
    // OpusTags 负载：魔数 8B@28 + vendorLen 4B@36 + vendor@40。
    let vendorLen = tags[36..<40].withUnsafeBytes { $0.load(as: UInt32.self) }
    check(vendorLen == 10, "N1ogg: vendor 长度 10")
    check(Data(tags[40..<50]) == Data("VoiceStick".utf8),
          "N1ogg: vendor = VoiceStick")

    let audio = pages[2]
    check(audio[5] == 0x00, "N1ogg: 非末帧音频页头型 0")
    check(u64(audio, 6) == 1920, "N1ogg: 40ms@16k granule 步进 1920（48kHz 口径）")
    check(Data(audio[28..<28 + payload.count]) == payload, "N1ogg: 音频负载原样")

    for (i, page) in pages.enumerated() {
        check(storedCRC(page) == recomputedCRC(page),
              "N1ogg: 第 \(i + 1) 页 CRC 存储值==就地重算")
    }

    let last = mux.append(opusPayload: payload, isLast: true)
    let lastPages = oggPages(last)
    check(lastPages.count == 1 && lastPages[0][5] == 0x04,
          "N1ogg: isLast 音频页 b0s（e-o-s）")
    check(u64(lastPages[0], 6) == 3840, "N1ogg: 第二帧 granule 累计 3840")

    let tail = mux.finish()
    let eosPages = oggPages(tail)
    check(eosPages.count == 1, "N1ogg: finish 输出单个 eos 空页")
    check(eosPages[0][5] == 0x04, "N1ogg: eos 空页头型 0x04")
    check(eosPages[0][26] == 0, "N1ogg: eos 空页段表计数 0")
    check(u64(eosPages[0], 6) == 3840, "N1ogg: eos 空页 granule 终值 3840")
    check(storedCRC(eosPages[0]) == recomputedCRC(eosPages[0]),
          "N1ogg: eos 空页 CRC 不变量")

    mux.reset()
    let again = mux.append(opusPayload: payload, isLast: false)
    let againPages = oggPages(again)
    check(againPages.count == 3 && againPages[0][5] == 0x02 && againPages[1][5] == 0x00,
          "N1ogg: reset 后头页/标签页重发且序列复位")
    check(u64(againPages[2], 6) == 1920, "N1ogg: reset 后 granule 从 1920 重新累计")
}
