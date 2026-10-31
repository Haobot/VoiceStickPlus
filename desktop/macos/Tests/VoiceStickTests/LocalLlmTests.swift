import Foundation
@testable import VoiceStickCore

/// 内嵌 LLM 路径（B/C 阶段）单测：
/// 1. det v5 确定性清理（FILLER_CASES 从评测 harness 逐例移植）；
/// 2. editform 守卫（applyEditInstructions 语义：热词命中/原文存在/上限/大小写定位）；
/// 3. 档位解析（默认档/覆盖/~ 展开/缺失）；
/// 4. GBNF 枚举语法构造；
/// 5. 引擎真模型冒烟（dylib/模型未就位时 SKIP——不伪造结果房规）。
func runLocalLlmTests() {
    testDeterministicCleanCases()
    testApplyEditInstructions()
    testLocalLlmTierResolution()
    testAnswerEnumGrammar()
    testLlamaEngineSmokeIfAvailable()
}

// MARK: - det v5（评测 FILLER_CASES 移植）

private func testDeterministicCleanCases() {
    // (输入, 期望)——与 scripts/local_model_eval/run_refine_bench.py FILLER_CASES 同源。
    let cases: [(String, String)] = [
        ("嗯，帮我打开浏览器。", "帮我打开浏览器。"),
        ("那个，今天天气不错。", "今天天气不错。"),
        ("我我我想去吃火锅。", "我想去吃火锅。"),
        // det v5 只做句首剥离/口吃/CJK 空格——句中填充词属 editform 层（评测
        // FILLER_CASES 的期望是 det+editform 双层后，移植时按 L1 能力折算）。
        ("嗯 呃 这个项目 嗯 用的是蓝牙连接", "这个项目嗯用的是蓝牙连接"),
        ("然后呢，明天下午三点提醒我开会。", "明天下午三点提醒我开会。"),
        ("啊，那个，现在的会议记录整理一下。", "现在的会议记录整理一下。"),
        ("行行行，我明白了。", "行，我明白了。"),
        ("嗯，请帮我把这个、那个文件打印三份。", "请帮我把这个、那个文件打印三份。"),
        ("就是，下周三之前要完成代码审查。", "下周三之前要完成代码审查。"),
        ("嗯那个把cpu使用率报警阈值设为百分之八十", "那个把cpu使用率报警阈值设为百分之八十"),
    ]
    for (index, (input, expected)) in cases.enumerated() {
        let output = LocalEditformRefiner.deterministicClean(input)
        checkEqual(output, expected, "LLM.det\(index)（「\(input)」→「\(output)」）")
    }
    // CJK 间空格折叠。
    checkEqual(
        LocalEditformRefiner.deterministicClean("蓝牙 连接 不稳定"),
        "蓝牙连接不稳定", "LLM.detCjkSpace")
}

// MARK: - editform 守卫

private func testApplyEditInstructions() {
    let hotwords = ["AGENTS.md", "GitHub", "蓝牙", "Ogg"]

    // 基本替换 + 删除（评测 few-shot 首例语义）。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "嗯，帮我编辑 agentsdmd 这个文件，然后用蓝崖遥控器测试。",
            rawOutput: "删除：嗯，\nagentsdmd→AGENTS.md\n蓝崖→蓝牙",
            hotwords: hotwords),
        "帮我编辑 AGENTS.md 这个文件，然后用蓝牙遥控器测试。",
        "LLM.guard.basic")

    // 替换目标不在热词表 → 跳过（宁可不改）。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "把skd装进die里面。",
            rawOutput: "skd→SDK\ndie→IDE",   // SDK/IDE 不在热词
            hotwords: hotwords),
        "把skd装进die里面。",
        "LLM.guard.targetNotHotword")

    // 错词不存在于原文 → 跳过。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "把ASR的音频封装成OG格式再发送。",
            rawOutput: "AA→Ogg",
            hotwords: hotwords),
        "把ASR的音频封装成OG格式再发送。",
        "LLM.guard.sourceMissing")

    // 大小写不敏感定位（OG→Ogg 命中原文 Og）。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "把ASR的音频封装成Og格式再发送。",
            rawOutput: "OG→Ogg",
            hotwords: hotwords),
        "把ASR的音频封装成Ogg格式再发送。",
        "LLM.guard.caseInsensitive")

    // 删除片段不存在 → 跳过；「无」/噪声行忽略。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "帮我打开浏览器。",
            rawOutput: "无\n删除：不存在的东西\n输入：嗯",
            hotwords: hotwords),
        "帮我打开浏览器。",
        "LLM.guard.noiseIgnored")

    // 指令上限 5 条：6 条合法删除指令只执行前 5。
    checkEqual(
        LocalEditformRefiner.applyEditInstructions(
            text: "啊1啊2啊3啊4啊5啊6",
            rawOutput: "删除：啊\n删除：啊\n删除：啊\n删除：啊\n删除：啊\n删除：啊",
            hotwords: hotwords),
        "12345啊6",
        "LLM.guard.cap")

    // cleanOutput：think 块剥除 + 包引号剥除。
    checkEqual(
        LocalEditformRefiner.cleanOutput("<think>推理</think>\n「蓝牙」"),
        "蓝牙", "LLM.cleanOutput.thinkAndQuotes")
    checkEqual(LocalEditformRefiner.cleanOutput("  无  "), "无", "LLM.cleanOutput.trim")

    // editform system prompt：few-shot 骨架 + 动态热词表（评测 VARIANTS 同式）。
    let prompt = LocalEditformRefiner.editformSystemPrompt(hotwords: ["ASR"])
    check(prompt.contains("AGENTS.md"), "LLM.prompt.fewshotKept")
    check(prompt.hasSuffix("热词表：ASR"), "LLM.prompt.hotwordSuffix")
}

