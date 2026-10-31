import Foundation
import VoiceStickCore

/// 移植 Windows tests/core_tests.cc 各 TestXiaomiAtvvSession*（规格见
/// Doc/Plan/xiaomi-remote-2-pro-support.md §3；按键语义镜像固件双击设计）。
/// 会话私有成员（decoder/accumulator）经 Mirror 反射只读访问，保持断言与 Windows 对齐。
/// （旧 XCTest XiaomiAtvvSessionTests 19 个用例逐条转换。）

// ---- 会话动作列表查找/收集辅助（对齐 Windows FindAtvv*/CollectAtvv*/HasAtvv*）----

private func findWriteTx(_ actions: [XiaomiAtvvAction]) -> Data? {
    for action in actions { if case .writeTx(let bytes) = action { return bytes } }
    return nil
}

private func findEvent(_ actions: [XiaomiAtvvAction], _ name: String) -> StateEvent? {
    for action in actions {
        if case .stateEvent(let event) = action, event.event == name { return event }
    }
    return nil
}

private func collectFrames(_ actions: [XiaomiAtvvAction]) -> [AudioFrame] {
    actions.compactMap { if case .audioFrame(let frame) = $0 { return frame }; return nil }
}

private func hasError(_ actions: [XiaomiAtvvAction], _ code: String) -> Bool {
    actions.contains { if case .error(let c) = $0 { return c == code }; return false }
}

/// 快速完成握手进入 ready（start + v1.0 CAPS：16kHz、帧长 120）。
private func handshakeReady(_ session: XiaomiAtvvSession, _ nowMs: Int64) {
    _ = session.start(nowMs: nowMs)
    _ = session.handleControlCommand(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78]),
                                     nowMs: nowMs + 10)
}

/// 会话私有成员反射访问（对齐 Windows session.decoder()/accumulator() 断言）。
private func decoder(of session: XiaomiAtvvSession) -> ImaAdpcmDecoder? {
    Mirror(reflecting: session).children.first { $0.label == "decoder" }?.value as? ImaAdpcmDecoder
}

private func accumulator(of session: XiaomiAtvvSession) -> FrameAccumulator? {
    Mirror(reflecting: session).children.first { $0.label == "accumulator" }?.value as? FrameAccumulator
}

private func makeSession(_ name: String,
                         options: XiaomiAtvvSession.Options = .init()) -> XiaomiAtvvSession? {
    do {
        return try XiaomiAtvvSession(options: options)
    } catch {
        check(false, "\(name): XiaomiAtvvSession init threw \(error)")
        return nil
    }
}

// ---- TestXiaomiAtvvSessionFlow ----

private func testCapsTimeout() {
    guard let session = makeSession("CapsTimeout") else { return }
    checkEqual(session.start(nowMs: 0).count, 1, "CapsTimeout.startEmitsGetCaps")
    check(session.tick(nowMs: XiaomiAtvvSession.capsTimeoutMs - 1).isEmpty,
          "CapsTimeout.beforeDeadlineSilent")
    let actions = session.tick(nowMs: XiaomiAtvvSession.capsTimeoutMs)
    check(hasError(actions, "caps_timeout"), "CapsTimeout.error")
    checkEqual(session.state, .error, "CapsTimeout.state")
}

