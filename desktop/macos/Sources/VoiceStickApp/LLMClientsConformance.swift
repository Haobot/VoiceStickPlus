import Foundation
import VoiceStickCore

// N1 切5 闸3/4：两个 LLM 客户端满足 Core 出口协议（实现面/默认参原样）。
extension LLMRefinementClient: RefinerServing {}

extension LLMTranslationClient: TranslatorServing {}
