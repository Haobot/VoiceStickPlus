import Foundation
import VoiceStickCore

// 流式精修 SSE 解析 + 热词评分裁剪单测（自 Windows llm_chat_client ParseSseLine 与
// hotword_selector 移植语义；对应 core_tests.cc 的 TestParseSseLine / TestHotwordSelector）。

func runLlmSseParserTests() {
    checkEqual(
        LlmSseParser.parseLine("data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}"),
        .token("你好"),
        "SSE data 行提取 delta token"
    )
    checkEqual(LlmSseParser.parseLine("data: [DONE]"), .done, "SSE [DONE]")
    checkEqual(LlmSseParser.parseLine("event: message"), LlmSseEvent.none, "SSE 非 data 行忽略")
    checkEqual(LlmSseParser.parseLine(": keep-alive"), .none, "SSE 注释行忽略")
    checkEqual(
        LlmSseParser.parseLine("data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}"),
        .none,
        "SSE delta 无 content 忽略"
    )
    checkEqual(
        LlmSseParser.parseLine("data: {\"choices\":[{\"finish_reason\":\"stop\"}]}"),
        .none,
        "SSE 收尾帧（无 delta）忽略"
    )
    checkEqual(LlmSseParser.parseLine("data: not-json"), .none, "SSE JSON 损坏忽略")
    checkEqual(
        LlmSseParser.parseLine("data:{\"choices\":[{\"delta\":{\"content\":\"x\"}}]}"),
        .token("x"),
        "SSE data: 后无空格也接受"
    )
    // 累积语义：多行 token 拼出全文。
    var full = ""
    for line in [
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}",
        "data: {\"choices\":[{\"delta\":{\"content\":\", \"}}]}",
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}",
        "data: [DONE]"
    ] {
        if case .token(let token) = LlmSseParser.parseLine(line) { full += token }
    }
    checkEqual(full, "Hello, world", "SSE 多行 token 拼接全文")
}

func runHotwordSelectorTests() {
    // ---- 合法性 ----
    check(HotwordSelector.isValidHotword("AGENTS.md"), "合法热词（拉丁）")
    check(HotwordSelector.isValidHotword("语音棒"), "合法热词（汉字）")
    check(!HotwordSelector.isValidHotword("has space"), "含空白非法")
    check(!HotwordSelector.isValidHotword(String(repeating: "超", count: 11)), "超 10 汉字非法")
    check(!HotwordSelector.isValidHotword(String(repeating: "a", count: 31)), "超 30 拉丁字符非法")
    check(!HotwordSelector.isValidHotword(""), "空串非法")

    // ---- 评分 ----
    let now: Int64 = 1_800_000_000
    let frequent = HotwordUsage(count: 50, lastUsedTs: now - 100, source: "mined")
    let stale = HotwordUsage(count: 1, lastUsedTs: now - 90 * 24 * 3600, source: "mined")
    let manual = HotwordUsage(count: 0, lastUsedTs: 0, source: "manual")
    check(
        HotwordSelector.score(frequent, nowS: now) > HotwordSelector.score(stale, nowS: now),
        "高频新近词评分高于低频陈旧词"
    )
    check(
        HotwordSelector.score(manual, nowS: now) > HotwordSelector.score(stale, nowS: now),
        "手动词（无使用记录）高于陈旧挖掘词"
    )
    checkEqual(
        HotwordSelector.score(HotwordUsage(count: 0, lastUsedTs: 0, source: "mined"), nowS: now),
        0.0,
        "零记录挖掘词评分为 0"
    )

    // ---- 排序与裁剪（镜像 hotword_select.py 自测断言）----
    var store: HotwordUsageStore = [
        "AGENTS.md": HotwordUsage(count: 10, lastUsedTs: now - 60, source: "manual"),
        "stale_word": HotwordUsage(count: 8, lastUsedTs: now - 120 * 24 * 3600, source: "mined"),
        "fresh_low": HotwordUsage(count: 1, lastUsedTs: now - 10, source: "mined")
    ]
    // never_used 不在 store 内 → 按 manual 处理（新加的词最先被裁的旧缺陷修复）。
    let words = ["stale_word", "AGENTS.md", "fresh_low", "never_used", "bad word"]
    let ranked = HotwordSelector.rank(store, hotwords: words, nowS: now)
    checkEqual(ranked.count, 4, "非法词（含空格）被过滤")
    checkEqual(ranked, ["AGENTS.md", "stale_word", "never_used", "fresh_low"],
               "评分排序确定性（高频手动 > 陈旧高频 > 库外按 manual > 低频新词）")
    check(
        ranked.firstIndex(of: "never_used")! < ranked.firstIndex(of: "fresh_low")!,
        "库外新词（按 manual 加权）不被排到最后（修复按插入序裁剪）"
    )
    let trimmed = HotwordSelector.trimForPrompt(store, hotwords: words, maxWords: 2, nowS: now)
    checkEqual(trimmed, Array(ranked.prefix(2)), "裁剪取评分 top-N")
    checkEqual(HotwordSelector.trimForPrompt(store, hotwords: words, maxWords: 0, nowS: now), [], "maxWords=0 返回空")

    // ---- 使用记录（大小写不敏感）----
    HotwordSelector.recordUsage(&store, text: "编辑 agents.md 与 Agents.MD 文档", hotwords: ["AGENTS.md", "fresh_low"], nowS: now)
    checkEqual(store["AGENTS.md"]?.count, 11, "命中计数 +1")
    checkEqual(store["AGENTS.md"]?.lastUsedTs, now, "命中刷新时间戳")
    checkEqual(store["fresh_low"]?.count, 1, "未出现词不计数")

    // ---- JSON 往返 ----
    let url = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("vs-hotword-usage-test.json")
    try? FileManager.default.removeItem(at: url)
    HotwordSelector.saveUsage(store, url: url)
    let loaded = HotwordSelector.loadUsage(url: url)
    checkEqual(loaded.count, store.count, "JSON 往返条目数一致")
    checkEqual(loaded["AGENTS.md"], store["AGENTS.md"], "JSON 往返字段一致")
    checkEqual(
        HotwordSelector.loadUsage(url: url.appendingPathExtension("missing")).count, 0,
        "文件缺失返回空 store"
    )
}

func runActiveSecretTests() {
    // 内置凭据回退（对齐 Windows ResolveActiveString）：配置值优先（trim 非空），
    // 空则回退内置；不反向写回。
    checkEqual(resolveActiveString("cfg-value", builtin: "builtin"), "cfg-value", "Active 配置值优先")
    checkEqual(resolveActiveString("  cfg-value  ", builtin: "builtin"), "cfg-value", "Active 配置值 trim")
    checkEqual(resolveActiveString("", builtin: "builtin"), "builtin", "Active 空配置回退内置")
    checkEqual(resolveActiveString("   ", builtin: "builtin"), "builtin", "Active 空白配置回退内置")
    checkEqual(resolveActiveString("", builtin: ""), "", "Active 双空保持空（公开构建语义）")
    checkEqual(resolveActiveString("cfg", builtin: ""), "cfg", "Active 无内置不影响配置值")
}