private func testFullLifecycle() {
    guard let session = makeSession("FullLifecycle") else { return }
    // 未握手时 MIC_OPEN 不应答。
    check(session.handleControlCommand(Data([0x08]), nowMs: 0).isEmpty,
          "FullLifecycle.micOpenBeforeHandshakeIgnored")

    var actions = session.start(nowMs: 0)
    checkEqual(findWriteTx(actions), Data([0x0A, 0x01, 0x00, 0x00, 0x03, 03]),
               "FullLifecycle.getCapsBytes")
    checkEqual(session.state, .capsRequested, "FullLifecycle.capsRequested")

    actions = session.handleControlCommand(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78]),
                                           nowMs: 10)
    check(actions.isEmpty, "FullLifecycle.capsAckSilent")
    checkEqual(session.state, .ready, "FullLifecycle.ready")

    // MIC_OPEN：只回 0C 00，暂不发送 button_down。
    actions = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    checkEqual(findWriteTx(actions), Data([0x0C, 0x00]), "FullLifecycle.micOpenAck")
    checkNil(findEvent(actions, "button_down"), "FullLifecycle.noButtonDownYet")
    checkEqual(session.state, .tapPending, "FullLifecycle.tapPending")

    // 0x04 流开始（无 SYNC）：硬重置解码器。
    actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x09]), nowMs: 1010)
    check(actions.isEmpty, "FullLifecycle.streamStartSilent")
    guard let decoder = unwrap(decoder(of: session), "FullLifecycle.decoderMirror") else { return }
    checkEqual(decoder.predictor, 0, "FullLifecycle.decoderPredictorReset")
    checkEqual(decoder.stepIndex, 0, "FullLifecycle.decoderStepReset")

    // 120B ADPCM = 240 采样，不足 640 采样帧，无输出。
    check(session.handleAudioData(Data(repeating: 0x11, count: 120), nowMs: 1020).isEmpty,
          "FullLifecycle.underrunSilent")

    // 300ms 长按阈值前不发 button_down；跨过阈值才发（sessionID=1）。
    check(session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs - 1).isEmpty,
          "FullLifecycle.beforeHoldThresholdSilent")
    actions = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    guard let down = unwrap(findEvent(actions, "button_down"), "FullLifecycle.buttonDown")
    else { return }
    checkEqual(down.button, "primary", "FullLifecycle.downButton")
    checkEqual(down.sessionID, 1, "FullLifecycle.downSessionID")
    checkEqual(session.state, .streaming, "FullLifecycle.streaming")

    // 再喂 240B（480 采样）→ 累计 720 → 出首帧（640 采样，start flag）。
    actions = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 1320)
    var frames = collectFrames(actions)
    checkEqual(frames.count, 1, "FullLifecycle.firstFrameCount")
    if let first = frames.first {
        checkEqual(first.sessionID, 1, "FullLifecycle.firstFrameSession")
        checkEqual(first.seq, 1, "FullLifecycle.firstFrameSeq")
        check(first.isStart, "FullLifecycle.firstFrameStartFlag")
        check(!first.isEnd, "FullLifecycle.firstFrameNoEndFlag")
        check(!first.payload.isEmpty, "FullLifecycle.firstFramePayload")
    }

    // STOP → button_up + draining。
    actions = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    guard let up = unwrap(findEvent(actions, "button_up"), "FullLifecycle.buttonUp")
    else { return }
    checkEqual(up.button, "primary", "FullLifecycle.upButton")
    checkEqual(session.state, .draining, "FullLifecycle.draining")

    // 150ms 宽限内尾包收下：再 480B（960 采样）→ 出一帧。
    actions = session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: 2100)
    frames = collectFrames(actions)
    checkEqual(frames.count, 1, "FullLifecycle.tailFrameCount")
    if let tail = frames.first {
        checkEqual(tail.seq, 2, "FullLifecycle.tailFrameSeq")
        check(!tail.isEnd, "FullLifecycle.tailFrameNoEndFlag")
    }

    // 超宽限的尾包丢弃（tick 尚未收尾，仍处 draining）。
    check(session.handleAudioData(Data(repeating: 0x11, count: 120),
                                  nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs + 1).isEmpty,
          "FullLifecycle.lateTailDropped")

    // 宽限到期：余量补零出末帧（end flag），回 ready。
    actions = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs + 2)
    frames = collectFrames(actions)
    checkEqual(frames.count, 1, "FullLifecycle.finalFrameCount")
    if let final = frames.first {
        checkEqual(final.seq, 3, "FullLifecycle.finalFrameSeq")
        check(final.isEnd, "FullLifecycle.finalFrameEndFlag")
        check(!final.isStart, "FullLifecycle.finalFrameNoStartFlag")
    }
    checkEqual(session.state, .ready, "FullLifecycle.backToReady")

    // STOP 已收到，断开不再发 MIC_CLOSE。
    check(session.stop(nowMs: 3000).isEmpty, "FullLifecycle.stopSilent")
    checkEqual(session.state, .idle, "FullLifecycle.idle")
}

private func testReconnectRequiresRehandshakeAndStreamHardReset() {
    guard let session = makeSession("ReconnectHardReset") else { return }
    handshakeReady(session, 0)

    // RC003 坑：第二次会话不发 SYNC，0x04 必须硬重置解码器。
    // 第一次会话把 predictor 推到饱和（0x77 连发）。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x0A]), nowMs: 1010)
    _ = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    _ = session.handleAudioData(Data(repeating: 0x77, count: 120), nowMs: 1320)
    guard let decoder = unwrap(decoder(of: session), "ReconnectHardReset.decoderMirror")
    else { return }
    checkEqual(decoder.predictor, 32767, "ReconnectHardReset.saturated")

    // 流内重入 0x04（streaming 中再收 STREAM_START）：同样硬重置。
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x0A]), nowMs: 1330)
    checkEqual(decoder.predictor, 0, "ReconnectHardReset.inStreamResetPredictor")
    checkEqual(decoder.stepIndex, 0, "ReconnectHardReset.inStreamResetStep")

    // 再饱和，走完整 STOP 收尾后验证新会话的 0x04 也硬重置。
    _ = session.handleAudioData(Data(repeating: 0x77, count: 120), nowMs: 1340)
    checkEqual(decoder.predictor, 32767, "ReconnectHardReset.resaturated")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    _ = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "ReconnectHardReset.ready")

    // 第二次会话 0x04 无 SYNC → predictor/step 归零。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 3000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x0B]), nowMs: 3010)
    checkEqual(decoder.predictor, 0, "ReconnectHardReset.secondSessionPredictor")
    checkEqual(decoder.stepIndex, 0, "ReconnectHardReset.secondSessionStep")

    // 断开重连需重新握手（stop 回 idle；session id 计数保持递增不复用）。
    _ = session.stop(nowMs: 3100)
    checkEqual(session.state, .idle, "ReconnectHardReset.idle")
    _ = session.start(nowMs: 3200)
    _ = session.handleControlCommand(Data([0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78]),
                                     nowMs: 3210)
    checkEqual(session.state, .ready, "ReconnectHardReset.rehandshakeReady")
}