// MARK: - 档位解析

private func testLocalLlmTierResolution() {
    let dir = FileManager.default.temporaryDirectory
        .appendingPathComponent("llm-tier-\(UUID().uuidString)")
    try? FileManager.default.createDirectory(
        at: dir.appendingPathComponent("Qwen2.5-3B-Instruct-Q4_K_M"),
        withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let gguf = dir.appendingPathComponent(
        "Qwen2.5-3B-Instruct-Q4_K_M/Qwen2.5-3B-Instruct-Q4_K_M.gguf")
    try? Data([0x01]).write(to: gguf)

    // 默认档：存在 → 返回；同级 1.5B 不存在 → nil。
    checkNotNil(
        LocalLlmRuntime.refineModelPath(
            modelsDir: dir.path, override: "", configDirectory: dir),
        "LLM.tier.refineDefault")
    checkNil(
        LocalLlmRuntime.alignModelPath(
            modelsDir: dir.path, override: "", configDirectory: dir),
        "LLM.tier.alignAbsent")

    // 覆盖：绝对路径存在 → 返回；不存在 → nil。
    checkNotNil(
        LocalLlmRuntime.alignModelPath(
            modelsDir: dir.path, override: gguf.path, configDirectory: dir),
        "LLM.tier.overrideAbsolute")
    checkNil(
        LocalLlmRuntime.alignModelPath(
            modelsDir: dir.path, override: "/definitely/absent.gguf",
            configDirectory: dir),
        "LLM.tier.overrideMissing")
}

// MARK: - GBNF 枚举语法

private func testAnswerEnumGrammar() {
    let grammar = VoiceTargetedAlign.answerEnumGrammar(candidates: ["北京", "升旗"])
    check(grammar.hasPrefix("root ::= object"), "LLM.grammar.root")
    check(grammar.contains("\"\\\"北京\\\"\" | \"\\\"升旗\\\"\" | \"\\\"NONE\\\"\""),
          "LLM.grammar.enumMembers")
    check(grammar.contains("member ::="), "LLM.grammar.member")
    check(grammar.contains("answer ::="), "LLM.grammar.answer")
}

// MARK: - 引擎真模型冒烟（不伪造结果：dylib/模型未就位时 SKIP）

private func testLlamaEngineSmokeIfAvailable() {
    guard LlamaCppEngine.runtimeDylibAvailable() else {
        print("LLM.engineSmoke: SKIP（libvoicestick-llama.dylib 未安装）")
        return
    }
    let configDir = FileManager.default.urls(
        for: .applicationSupportDirectory, in: .userDomainMask).first!
        .appendingPathComponent("VoiceStick")
    let settings = LocalAsrSettings()
    guard let modelPath = LocalLlmRuntime.refineModelPath(
        modelsDir: settings.modelsDir, override: "", configDirectory: configDir) else {
        print("LLM.engineSmoke: SKIP（3B 精修档 GGUF 未下载）")
        return
    }
    guard let engine = try? LlamaCppEngine(modelPath: modelPath, numThreads: 4) else {
        check(false, "LLM.engineSmoke.init（模型就位但引擎创建失败）")
        return
    }
    do {
        let reply = try engine.chat(
            systemPrompt: "你是回声助手，原样复述用户的话。",
            userText: "你好")
        check(!reply.isEmpty, "LLM.engineSmoke.reply（实际「\(reply)」）")
    } catch {
        check(false, "LLM.engineSmoke.chat（\(error.localizedDescription)）")
    }
}
