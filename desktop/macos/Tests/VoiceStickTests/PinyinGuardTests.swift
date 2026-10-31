import Foundation
import VoiceStickCore

// 拼音同音/近音判定单测（自 Windows pinyin_guard 移植语义；与 core_tests
// 的 PinyinSameOrNear 口径同源——M0 spike 实测口径）+ 纠错对入表闸门标注。

func runPinyinGuardTests() {
    // ---- 单字判定 ----
    check(PinyinGuard.sameOrNear("渍", "词"), "同音：渍 zì/词 cí（韵母交集，声母不参与）")
    check(PinyinGuard.sameOrNear("马", "打"), "同音：马 mǎ/打 dǎ（声母不同仍放行）")
    check(PinyinGuard.sameOrNear("设", "识"), "近音：设 shè/识 shí（模糊韵母 e/i + 声母交集）")
    check(PinyinGuard.sameOrNear("这", "之"), "近音：这 zhè/之 zhī")
    check(!PinyinGuard.sameOrNear("安", "应"), "异音：安 an/应 ing（韵母无交集）")
    check(PinyinGuard.sameOrNear("A", "a"), "ASCII：大小写折叠相等")
    check(!PinyinGuard.sameOrNear("a", "b"), "ASCII：不同字母判不等")

    // ---- 等长逐字近音 ----
    check(PinyinGuard.sameOrNearText("鱼器渍", "语气词"), "sameOrNearText：鱼器渍/语气词（验收基准）")
    check(PinyinGuard.sameOrNearText("设别", "识别"), "sameOrNearText：设别/识别（模糊韵母逐字）")
    check(!PinyinGuard.sameOrNearText("鱼器渍", "语气"), "sameOrNearText：码点数不等判 false")
    check(PinyinGuard.sameOrNearText("", ""), "sameOrNearText：两个空串 true")

    // ---- 纠错对入表闸门：record 自动标注 samePronunciation ----
    var store: [CorrectionPair] = []
    CorrectionPairs.record(
        &store,
        pairs: [CorrectionReplacement(before: "鱼器渍", after: "语气词"),
                CorrectionReplacement(before: "克劳德", after: "Claude")],
        nowTs: 1000
    )
    checkEqual(store.count, 2, "闸门：两对均入表（非同音不丢弃，只降权）")
    checkEqual(store[0].samePronunciation, true, "闸门：同音对标注 true")
    checkEqual(store[1].samePronunciation, false, "闸门：拉丁对标注 false")

    // few-shot 排序：同音对优先（桶优先之后第二序）。
    let ranked = CorrectionPairs.trimForPrompt(store, maxPairs: 8)
    checkEqual(ranked.first?.before, "鱼器渍", "闸门：few-shot 同音对优先")

    // JSON 往返保留标注。
    let url = FileManager.default.temporaryDirectory
        .appendingPathComponent("voicestick_pinyin_gate.json")
    try? FileManager.default.removeItem(at: url)
    CorrectionPairs.save(store, url: url)
    // save 按字典序排序：克(U+514B) < 鱼(U+9C7C) → [0]=克劳德、[1]=鱼器渍。
    let loaded = CorrectionPairs.load(url: url)
    checkEqual(loaded[0].samePronunciation, false, "闸门：JSON 往返保留非同音标注")
    checkEqual(loaded[1].samePronunciation, true, "闸门：JSON 往返保留标注")
    try? FileManager.default.removeItem(at: url)
}