private func testAudioSyncResetsDecoderAndAccumulator() {
    guard let session = makeSession("AudioSync") else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x0B]), nowMs: 1010)

    // 0x0A AUDIO_SYNC：按值重置并清空帧累积器。
    _ = session.handleAudioData(Data(repeating: 0x11, count: 100), nowMs: 1020)
    guard let accumulator = unwrap(accumulator(of: session), "AudioSync.accumulatorMirror"),
          let decoder = unwrap(decoder(of: session), "AudioSync.decoderMirror")
    else { return }
    checkEqual(accumulator.pendingBytes, 100, "AudioSync.pending100")
    _ = session.handleControlCommand(Data([0x0A, 0x00, 0x00, 0x00, 0x01, 0x00, 10]), nowMs: 1030)
    checkEqual(decoder.predictor, 256, "AudioSync.predictor256")
    checkEqual(decoder.stepIndex, 10, "AudioSync.stepIndex10")
    checkEqual(accumulator.pendingBytes, 0, "AudioSync.accumulatorCleared")
    // predictor 为 BE 有符号：0xFF00 = -256。
    _ = session.handleControlCommand(Data([0x0A, 0x00, 0x00, 0x00, 0xFF, 0x00, 5]), nowMs: 1040)
    checkEqual(decoder.predictor, -256, "AudioSync.predictorNegative256")
    checkEqual(decoder.stepIndex, 5, "AudioSync.stepIndex5")

    // SYNC 清空了 slicer：SYNC 前已累积 200 采样被丢弃，再喂 220B（440 采样）
    // 不足 640 不出帧；确认长按后才继续正常出帧。
    _ = session.handleAudioData(Data(repeating: 0x11, count: 220), nowMs: 1050)
    let actions = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    guard let down = unwrap(findEvent(actions, "button_down"), "AudioSync.buttonDown")
    else { return }
    checkEqual(down.sessionID, 1, "AudioSync.sessionID1")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    _ = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "AudioSync.ready")
}

private func testUnsupportedCodecCaps() {
    // 8kHz-only → error，后续输入全部忽略。
    guard let session = makeSession("UnsupportedCodecCaps") else { return }
    _ = session.start(nowMs: 0)
    let actions = session.handleControlCommand(
        Data([0x0B, 0x01, 0x00, 0x01, 0x03, 0x00, 0x78]), nowMs: 10)
    check(hasError(actions, "unsupported_codec"), "UnsupportedCodecCaps.error")
    checkEqual(session.state, .error, "UnsupportedCodecCaps.state")
    check(session.handleControlCommand(Data([0x08]), nowMs: 100).isEmpty,
          "UnsupportedCodecCaps.micOpenIgnored")
    check(session.handleAudioData(Data(repeating: 0x11, count: 120), nowMs: 100).isEmpty,
          "UnsupportedCodecCaps.audioIgnored")
}

private func testLegacyLayoutCommands() {
    // 旧版布局：MIC_OPEN 应答带 codec 字节；MIC_CLOSE 仅 0x0D。
    guard let session = makeSession("LegacyLayoutCommands") else { return }
    _ = session.start(nowMs: 0)
    _ = session.handleControlCommand(
        Data([0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00]), nowMs: 10)
    var actions = session.handleControlCommand(Data([0x08]), nowMs: 100)
    checkEqual(findWriteTx(actions), Data([0x0C, 0x00, 0x02]), "LegacyLayoutCommands.micOpenAck")
    actions = session.stop(nowMs: 200)
    checkEqual(findWriteTx(actions), Data([0x0D]), "LegacyLayoutCommands.micClose")
}

private func testMicCloseCarriesStreamSessionID() {
    // v1 且 mic 未 STOP 时断开：MIC_CLOSE 透传 0x04 的 session id。
    guard let session = makeSession("MicCloseCarriesStreamSessionID") else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 100)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x09]), nowMs: 110)
    let actions = session.stop(nowMs: 200)
    checkEqual(findWriteTx(actions), Data([0x0D, 0x09]), "MicCloseCarriesStreamSessionID.bytes")
}

// ---- TestXiaomiAtvvSessionKeyMapping ----

