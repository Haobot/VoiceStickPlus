import Foundation

/// 测试共享工具：repo root 定位、ATVV golden fixtures 读取（WAV/sidecar）。
/// 对齐 Windows tests/core_tests.cc 的 ResolveAtvvFixturesRoot / ReadWavPcm16ForTest /
/// ParseAtvvSidecarForTest。
enum TestSupport {
    /// 本文件位于 desktop/macos/Tests/VoiceStickTests/，目录向上 4 级即 repo root。
    static var repoRoot: URL {
        URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent() // Tests/VoiceStickTests
            .deletingLastPathComponent() // Tests
            .deletingLastPathComponent() // macos
            .deletingLastPathComponent() // desktop
            .deletingLastPathComponent() // repo root
    }

    /// fixtures 根目录：默认 scripts/e2e_test/fixtures/xiaomi，
    /// 可用 VOICESTICK_ATVV_FIXTURES_DIR 环境变量覆盖（对齐 Windows）。
    static var atvvFixturesRoot: URL {
        if let env = ProcessInfo.processInfo.environment["VOICESTICK_ATVV_FIXTURES_DIR"],
           !env.isEmpty {
            return URL(fileURLWithPath: env, isDirectory: true)
        }
        return repoRoot.appendingPathComponent("scripts/e2e_test/fixtures/xiaomi", isDirectory: true)
    }

    /// 读 PCM16 mono WAV（Python wave 模块产物）：walk RIFF chunk 取 fmt/data。
    /// 非 RIFF/WAVE、非 PCM/mono/16bit、截断均返回 nil。
    static func readWavPcm16(_ url: URL) -> [Int16]? {
        guard let bytes = try? Data(contentsOf: url), bytes.count >= 12 else { return nil }
        func u16(_ off: Int) -> UInt16 {
            UInt16(bytes[off]) | UInt16(bytes[off + 1]) << 8
        }
        func u32(_ off: Int) -> UInt32 {
            UInt32(bytes[off]) | UInt32(bytes[off + 1]) << 8 |
                UInt32(bytes[off + 2]) << 16 | UInt32(bytes[off + 3]) << 24
        }
        guard bytes[0..<4].elementsEqual([0x52, 0x49, 0x46, 0x46]), // "RIFF"
              bytes[8..<12].elementsEqual([0x57, 0x41, 0x56, 0x45]) // "WAVE"
        else { return nil }
        var pos = 12
        var fmtOk = false
        while pos + 8 <= bytes.count {
            let id = bytes[pos..<(pos + 4)]
            let size = Int(u32(pos + 4))
            let payload = pos + 8
            if payload + size > bytes.count { break }
            if id.elementsEqual([0x66, 0x6D, 0x74, 0x20]) { // "fmt "
                fmtOk = size >= 16 && u16(payload) == 1 && // PCM
                    u16(payload + 2) == 1 && // mono
                    u16(payload + 14) == 16 // 16bit
            } else if id.elementsEqual([0x64, 0x61, 0x74, 0x61]), fmtOk { // "data"
                var out: [Int16] = []
                out.reserveCapacity(size / 2)
                for i in stride(from: 0, to: size - size % 2, by: 2) {
                    out.append(Int16(bitPattern: u16(payload + i)))
                }
                return out
            }
            pos = payload + size + (size & 1) // chunk 按偶数字节对齐
        }
        return nil
    }

    /// ATVV sidecar 段落：ADPCM 流内 [offset, offset+bytes) 区间按 predictor/step_index reset 解码。
    struct GoldenSegment {
        let offset: Int
        let bytes: Int
        let predictor: Int
        let stepIndex: Int
    }

    /// 解析 session_N.json sidecar：gain_db + segments（非空数组，字段全为数值）。
    /// 畸形 JSON 返回 nil（对齐 Windows ParseAtvvSidecarForTest 的 false）。
    static func parseAtvvSidecar(_ data: Data) -> (gainDb: Double, segments: [GoldenSegment])? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let gainDb = (root["gain_db"] as? NSNumber)?.doubleValue,
              let segs = root["segments"] as? [[String: Any]], !segs.isEmpty
        else { return nil }
        var segments: [GoldenSegment] = []
        for item in segs {
            guard let offset = (item["offset"] as? NSNumber)?.intValue,
                  let bytes = (item["bytes"] as? NSNumber)?.intValue,
                  let predictor = (item["predictor"] as? NSNumber)?.intValue,
                  let stepIndex = (item["step_index"] as? NSNumber)?.intValue
            else { return nil }
            segments.append(GoldenSegment(offset: offset, bytes: bytes,
                                          predictor: predictor, stepIndex: stepIndex))
        }
        return (gainDb, segments)
    }
}
