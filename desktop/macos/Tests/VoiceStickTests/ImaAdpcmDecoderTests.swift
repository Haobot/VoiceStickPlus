import Foundation
import VoiceStickCore

/// 移植 Windows TestImaAdpcmDecoderGolden + TestImaAdpcmDecoderGoldenFixtures。
/// （旧 XCTest ImaAdpcmDecoderTests 逐条转换。）

// 测试本地 IMA 编码器（公开标准算法的独立实现）：输出编码字节与编码器内部
// predictor 轨迹（即标准解码的期望输出），用于与解码器逐样本对拍。
private struct GoldenVector {
    let encoded: Data
    let expectedDecoded: [Int16]
}

private func imaEncodeForTest(_ pcm: [Int16]) -> GoldenVector {
    let stepTable: [Int] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767,
    ]
    let indexTable = [-1, -1, -1, -1, 2, 4, 6, 8]
    var encoded = Data()
    var expected: [Int16] = []
    var predictor = 0
    var index = 0
    var highNibble: Int? // 高半字节优先
    for sample in pcm {
        let step = stepTable[index]
        var nibble = 0
        var diff = step >> 3
        var delta = Int(sample) - predictor
        if delta < 0 {
            nibble = 8
            delta = -delta
        }
        if delta >= step {
            nibble |= 4
            delta -= step
            diff += step
        }
        if delta >= (step >> 1) {
            nibble |= 2
            delta -= step >> 1
            diff += step >> 1
        }
        if delta >= (step >> 2) {
            nibble |= 1
            diff += step >> 2
        }
        predictor = nibble & 8 != 0 ? predictor - diff : predictor + diff
        predictor = min(max(predictor, -32768), 32767)
        index = min(max(index + indexTable[nibble & 7], 0), 88)
        expected.append(Int16(predictor))
        if let high = highNibble {
            encoded.append(UInt8((high << 4) | nibble))
            highNibble = nil
        } else {
            highNibble = nibble
        }
    }
    // 奇数样本需补低半字节，会多解一个样本；测试只用偶数样本输入。
    precondition(highNibble == nil)
    return GoldenVector(encoded: encoded, expectedDecoded: expected)
}

func runImaAdpcmDecoderTests() {
    // ---- testGoldenVectors ----
    let decoder = ImaAdpcmDecoder()
    // 标准向量：reset(0,0) 后 0x11 → [1,2]。
    decoder.reset(predictor: 0, stepIndex: 0)
    checkEqual(decoder.decode(Data([0x11])), [1, 2], "GoldenVectors.singleByte")

    // 预计算 golden（Windows 侧独立 Python 实现对拍，高半字节优先）。
    decoder.reset(predictor: 0, stepIndex: 0)
    let out = decoder.decode(Data([0x11, 0x22, 0x77, 0x88, 0xF0, 0x0F, 0x45, 0x54]))
    checkEqual(out, [1, 2, 5, 8, 19, 49, 45, 42,
                     -10, -3, 3, -90, 30, 208, 468, 781],
               "GoldenVectors.precomputed")

    // 状态连续推进：分段解码与一次性解码逐样本相等。
    decoder.reset(predictor: 0, stepIndex: 0)
    var joined = decoder.decode(Data([0x11, 0x22, 0x77]))
    joined.append(contentsOf: decoder.decode(Data([0x88, 0xF0, 0x0F, 0x45, 0x54])))
    checkEqual(joined, out, "GoldenVectors.splitEqualsJoined")

    // ---- testEncodeRoundTrip ----
    // 编码往返：正弦+斜波（480 样本，偶数）经测试编码器后与解码器逐样本对拍。
    var pcm: [Int16] = []
    for i in 0..<480 {
        pcm.append(Int16(sin(Double(i) * 0.05) * 12000.0 + Double(i) * 10))
    }
    let golden = imaEncodeForTest(pcm)
    decoder.reset(predictor: 0, stepIndex: 0)
    checkEqual(decoder.decode(golden.encoded), golden.expectedDecoded, "EncodeRoundTrip")

    // ---- testResetClamping ----
    decoder.reset(predictor: 0, stepIndex: 200)
    checkEqual(decoder.stepIndex, 88, "ResetClamping.highClampsTo88")
    decoder.reset(predictor: 0, stepIndex: -5)
    checkEqual(decoder.stepIndex, 0, "ResetClamping.negativeClampsTo0")
    decoder.reset(predictor: -100, stepIndex: 40)
    checkEqual(decoder.predictor, -100, "ResetClamping.predictor")
    checkEqual(decoder.stepIndex, 40, "ResetClamping.stepIndex")
    // 高 step 起步解码正常推进。
    checkEqual(decoder.decode(Data([0x11, 0x11])).count, 4, "ResetClamping.decodeAdvances")
}