private func testKeyMappingHoldToTalk() {
    // 默认 hold_to_talk，双击窗 350ms。
    guard let session = makeSession("KeyMappingHoldToTalk") else { return }
    handshakeReady(session, 0)

    // 长按：MIC_OPEN 只应答不发事件；缓冲音频在确认后流出（不丢前 300ms 语音）。
    var actions = session.handleControlCommand(Data([0x08]), nowMs: 100)
    checkNotNil(findWriteTx(actions), "KeyMappingHoldToTalk.ack")
    checkNil(findEvent(actions, "button_down"), "KeyMappingHoldToTalk.noDownOnPress")
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: 110)
    check(session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: 150).isEmpty,
          "KeyMappingHoldToTalk.bufferedSilent") // 960 采样暂存
    check(session.tick(nowMs: 399).isEmpty, "KeyMappingHoldToTalk.beforeThresholdSilent")
    actions = session.tick(nowMs: 100 + XiaomiAtvvSession.holdThresholdMs)
    guard let down = unwrap(findEvent(actions, "button_down"), "KeyMappingHoldToTalk.down")
    else { return }
    checkEqual(down.button, "primary", "KeyMappingHoldToTalk.downButton")
    checkEqual(down.sessionID, 1, "KeyMappingHoldToTalk.downSession1")
    var frames = collectFrames(actions)
    checkEqual(frames.count, 1, "KeyMappingHoldToTalk.bufferedFrameOut")
    if let first = frames.first {
        checkEqual(first.sessionID, 1, "KeyMappingHoldToTalk.bufferedFrameSession")
        check(first.isStart, "KeyMappingHoldToTalk.bufferedFrameStartFlag")
    }
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    _ = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "KeyMappingHoldToTalk.readyAfterLong")

    // 短击：缓冲音频丢弃，无 button_down/up；窗超时发 button_click。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 3000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x02]), nowMs: 3010)
    _ = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 3020)
    actions = session.handleControlCommand(Data([0x00]), nowMs: 3150) // 150ms < 300ms
    check(actions.isEmpty, "KeyMappingHoldToTalk.shortTapStopSilent")
    checkEqual(session.state, .waitSecondTap, "KeyMappingHoldToTalk.waitSecondTap")
    check(session.tick(nowMs: 3150 + 349).isEmpty, "KeyMappingHoldToTalk.beforeWindowSilent")
    actions = session.tick(nowMs: 3150 + 350)
    guard let click = unwrap(findEvent(actions, "button_click"), "KeyMappingHoldToTalk.click")
    else { return }
    checkEqual(click.button, "primary", "KeyMappingHoldToTalk.clickButton")
    checkEqual(session.state, .ready, "KeyMappingHoldToTalk.readyAfterClick")

    // 双击：窗内第二次 MIC_OPEN → button_double_click（仍应答 TX），不录音。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 5000)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 5100)
    actions = session.handleControlCommand(Data([0x08]), nowMs: 5200) // 5100+350 窗内
    checkNotNil(findEvent(actions, "button_double_click"), "KeyMappingHoldToTalk.doubleClick")
    checkNil(findEvent(actions, "button_down"), "KeyMappingHoldToTalk.noDownOnDouble")
    checkNotNil(findWriteTx(actions), "KeyMappingHoldToTalk.doubleClickAck")
    // 被双击消费的第二次按下：音频丢弃、跨阈值不发事件、松开不再发事件。
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x03]), nowMs: 5210)
    check(session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: 5220).isEmpty,
          "KeyMappingHoldToTalk.suppressedAudioDropped")
    check(session.tick(nowMs: 5600).isEmpty, "KeyMappingHoldToTalk.suppressedThresholdSilent")
    actions = session.handleControlCommand(Data([0x00]), nowMs: 5700)
    check(actions.isEmpty, "KeyMappingHoldToTalk.suppressedStopSilent")
    checkEqual(session.state, .ready, "KeyMappingHoldToTalk.readyAfterDouble")
    check(session.tick(nowMs: 6100).isEmpty, "KeyMappingHoldToTalk.noStaleWindow") // 无滞留双击窗

    // 三击：双击窗只合成一次 double_click，第三次按下正常开录（会话 id 自增）。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 7000)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 7100)
    actions = session.handleControlCommand(Data([0x08]), nowMs: 7200)
    checkNotNil(findEvent(actions, "button_double_click"), "KeyMappingHoldToTalk.tripleDoubleClick")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 7300)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 8000)
    actions = session.tick(nowMs: 8000 + XiaomiAtvvSession.holdThresholdMs)
    guard let down2 = unwrap(findEvent(actions, "button_down"), "KeyMappingHoldToTalk.thirdDown")
    else { return }
    checkEqual(down2.sessionID, 2, "KeyMappingHoldToTalk.thirdSession2")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 9000)
    _ = session.tick(nowMs: 9000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "KeyMappingHoldToTalk.readyAfterTriple")

    // sessionID 在 button_down 与 AudioFrame 间一致且逐会话自增。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 10000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x04]), nowMs: 10010)
    actions = session.tick(nowMs: 10000 + XiaomiAtvvSession.holdThresholdMs)
    guard let down3 = unwrap(findEvent(actions, "button_down"), "KeyMappingHoldToTalk.fourthDown")
    else { return }
    checkEqual(down3.sessionID, 3, "KeyMappingHoldToTalk.fourthSession3")
    actions = session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: 10310)
    frames = collectFrames(actions)
    checkEqual(frames.count, 1, "KeyMappingHoldToTalk.fourthFrameCount")
    if let first = frames.first {
        checkEqual(first.sessionID, 3, "KeyMappingHoldToTalk.fourthFrameSession")
    }
    _ = session.handleControlCommand(Data([0x00]), nowMs: 11000)
    _ = session.tick(nowMs: 11000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "KeyMappingHoldToTalk.readyAtEnd")
}

private func testKeyMappingClickToTalk() {
    // click_to_talk：MIC_OPEN 立即发 button_down，STOP 发 button_up；双击仍合成。
    var options = XiaomiAtvvSession.Options()
    options.interactionMode = .clickToTalk
    guard let session = makeSession("KeyMappingClickToTalk", options: options) else { return }
    handshakeReady(session, 0)

    var actions = session.handleControlCommand(Data([0x08]), nowMs: 100)
    guard let down = unwrap(findEvent(actions, "button_down"), "KeyMappingClickToTalk.down1")
    else { return }
    checkEqual(down.sessionID, 1, "KeyMappingClickToTalk.session1")
    checkEqual(session.state, .streaming, "KeyMappingClickToTalk.streaming")
    actions = session.handleControlCommand(Data([0x00]), nowMs: 250) // 短按 150ms
    checkNotNil(findEvent(actions, "button_up"), "KeyMappingClickToTalk.up")
    // 尾包宽限收尾后进入双击窗（短按）。
    _ = session.tick(nowMs: 250 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .waitSecondTap, "KeyMappingClickToTalk.waitSecondTap")
    actions = session.handleControlCommand(Data([0x08]), nowMs: 500) // 250+350 窗内
    checkNotNil(findEvent(actions, "button_double_click"), "KeyMappingClickToTalk.doubleClick")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 600)
    checkEqual(session.state, .ready, "KeyMappingClickToTalk.ready")
    // 窗后按下：立即开录新会话。
    actions = session.handleControlCommand(Data([0x08]), nowMs: 5000)
    guard let down2 = unwrap(findEvent(actions, "button_down"), "KeyMappingClickToTalk.down2")
    else { return }
    checkEqual(down2.sessionID, 2, "KeyMappingClickToTalk.session2")
}

