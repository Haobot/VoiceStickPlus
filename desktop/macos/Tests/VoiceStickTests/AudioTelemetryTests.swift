import Foundation
@testable import VoiceStickCore

/// 音频会话遥测（音频质量实验台 Phase 1）单测：
/// 1. 来源解析纯函数（VS-/VG-/RC- 三态）；
/// 2. seq 缺口实测丢帧；
/// 3. 侧车 JSON 读写/finalText 补写。
func runAudioTelemetryTests() {
    testAudioSessionSourceResolve()
    testAudioSeqGapTracker()
    testDebugAudioSidecarRoundtrip()
}

// MARK: - 来源解析

private func testAudioSessionSourceResolve() {
    // Stick 设备 + 物理键（无 source）→ VS-。
    checkEqual(
        AudioSessionSource.resolve(triggerSource: nil, deviceClass: .stickS3),
        .stickMic, "AudioTel.resolve.stickPhysical")
    // Stick 设备 + source=="xiaomi"（网关模式遥控器语音键）→ VG-。
    checkEqual(
        AudioSessionSource.resolve(triggerSource: "xiaomi", deviceClass: .stickS3),
        .gatewayRelay, "AudioTel.resolve.gatewayRelay")
    // 编码器键 / 其他 source 值：仍 Stick 麦。
    checkEqual(
        AudioSessionSource.resolve(triggerSource: "encoder", deviceClass: .stickS3),
        .stickMic, "AudioTel.resolve.encoderIsStickMic")
    // 遥控器直连（设备类优先，即使未来带上 source）→ RC-。
    checkEqual(
        AudioSessionSource.resolve(triggerSource: nil, deviceClass: .xiaomiRemote2Pro),
        .remoteDirect, "AudioTel.resolve.remoteDirect")
    checkEqual(
        AudioSessionSource.resolve(triggerSource: "xiaomi", deviceClass: .xiaomiRemote2Pro),
        .remoteDirect, "AudioTel.resolve.remoteDirectWins")
    // 设备类未知（防御）→ 按触发源判。
    checkEqual(
        AudioSessionSource.resolve(triggerSource: nil, deviceClass: nil),
        .stickMic, "AudioTel.resolve.unknownDeviceDefaults")

    checkEqual(AudioSessionSource.stickMic.filenamePrefix, "VS-", "AudioTel.prefix.vs")
    checkEqual(AudioSessionSource.gatewayRelay.filenamePrefix, "VG-", "AudioTel.prefix.vg")
    checkEqual(AudioSessionSource.remoteDirect.filenamePrefix, "RC-", "AudioTel.prefix.rc")

    checkEqual(AudioSessionSource.stickMic.deviceClassTag, "stick_s3", "AudioTel.tag.stick")
    checkEqual(AudioSessionSource.gatewayRelay.deviceClassTag, "stick_s3", "AudioTel.tag.gateway")
    checkEqual(AudioSessionSource.remoteDirect.deviceClassTag,
               "xiaomi_remote_2pro", "AudioTel.tag.remote")
}

// MARK: - seq 缺口

private func testAudioSeqGapTracker() {
    var tracker = AudioSeqGapTracker()
    checkEqual(tracker.droppedFrames, 0, "AudioTel.seq.initial")

    // 连续：无缺口。
    tracker.feed(5)
    tracker.feed(6)
    tracker.feed(7)
    checkEqual(tracker.droppedFrames, 0, "AudioTel.seq.contiguous")

    // 跳 3 帧（7→11，缺 8/9/10）：+3。
    tracker.feed(11)
    checkEqual(tracker.droppedFrames, 3, "AudioTel.seq.gap3")

    // 重复帧与乱序回退帧：不计，且不回拨 last（否则会把旧区间重复计缺口）。
    tracker.feed(11)
    tracker.feed(9)
    checkEqual(tracker.droppedFrames, 3, "AudioTel.seq.dupIgnored")

    // 再跳 1（11→13，缺 12）：累计 4。
    tracker.feed(13)
    checkEqual(tracker.droppedFrames, 4, "AudioTel.seq.accumulate")

    // 复位清零。
    tracker.reset()
    checkEqual(tracker.droppedFrames, 0, "AudioTel.seq.reset")
    tracker.feed(100)
    checkEqual(tracker.droppedFrames, 0, "AudioTel.seq.afterReset")
}

// MARK: - 侧车读写

private func testDebugAudioSidecarRoundtrip() {
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("sidecar-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: dir) }

    let sidecar = DebugAudioSidecar(
        source: AudioSessionSource.gatewayRelay.rawValue,
        deviceClass: "stick_s3",
        sessionID: 42,
        frames: 120,
        streamSpanSeconds: 4.83,
        recordingSeconds: 5.02,
        droppedFrames: 1,
        noiseSuppression: true)
    let url = dir.appendingPathComponent("20261031-120000-VG-XXXX-session-42.json")
    check(sidecar.write(to: url), "AudioTel.sidecar.write")

    let loaded = DebugAudioSidecar.read(from: url)
    checkNotNil(loaded, "AudioTel.sidecar.read")
    checkEqual(loaded?.source, "gatewayRelay", "AudioTel.sidecar.source")
    checkEqual(loaded?.frames, 120, "AudioTel.sidecar.frames")
    checkEqual(loaded?.droppedFrames, 1, "AudioTel.sidecar.dropped")
    checkEqual(loaded?.noiseSuppression, true, "AudioTel.sidecar.ns")
    checkNil(loaded?.finalText, "AudioTel.sidecar.noFinalYet")
    checkEqual(loaded?.schema, DebugAudioSidecar.schemaVersion, "AudioTel.sidecar.schema")

    // finalText 补写（读-改-写）。
    check(DebugAudioSidecar.updateFinalText("帮我打开浏览器", at: url),
          "AudioTel.sidecar.updateFinal")
    checkEqual(DebugAudioSidecar.read(from: url)?.finalText,
               "帮我打开浏览器", "AudioTel.sidecar.finalPersisted")

    // 缺文件补写：失败不抛。
    check(!DebugAudioSidecar.updateFinalText("x", at: dir.appendingPathComponent("absent.json")),
          "AudioTel.sidecar.updateMissing")

    // 坏 JSON：read 返回 nil。
    let bad = dir.appendingPathComponent("bad.json")
    try? Data("{not-json".utf8).write(to: bad)
    checkNil(DebugAudioSidecar.read(from: bad), "AudioTel.sidecar.badJson")
}
