import Foundation
import VoiceStickCore

// N1 闸6b：三个具体类满足 Core 出口协议（框架实现全留 App）。
extension InputInjector: VoiceStickInputServing {}

extension SubtitleController: VoiceStickSubtitleServing {}

extension DebugAudioRecorder: VoiceStickDebugRecordServing {}