// ---- TestXiaomiAtvvSessionClickTapTimeoutSilent ----

private func testClickToTalkTapTimeoutSilent() {
    // click_to_talk：短按已由 button_down/up 完整表达并开录，双击窗超时不得再补发
    // button_click（否则协调器 click 分支把空闲态无 duration_ms 的 click 当启动，
    // 产生永远收不到音频的幽灵会话）。
    var options = XiaomiAtvvSession.Options()
    options.interactionMode = .clickToTalk
    guard let session = makeSession("ClickToTalkTapTimeoutSilent", options: options) else { return }
    handshakeReady(session, 0)

    var actions = session.handleControlCommand(Data([0x08]), nowMs: 100)
    checkNotNil(findEvent(actions, "button_down"), "ClickTapTimeoutSilent.downOnPress") // 按下即开录
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: 110)
    _ = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 120)
    actions = session.handleControlCommand(Data([0x00]), nowMs: 200) // 短按 100ms
    checkNotNil(findEvent(actions, "button_up"), "ClickTapTimeoutSilent.up")
    // 尾包宽限收尾：click 短按武装双击窗。
    _ = session.tick(nowMs: 200 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .waitSecondTap, "ClickTapTimeoutSilent.waitSecondTap")
    // 双击窗超时：无任何补发事件，仅回 ready。
    actions = session.tick(nowMs: 200 + 350)
    check(actions.isEmpty, "ClickTapTimeoutSilent.timeoutSilent")
    checkEqual(session.state, .ready, "ClickTapTimeoutSilent.ready")

    // tick 未跑、窗已过恰好 MIC_OPEN：同样不补发 button_click，直接开新会话。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 1100)
    _ = session.tick(nowMs: 1100 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .waitSecondTap, "ClickTapTimeoutSilent.waitSecondTap2")
    actions = session.handleControlCommand(Data([0x08]), nowMs: 1100 + 350 + 10)
    checkNil(findEvent(actions, "button_click"), "ClickTapTimeoutSilent.noLateClick")
    checkNotNil(findEvent(actions, "button_down"), "ClickTapTimeoutSilent.newDown") // 新按下立即开录
    checkEqual(session.state, .streaming, "ClickTapTimeoutSilent.streaming")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    _ = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
}

private func testHoldToTalkTapTimeoutEmitsClick() {
    // 对照：hold_to_talk 窗超时仍补发 button_click（短击未发过任何事件，
    // 协调器 hold 分支对 click 是无害 no-op）。tick 未跑窗已过的路径同样补发。
    guard let session = makeSession("HoldToTalkTapTimeoutEmitsClick") else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 100)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 200)
    checkEqual(session.state, .waitSecondTap, "HoldTapTimeoutEmitsClick.waitSecondTap")
    var actions = session.tick(nowMs: 200 + 350)
    checkNotNil(findEvent(actions, "button_click"), "HoldTapTimeoutEmitsClick.click")
    checkEqual(session.state, .ready, "HoldTapTimeoutEmitsClick.ready")

    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 1100)
    checkEqual(session.state, .waitSecondTap, "HoldTapTimeoutEmitsClick.waitSecondTap2")
    actions = session.handleControlCommand(Data([0x08]), nowMs: 1100 + 350 + 10)
    checkNotNil(findEvent(actions, "button_click"), "HoldTapTimeoutEmitsClick.lateClick")
    checkEqual(session.state, .tapPending, "HoldTapTimeoutEmitsClick.tapPending")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000)
}

// ---- TestXiaomiAtvvSessionReopenRejectWindow ----

private func testReopenRejectWindow() {
    // 规格：STOP 后 300ms 内拒绝重开会话（防遥控器抖动/急速重开）。
    guard let session = makeSession("ReopenRejectWindow") else { return } // hold_to_talk
    handshakeReady(session, 0)

    // 长按完整键程：MIC_OPEN → STREAM_START → 长按确认 → STOP → draining。
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: 1010)
    var actions = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    checkNotNil(findEvent(actions, "button_down"), "ReopenRejectWindow.down")
    actions = session.handleControlCommand(Data([0x00]), nowMs: 2000)
    checkNotNil(findEvent(actions, "button_up"), "ReopenRejectWindow.up")
    checkEqual(session.state, .draining, "ReopenRejectWindow.draining")

    // 拒绝窗内（draining，STOP 后 100ms）MIC_OPEN：忽略，无 ACK、无事件、状态不变。
    actions = session.handleControlCommand(Data([0x08]), nowMs: 2100)
    check(actions.isEmpty, "ReopenRejectWindow.inWindowIgnored")
    checkEqual(session.state, .draining, "ReopenRejectWindow.stillDraining")

    // 尾包宽限到期收尾回 ready，但拒绝窗（300ms）仍未满：MIC_OPEN 依旧忽略。
    _ = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .ready, "ReopenRejectWindow.ready")
    actions = session.handleControlCommand(Data([0x08]), nowMs: 2200) // STOP 后 200ms
    check(actions.isEmpty, "ReopenRejectWindow.inWindowIgnoredAfterReady")
    checkEqual(session.state, .ready, "ReopenRejectWindow.stillReady")

    // 窗满（STOP 后 ≥300ms）：正常应答并开新按下。
    actions = session.handleControlCommand(Data([0x08]),
                                           nowMs: 2000 + XiaomiAtvvSession.reopenRejectMs)
    checkNotNil(findWriteTx(actions), "ReopenRejectWindow.afterWindowAck")
    checkEqual(session.state, .tapPending, "ReopenRejectWindow.tapPending")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2400) // 短击收尾
    _ = session.tick(nowMs: 2400 + 350 + 1) // 双击窗超时回 ready
    checkEqual(session.state, .ready, "ReopenRejectWindow.readyAtEnd")
}

