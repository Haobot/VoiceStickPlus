import Foundation
import VoiceStickCore

// ASR corpus.context 直传载荷单测（用户数据飞轮 P1-6；与 Windows asr_protocol
// FitHotwordsToCorpusBudget 口径互补——本函数只做序列化，预算由调用方负责）。

func runAsrCorpusTests() {
    // 热词段。
    guard let hotwordsOnly = unwrap(
        ASRCorpus.contextJSON(hotwords: ["语气词", "  ", "AGENTS.md"], dialogContext: []),
        "corpus：热词段可序列化"
    ) else { return }
    var object = (try? JSONSerialization.jsonObject(with: Data(hotwordsOnly.utf8))) as? [String: Any]
    checkNotNil(object, "corpus：热词段 JSON 合法")
    checkEqual(
        object?["hotwords"] as? [[String: String]],
        [["word": "语气词"], ["word": "AGENTS.md"]],
        "corpus：热词段过滤空白项并保留词形"
    )
    check(object?["dialog_ctx"] == nil, "corpus：无对话上下文时不发 dialog_ctx")

    // dialog_ctx 段（旧→新语序原样保留）。
    guard let dialogOnly = unwrap(
        ASRCorpus.contextJSON(hotwords: [], dialogContext: ["上一句", "上上句"]),
        "corpus：对话段可序列化"
    ) else { return }
    object = (try? JSONSerialization.jsonObject(with: Data(dialogOnly.utf8))) as? [String: Any]
    checkEqual(
        object?["dialog_ctx"] as? [String],
        ["上一句", "上上句"],
        "corpus：对话段保序"
    )
    check(object?["hotwords"] == nil, "corpus：无热词时不发 hotwords 键")

    // 两段并存。
    guard let both = unwrap(
        ASRCorpus.contextJSON(hotwords: ["Claude"], dialogContext: ["上一句"]),
        "corpus：两段并存可序列化"
    ) else { return }
    object = (try? JSONSerialization.jsonObject(with: Data(both.utf8))) as? [String: Any]
    checkNotNil(object?["hotwords"], "corpus：并存含热词段")
    checkNotNil(object?["dialog_ctx"], "corpus：并存含对话段")

    // 全空（含纯空白）不带 corpus。
    checkNil(ASRCorpus.contextJSON(hotwords: [], dialogContext: []), "corpus：全空返回 nil")
    checkNil(
        ASRCorpus.contextJSON(hotwords: ["  "], dialogContext: ["\n"]),
        "corpus：纯空白返回 nil"
    )
}