// ===== ATVV golden fixtures 对拍 =====
// 数据源：atvv_capture.py 真机采集（或 atvv_bench.py --emit-demo-fixture 合成），
// 默认扫描 scripts/e2e_test/fixtures/xiaomi/**（可用 VOICESTICK_ATVV_FIXTURES_DIR
// 环境变量覆盖）。每会话四件套：session_N.adpcm（原始流）、session_N.json
//（sidecar：增益/逐段 reset 区间）、session_N.raw.wav（纯解码）、
// session_N.wav（解码+三点平滑+增益）。按 sidecar 段落复现解码路径，逐样本对拍。
// fixtures 目录缺失时打印 SKIP 不算失败（不伪造结果；本机存在时必须真实跑通）。
// 返回实际对拍的会话数（供 runner 汇总打印）。
func runImaAdpcmGoldenFixtures() -> Int {
    let root = TestSupport.atvvFixturesRoot
    guard FileManager.default.fileExists(atPath: root.path) else {
        print("SKIP ImaAdpcmGoldenFixtures: ATVV golden fixtures 目录不存在: \(root.path)")
        return 0
    }
    var adpcmFiles: [URL] = []
    if let enumerator = FileManager.default.enumerator(
        at: root, includingPropertiesForKeys: [.isRegularFileKey]) {
        for case let url as URL in enumerator {
            let isRegular = (try? url.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile ?? false
            if isRegular, url.pathExtension == "adpcm", url.lastPathComponent.hasPrefix("session_") {
                adpcmFiles.append(url)
            }
        }
    }
    adpcmFiles.sort { $0.path < $1.path }
    guard !adpcmFiles.isEmpty else {
        print("SKIP ImaAdpcmGoldenFixtures: \(root.path) 下无 session_*.adpcm")
        return 0
    }

    var checked = 0
    var checkedDirs = Set<String>()
    for adpcmURL in adpcmFiles {
        let dir = adpcmURL.deletingLastPathComponent()
        let stem = adpcmURL.deletingPathExtension().lastPathComponent // session_N
        let sidecarURL = dir.appendingPathComponent("\(stem).json")
        let rawWavURL = dir.appendingPathComponent("\(stem).raw.wav")
        let wavURL = dir.appendingPathComponent("\(stem).wav")
        // 有 .adpcm 但缺 sidecar/WAV 属于残缺 fixtures，直接失败暴露问题。
        check(FileManager.default.fileExists(atPath: sidecarURL.path),
              "GoldenFixtures.\(stem).sidecarExists")
        check(FileManager.default.fileExists(atPath: rawWavURL.path),
              "GoldenFixtures.\(stem).rawWavExists")
        check(FileManager.default.fileExists(atPath: wavURL.path),
              "GoldenFixtures.\(stem).wavExists")

        guard let sidecarData = try? Data(contentsOf: sidecarURL),
              let (gainDb, segments) = unwrap(TestSupport.parseAtvvSidecar(sidecarData),
                                              "GoldenFixtures.\(stem).sidecarParsed"),
              let adpcm = try? Data(contentsOf: adpcmURL)
        else { continue }

        let segmentDecoder = ImaAdpcmDecoder()
        var pcm: [Int16] = []
        var segmentsInBounds = true
        for segment in segments {
            if segment.offset + segment.bytes > adpcm.count { segmentsInBounds = false }
            segmentDecoder.reset(predictor: Int16(truncatingIfNeeded: segment.predictor),
                                 stepIndex: segment.stepIndex)
            pcm.append(contentsOf: segmentDecoder.decode(
                adpcm.subdata(in: segment.offset..<(segment.offset + segment.bytes))))
        }
        check(segmentsInBounds, "GoldenFixtures.\(stem).segmentsInBounds")

        // 对拍 1：纯解码 == session_N.raw.wav（逐样本相等）。
        guard let expectedRaw = unwrap(TestSupport.readWavPcm16(rawWavURL),
                                       "GoldenFixtures.\(stem).rawWavRead")
        else { continue }
        if pcm != expectedRaw {
            var diff = 0
            while diff < min(pcm.count, expectedRaw.count), pcm[diff] == expectedRaw[diff] {
                diff += 1
            }
            check(false, "GoldenFixtures.\(stem).rawMatch sizes \(pcm.count) vs "
                  + "\(expectedRaw.count), 首个差异样本 #\(diff)")
        } else {
            check(true, "GoldenFixtures.\(stem).rawMatch")
        }

        // 对拍 2：解码 + PcmPostprocessor(sidecar 增益) == session_N.wav。
        // Python 侧 smooth3+apply_gain 的舍入已对齐 lround（.toNearestOrAwayFromZero）。
        let processed = PcmPostprocessor(gainDb: gainDb).process(pcm)
        guard let expectedWav = unwrap(TestSupport.readWavPcm16(wavURL),
                                       "GoldenFixtures.\(stem).wavRead")
        else { continue }
        checkEqual(processed, expectedWav, "GoldenFixtures.\(stem).wavMatch gain_db=\(gainDb)")

        checked += 1
        checkedDirs.insert(dir.lastPathComponent)
        print("  golden \(dir.lastPathComponent)/\(stem): \(pcm.count) samples, "
            + "\(segments.count) segment(s) OK")
    }
    print("ATVV golden fixtures: \(checked) session(s) checked "
        + "across \(checkedDirs.count) capture dir(s)")
    return checked
}