private func testReopenRejectWindowExemptsDoubleClick() {
    // 双击路径不受拒绝窗影响：click 短按的 STOP 也武装拒绝窗，
    // 但第二击走 waitSecondTap 分支，窗内照常合成 button_double_click。
    var options = XiaomiAtvvSession.Options()
    options.interactionMode = .clickToTalk
    guard let session = makeSession("ReopenRejectWindowExemptsDoubleClick", options: options)
    else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 1100) // 短按 → draining + 拒绝窗
    _ = session.tick(nowMs: 1100 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(session.state, .waitSecondTap, "ReopenExemptsDoubleClick.waitSecondTap")
    let actions = session.handleControlCommand(Data([0x08]), nowMs: 1200) // STOP 后 100ms，拒绝窗内
    checkNotNil(findEvent(actions, "button_double_click"), "ReopenExemptsDoubleClick.doubleClick")
    _ = session.handleControlCommand(Data([0x00]), nowMs: 1300)
}

private func testStopClearsRejectWindow() {
    // stop 复位清窗：重连握手后立即可开录（不残留拒绝窗）。
    guard let session = makeSession("StopClearsRejectWindow") else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x08]), nowMs: 1000)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: 1010)
    _ = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    _ = session.handleControlCommand(Data([0x00]), nowMs: 2000) // 武装拒绝窗（至 2300）
    _ = session.stop(nowMs: 2050) // 断连复位
    handshakeReady(session, 2100)
    let actions = session.handleControlCommand(Data([0x08]), nowMs: 2150) // 旧窗内时刻
    checkNotNil(findWriteTx(actions), "StopClearsRejectWindow.ack")
    checkEqual(session.state, .tapPending, "StopClearsRejectWindow.tapPending")
}

// ---- TestXiaomiAtvvStreamStartOpensSession（2 Pro 直开入径）----

private func testStreamStartOpensSession() {
    // 2 Pro 入径（真机实测）：按下语音键直接发 0x04 <interaction> <codec> <sid>
    //（按下+开流一体帧，无 0x08、主机不写 0x0C ACK），松开发 0x00（可带尾字节）。
    // hold_to_talk：0x04 → tapPending（无 ACK、无事件），音频即刻缓冲。
    guard let session = makeSession("StreamStartOpensSession") else { return }
    handshakeReady(session, 0)

    var actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x03]), nowMs: 1000)
    check(actions.isEmpty, "StreamStartOpensSession.silent") // 无 ACK、无事件
    checkEqual(session.state, .tapPending, "StreamStartOpensSession.tapPending")

    // 流已激活：音频立即缓冲（240B=480 采样 <640 不出帧）；跨阈值确认长按。
    check(session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 1010).isEmpty,
          "StreamStartOpensSession.bufferedSilent")
    actions = session.tick(nowMs: 1000 + XiaomiAtvvSession.holdThresholdMs)
    guard let down = unwrap(findEvent(actions, "button_down"), "StreamStartOpensSession.down")
    else { return }
    checkEqual(down.sessionID, 1, "StreamStartOpensSession.session1")
    checkEqual(session.state, .streaming, "StreamStartOpensSession.streaming")

    actions = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 1310) // 累计 960 → 出首帧
    var frames = collectFrames(actions)
    checkEqual(frames.count, 1, "StreamStartOpensSession.firstFrame")
    if let first = frames.first {
        checkEqual(first.sessionID, 1, "StreamStartOpensSession.firstFrameSession")
        check(first.isStart, "StreamStartOpensSession.firstFrameStartFlag")
    }

    // STOP 带尾字节（00 02）：button_up + draining，宽限到期出末帧回 ready。
    actions = session.handleControlCommand(Data([0x00, 0x02]), nowMs: 2000)
    checkNotNil(findEvent(actions, "button_up"), "StreamStartOpensSession.up")
    checkEqual(session.state, .draining, "StreamStartOpensSession.draining")
    actions = session.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    frames = collectFrames(actions)
    checkEqual(frames.count, 1, "StreamStartOpensSession.finalFrame")
    if let final = frames.first {
        check(final.isEnd, "StreamStartOpensSession.finalFrameEndFlag")
    }
    checkEqual(session.state, .ready, "StreamStartOpensSession.ready")

    // codec 非 16kHz（byte2=0x01）：上报错误、不开会话、不回 ACK。
    actions = session.handleControlCommand(Data([0x04, 0x03, 0x01, 0x05]), nowMs: 3000)
    check(hasError(actions, "unsupported_codec"), "StreamStartOpensSession.unsupportedCodec")
    checkNil(findWriteTx(actions), "StreamStartOpensSession.noAckOnBadCodec")
    checkEqual(session.state, .ready, "StreamStartOpensSession.readyAfterBadCodec")

    // STOP 后 300ms 重开拒绝窗对 0x04 同样生效：先跑一轮长按键程武装拒绝窗。
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x06]), nowMs: 4000)
    _ = session.tick(nowMs: 4000 + XiaomiAtvvSession.holdThresholdMs)
    _ = session.handleControlCommand(Data([0x00, 0x02]), nowMs: 5000) // draining，拒绝窗至 5300
    actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x07]), nowMs: 5100)
    check(actions.isEmpty, "StreamStartOpensSession.inWindowIgnored") // 窗内忽略
    checkEqual(session.state, .draining, "StreamStartOpensSession.stillDraining")

    // 窗外 0x04 重开：draining 收尾（本轮无音频故无末帧）后直接开新按下。
    actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x08]), nowMs: 5400)
    checkNil(findWriteTx(actions), "StreamStartOpensSession.noAckOnReopen")
    checkEqual(session.state, .tapPending, "StreamStartOpensSession.reopenTapPending")

    // 短击松开 → 双击窗；窗内第二个 0x04 合成 button_double_click，按下被消费
    //（无 ACK、音频丢弃、跨阈值不确认、松手无事件）。
    actions = session.handleControlCommand(Data([0x00, 0x02]), nowMs: 5410)
    check(actions.isEmpty, "StreamStartOpensSession.shortTapStopSilent")
    checkEqual(session.state, .waitSecondTap, "StreamStartOpensSession.waitSecondTap")
    actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x09]), nowMs: 5500)
    checkNotNil(findEvent(actions, "button_double_click"), "StreamStartOpensSession.doubleClick")
    checkNil(findWriteTx(actions), "StreamStartOpensSession.noAckOnDouble")
    checkEqual(session.state, .tapPending, "StreamStartOpensSession.suppressedTapPending")
    check(session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 5510).isEmpty,
          "StreamStartOpensSession.suppressedAudioDropped")
    check(session.tick(nowMs: 5500 + XiaomiAtvvSession.holdThresholdMs).isEmpty,
          "StreamStartOpensSession.suppressedThresholdSilent")
    actions = session.handleControlCommand(Data([0x00, 0x02]), nowMs: 5700)
    check(actions.isEmpty, "StreamStartOpensSession.suppressedStopSilent")
    checkEqual(session.state, .ready, "StreamStartOpensSession.readyAtEnd")
}

