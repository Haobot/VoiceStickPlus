import Foundation
import VoiceStickCore

/// 移植 Windows TestXiaomiAtvvCapsParsing：TX 命令字节流 + CAPS 解析全分支。
/// （旧 XCTest XiaomiAtvvProtocolTests 逐条转换。）
func runAtvvProtocolTests() {
    // ---- testCommandByteStreams ----
    checkEqual(XiaomiAtvvProtocol.getCapsCommand(), Data([0x0A, 0x01, 0x00, 0x00, 0x03, 0x03]),
               "CommandByteStreams.getCaps")
    // MIC_OPEN ACK：v≥1.0 为 0C 00；旧版补所选 codec（0x02=16kHz）。
    checkEqual(XiaomiAtvvProtocol.micOpenAckCommand(legacyLayout: false), Data([0x0C, 0x00]),
               "CommandByteStreams.micOpenAck.v1")
    checkEqual(XiaomiAtvvProtocol.micOpenAckCommand(legacyLayout: true), Data([0x0C, 0x00, 0x02]),
               "CommandByteStreams.micOpenAck.legacy")
    // MIC_CLOSE：v≥1.0 带 sessionID；旧版仅 0D。
    checkEqual(XiaomiAtvvProtocol.micCloseCommand(legacyLayout: false, sessionID: 0x07),
               Data([0x0D, 0x07]), "CommandByteStreams.micClose.v1")
    checkEqual(XiaomiAtvvProtocol.micCloseCommand(legacyLayout: true, sessionID: 0x07),
               Data([0x0D]), "CommandByteStreams.micClose.legacy")

    // ---- testParseCapsV1StandardLayout ----
    // v1.0 标准布局：16kHz、interaction=3、协商帧长 120。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78])),
                         "ParseCapsV1Standard.unwrap") {
        check(caps.isV1OrLater, "ParseCapsV1Standard.isV1OrLater")
        checkEqual(caps.codecs, 0x02, "ParseCapsV1Standard.codecs")
        check(caps.supports16kHz, "ParseCapsV1Standard.supports16kHz")
        checkEqual(caps.interaction, 0x03, "ParseCapsV1Standard.interaction")
        checkEqual(caps.frameBytes, 120, "ParseCapsV1Standard.frameBytes")
    }

    // ---- testParseCapsV1FrameBytesVariants ----
    // 协商帧长 0 → 默认 120。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x00])),
                         "ParseCapsV1FrameVariants.zero.unwrap") {
        checkEqual(caps.frameBytes, XiaomiAtvvProtocol.defaultFrameBytes,
                   "ParseCapsV1FrameVariants.zeroDefaults")
    }
    // 自定义帧长 256。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x01, 0x00])),
                         "ParseCapsV1FrameVariants.custom.unwrap") {
        checkEqual(caps.frameBytes, 256, "ParseCapsV1FrameVariants.custom256")
    }
    // 无帧长字段（len 5）→ 默认 120。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x02, 0x03])),
                         "ParseCapsV1FrameVariants.missing.unwrap") {
        checkEqual(caps.frameBytes, 120, "ParseCapsV1FrameVariants.missingDefaults")
    }

    // ---- testParseCapsV1LegacyCompatLayout ----
    // 兼容分支：报 v1 但 codecs==0，旧版双字节 codec 布局（[4]=0x02 且 len≥9）；
    // 旧版布局未定义帧长字段，[5:7] 的垃圾值（0x0100=256）不得被采用，保持默认 120。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(
        Data([0x0B, 0x01, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00])),
        "ParseCapsV1LegacyCompat.unwrap") {
        checkEqual(caps.codecs, 0x02, "ParseCapsV1LegacyCompat.codecs")
        checkEqual(caps.interaction, 0x03, "ParseCapsV1LegacyCompat.interaction")
        check(caps.supports16kHz, "ParseCapsV1LegacyCompat.supports16kHz")
        checkEqual(caps.frameBytes, XiaomiAtvvProtocol.defaultFrameBytes,
                   "ParseCapsV1LegacyCompat.frameBytesStaysDefault")
    }

    // ---- testParseCapsLegacyV0Layout ----
    // 旧版 v0 布局：len≥9，codecs=[4]，interaction=0，帧长默认。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(
        Data([0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00])),
        "ParseCapsLegacyV0.unwrap") {
        check(!caps.isV1OrLater, "ParseCapsLegacyV0.notV1")
        checkEqual(caps.codecs, 0x02, "ParseCapsLegacyV0.codecs")
        checkEqual(caps.interaction, 0x00, "ParseCapsLegacyV0.interaction")
        checkEqual(caps.frameBytes, XiaomiAtvvProtocol.defaultFrameBytes,
                   "ParseCapsLegacyV0.frameBytes")
    }

    // ---- testParseCapsUnsupportedCodecStillParses ----
    // 8kHz-only：解析成功但不支持 16kHz（由会话层判 error）。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78])),
                         "ParseCapsUnsupported.8khzOnly.unwrap") {
        check(!caps.supports16kHz, "ParseCapsUnsupported.8khzOnly.no16kHz")
    }
    // v1 codecs==0 且不满足兼容分支（[4]&0x03==0）→ 无可用 codec。
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x00, 0x00, 0x00, 0x78])),
                         "ParseCapsUnsupported.zeroCodecs.unwrap") {
        checkEqual(caps.codecs, 0x00, "ParseCapsUnsupported.zeroCodecs.codecs")
        check(!caps.supports16kHz, "ParseCapsUnsupported.zeroCodecs.no16kHz")
    }

    // ---- testParseCapsRejectsShortOrWrongPackets ----
    checkNil(XiaomiAtvvProtocol.parseCaps(Data()), "ParseCapsReject.empty")
    checkNil(XiaomiAtvvProtocol.parseCaps(Data([0x0B])), "ParseCapsReject.len1")
    checkNil(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01])), "ParseCapsReject.len2")
    // 错 opcode。
    checkNil(XiaomiAtvvProtocol.parseCaps(Data([0x0C, 0x01, 0x00, 0x02, 0x03])),
             "ParseCapsReject.wrongOpcode")
    // v1 缺 interaction 字段。
    checkNil(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x01, 0x00, 0x02])),
             "ParseCapsReject.v1MissingInteraction")
    // 旧版短包（len<9）。
    checkNil(XiaomiAtvvProtocol.parseCaps(Data([0x0B, 0x00, 0x01, 0x00, 0x02])),
             "ParseCapsReject.legacyShort")

    // ---- testParseCapsSliceInput ----
    // 切片 Data（startIndex≠0）不得越界崩溃：入口归一化后绝对下标安全，
    // 解析结果与整段输入一致（v1.0、16kHz、interaction=3、帧长 120）。
    let wrapped = Data([0xAA, 0xBB, 0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78, 0xCC])
    let slice: Data = wrapped[2..<9]
    check(slice.startIndex == 2, "ParseCapsSlice.startIndexPreserved") // 前置条件
    if let caps = unwrap(XiaomiAtvvProtocol.parseCaps(slice), "ParseCapsSlice.unwrap") {
        check(caps.isV1OrLater, "ParseCapsSlice.isV1OrLater")
        check(caps.supports16kHz, "ParseCapsSlice.supports16kHz")
        checkEqual(caps.interaction, 0x03, "ParseCapsSlice.interaction")
        checkEqual(caps.frameBytes, 120, "ParseCapsSlice.frameBytes")
    }
}