private func testStreamStartClickToTalkImmediate() {
    // click_to_talk：0x04 直开立即发 button_down，音频即时流出。
    var options = XiaomiAtvvSession.Options()
    options.interactionMode = .clickToTalk
    guard let session = makeSession("StreamStartClickToTalkImmediate", options: options)
    else { return }
    handshakeReady(session, 0)
    var actions = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: 100)
    checkNil(findWriteTx(actions), "StreamStartClickImmediate.noAck")
    guard let down = unwrap(findEvent(actions, "button_down"), "StreamStartClickImmediate.down")
    else { return }
    checkEqual(down.sessionID, 1, "StreamStartClickImmediate.session1")
    checkEqual(session.state, .streaming, "StreamStartClickImmediate.streaming")
    _ = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 110)
    actions = session.handleAudioData(Data(repeating: 0x11, count: 240), nowMs: 120)
    let frames = collectFrames(actions)
    checkEqual(frames.count, 1, "StreamStartClickImmediate.firstFrame")
    if let first = frames.first {
        check(first.isStart, "StreamStartClickImmediate.firstFrameStartFlag")
    }
    actions = session.handleControlCommand(Data([0x00, 0x02]), nowMs: 200)
    checkNotNil(findEvent(actions, "button_up"), "StreamStartClickImmediate.up")
}

private func testMicCloseCarriesStreamStartSessionID() {
    // 0x04 直开路径下断开：MIC_CLOSE 透传 0x04 byte3 的会话计数。
    guard let session = makeSession("MicCloseCarriesStreamStartSessionID") else { return }
    handshakeReady(session, 0)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x42]), nowMs: 100)
    let actions = session.stop(nowMs: 200)
    checkEqual(findWriteTx(actions), Data([0x0D, 0x42]),
               "MicCloseCarriesStreamStartSessionID.bytes")
}

// ---- TestXiaomiAtvvSessionEncoderResetPerSession ----

/// hold_to_talk：完成一轮「按下→暂存→确认长按」，返回首帧 payload（编码器状态探针）。
private func holdFirstFramePayload(_ session: XiaomiAtvvSession, _ t: Int64,
                                   _ name: String) -> Data? {
    _ = session.handleControlCommand(Data([0x08]), nowMs: t)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: t + 10)
    _ = session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: t + 20) // 960 采样 → 暂存 1 帧
    let actions = session.tick(nowMs: t + XiaomiAtvvSession.holdThresholdMs)
    let frames = collectFrames(actions)
    checkEqual(frames.count, 1, "\(name).frameCount")
    check(frames.first?.isStart ?? false, "\(name).startFlag")
    return unwrap(frames.first?.payload, "\(name).payload")
}

/// click_to_talk：「按下→立即开录→喂音频」，返回首帧 payload。
private func clickFirstFramePayload(_ session: XiaomiAtvvSession, _ t: Int64,
                                    _ name: String) -> Data? {
    _ = session.handleControlCommand(Data([0x08]), nowMs: t)
    _ = session.handleControlCommand(Data([0x04, 0x03, 0x02, 0x01]), nowMs: t + 10)
    let actions = session.handleAudioData(Data(repeating: 0x11, count: 480), nowMs: t + 20)
    let frames = collectFrames(actions)
    checkEqual(frames.count, 1, "\(name).frameCount")
    check(frames.first?.isStart ?? false, "\(name).startFlag")
    return unwrap(frames.first?.payload, "\(name).payload")
}

private func testEncoderResetPerSession() {
    // 上一会话的 Opus 编码器残余状态不得污染下一会话（对齐固件 audio_pipeline.c
    // 每次会话开始 OPUS_RESET_STATE）：复用 session 的第二会话首帧须与全新
    // session 的首帧逐字节一致。

    // 全新 session 的首会话首帧（编码器出厂状态）。
    guard let fresh = makeSession("EncoderResetPerSession.fresh"),
          let reused = makeSession("EncoderResetPerSession.reused") else { return }
    handshakeReady(fresh, 0)
    guard let freshFirst = holdFirstFramePayload(fresh, 1000, "EncoderResetPerSession.freshFirst")
    else { return }

    // 复用 session：会话 1（多编几帧改变内部状态）结束后开会话 2。
    handshakeReady(reused, 0)
    guard let reusedFirst = holdFirstFramePayload(reused, 1000, "EncoderResetPerSession.reusedFirst")
    else { return }
    checkEqual(reusedFirst, freshFirst, "EncoderResetPerSession.firstSessionSanity") // sanity：首会话本就该一致
    _ = reused.handleAudioData(Data(repeating: 0x77, count: 480), nowMs: 1400) // 会话 1 多喂两帧
    _ = reused.handleControlCommand(Data([0x00]), nowMs: 2000)
    _ = reused.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(reused.state, .ready, "EncoderResetPerSession.ready")
    guard let reusedSecond = holdFirstFramePayload(reused, 10000, "EncoderResetPerSession.reusedSecond")
    else { return } // 远超拒绝窗
    checkEqual(reusedSecond, freshFirst, "EncoderResetPerSession.resetMatchesFresh") // 关键：reset 后与全新逐字节一致
}

private func testEncoderResetPerSessionClickToTalk() {
    // click_to_talk 立即路径同样每会话 reset（beginPress 非 suppressed 分支覆盖）。
    var options = XiaomiAtvvSession.Options()
    options.interactionMode = .clickToTalk
    guard let fresh = makeSession("EncoderResetClick.fresh", options: options),
          let reused = makeSession("EncoderResetClick.reused", options: options) else { return }
    handshakeReady(fresh, 0)
    guard let freshFirst = clickFirstFramePayload(fresh, 1000, "EncoderResetClick.freshFirst")
    else { return }

    handshakeReady(reused, 0)
    guard let reusedFirst = clickFirstFramePayload(reused, 1000, "EncoderResetClick.reusedFirst")
    else { return }
    checkEqual(reusedFirst, freshFirst, "EncoderResetClick.firstSessionSanity")
    _ = reused.handleControlCommand(Data([0x00]), nowMs: 2000) // 按压 1000ms（长按路径）
    _ = reused.tick(nowMs: 2000 + XiaomiAtvvSession.audioTailGraceMs)
    checkEqual(reused.state, .ready, "EncoderResetClick.ready")
    guard let reusedSecond = clickFirstFramePayload(reused, 10000, "EncoderResetClick.reusedSecond")
    else { return }
    checkEqual(reusedSecond, freshFirst, "EncoderResetClick.resetMatchesFresh")
}

private func testControlCommandSliceInput() {
    // 切片 Data（startIndex≠0）进 handleControlCommand 不得越界崩溃（入口归一化）：
    // 与整段 Data 输入行为一致，完成 CAPS 握手进入 ready。
    guard let session = makeSession("ControlCommandSlice") else { return }
    _ = session.start(nowMs: 0)
    let wrapped = Data([0xAA, 0xBB, 0x0B, 0x01, 0x00, 0x02, 0x03, 0x00, 0x78, 0xCC])
    let slice: Data = wrapped[2..<9]
    check(slice.startIndex == 2, "ControlCommandSlice.startIndexPreserved") // 前置条件
    _ = session.handleControlCommand(slice, nowMs: 10)
    checkEqual(session.state, .ready, "ControlCommandSlice.ready")
}

func runAtvvSessionTests() {
    testCapsTimeout()
    testFullLifecycle()
    testReconnectRequiresRehandshakeAndStreamHardReset()
    testAudioSyncResetsDecoderAndAccumulator()
    testUnsupportedCodecCaps()
    testLegacyLayoutCommands()
    testMicCloseCarriesStreamSessionID()
    testKeyMappingHoldToTalk()
    testKeyMappingClickToTalk()
    testClickToTalkTapTimeoutSilent()
    testHoldToTalkTapTimeoutEmitsClick()
    testReopenRejectWindow()
    testReopenRejectWindowExemptsDoubleClick()
    testStopClearsRejectWindow()
    testStreamStartOpensSession()
    testStreamStartClickToTalkImmediate()
    testMicCloseCarriesStreamStartSessionID()
    testEncoderResetPerSession()
    testEncoderResetPerSessionClickToTalk()
    testControlCommandSliceInput()
}
