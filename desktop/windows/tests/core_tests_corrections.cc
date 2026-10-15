// 用户数据飞轮 P0-1：纠错对资产化批（correction_pairs.{h,cc}，双端同向量——
// macOS Tests/VoiceStickTests/CorrectionPairsTests.swift 钉同一组期望值）。
#include "test_suites.h"

#include "correction_pairs.h"
#include "llm_refinement_client.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using voicestick::CorrectionPair;
using voicestick::ExtractReplacementPairs;
using voicestick::kCorrectionPairStoreCap;
using voicestick::LLMRefinementClient;
using voicestick::LoadCorrectionPairs;
using voicestick::RecordCorrectionPairs;
using voicestick::RecordCorrectionPairsToDisk;
using voicestick::SaveCorrectionPairs;
using voicestick::TrimCorrectionPairsForPrompt;
using Pairs = std::vector<std::pair<std::string, std::string>>;

namespace {

// few-shot 段头（与 macOS CorrectionPairs.promptSection 逐字一致）。
const char* kFewshotHeader =
    "\n\n用户历史纠正（输出时请遵循这些偏好：仅当识别文本出现与纠正前写法相同或同音、"
    "近音的错误时替换为纠正后写法，其余内容不得改动）：";

} // namespace

void TestExtractReplacementPairs() {
    // 基准用例：同音错词「鱼器渍→语气词」。
    assert((ExtractReplacementPairs("这个鱼器渍是什么", "这个语气词是什么") ==
            Pairs{{"鱼器渍", "语气词"}}));
    // 删除/插入（单侧为空）不入表。
    assert(ExtractReplacementPairs("我觉得嗯这个方案可以", "我觉得这个方案可以").empty());
    // 完全一致无对。
    assert(ExtractReplacementPairs("你好世界", "你好世界").empty());
    // 拉丁词纠正（整词无公共子序列干扰，对形态完整）。
    assert((ExtractReplacementPairs("我用克劳德写代码", "我用 Claude 写代码") ==
            Pairs{{"克劳德", "Claude"}}));
    // 纯标点/空白调整不入表（无内容字符锚点）。
    assert(ExtractReplacementPairs("好的，谢谢", "好的、谢谢").empty());
    assert(ExtractReplacementPairs("中文  之间空格", "中文之间空格").empty());
    // 多对同时提取，保序去重。
    assert((ExtractReplacementPairs("用甲和乙测试", "用 A 和 B 测试") ==
            Pairs{{"甲", "A"}, {"乙", "B"}}));
    // 单侧超过 kCorrectionPairMaxSideChars(24) 码点：整句改写量级不入表。
    const std::string long_a(25, static_cast<char>('x'));
    assert(ExtractReplacementPairs("前缀" + long_a + "后缀", "前缀Y后缀").empty());
    // 任一侧超过 512 码点（整句改写量级护栏）返回空。
    const std::string huge(600, static_cast<char>('a'));
    assert(ExtractReplacementPairs(huge, huge + "b").empty());
}

void TestCorrectionPairStoreMergeAndCap() {
    std::vector<CorrectionPair> store;
    RecordCorrectionPairs(store, {{"鱼器渍", "语气词"}}, 1000);
    RecordCorrectionPairs(store, {{"鱼器渍", "语气词"}, {"克劳德", "Claude"}}, 2000);
    assert(store.size() == 2);
    assert(store[0].before == "鱼器渍" && store[0].count == 2);
    assert(store[0].last_seen_ts == 2000);
    assert(store[1].count == 1 && store[1].last_seen_ts == 2000);

    // 容量硬上限：超限按 count 升序、last_seen 升序逐出，保留 1000 条。
    std::vector<CorrectionPair> big;
    Pairs flood;
    for (int i = 0; i < static_cast<int>(kCorrectionPairStoreCap) + 5; ++i) {
        flood.emplace_back("词" + std::to_string(i), "word" + std::to_string(i));
    }
    RecordCorrectionPairs(big, flood, 3000);
    assert(big.size() == kCorrectionPairStoreCap);
    // 计数 1 的新对按字典序先逐出：词0~词4 应被挤出，最后一个必在。
    bool has_first = false;
    bool has_last = false;
    for (const auto& entry : big) {
        if (entry.before == "词0") has_first = true;
        if (entry.before == "词" + std::to_string(kCorrectionPairStoreCap + 4)) has_last = true;
    }
    assert(!has_first);
    assert(has_last);
}

void TestCorrectionPairsJsonRoundtrip() {
    const auto temp = fs::temp_directory_path() / "voicestick_correction_pairs_test.json";
    std::error_code ec;
    fs::remove(temp, ec);
    std::vector<CorrectionPair> store;
    CorrectionPair pair;
    pair.before = "鱼器渍";
    pair.after = "语气词";
    pair.count = 3;
    pair.last_seen_ts = 1760000000;
    store.push_back(pair);
    SaveCorrectionPairs(temp, store);
    const auto loaded = LoadCorrectionPairs(temp);
    assert(loaded.size() == 1);
    assert(loaded[0].before == "鱼器渍" && loaded[0].after == "语气词");
    assert(loaded[0].count == 3 && loaded[0].last_seen_ts == 1760000000);
    assert(loaded[0].source == "refine" && loaded[0].app.empty());
    fs::remove(temp, ec);

    // 缺失/损坏文件返回空 store。
    assert(LoadCorrectionPairs(temp).empty());
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        f << "{broken json";
    }
    assert(LoadCorrectionPairs(temp).empty());
    fs::remove(temp, ec);
}

void TestCorrectionPairsSingleWriter() {
    // B13 模式：唯一写入口 reload-merge-save，跨调用累计不覆盖。
    const auto dir = fs::temp_directory_path() / "voicestick_correction_pairs_rw";
    fs::create_directories(dir);
    const auto path = dir / "correction_pairs.json";
    std::error_code ec;
    fs::remove(path, ec);

    RecordCorrectionPairsToDisk(path, {{"鱼器渍", "语气词"}}, 1000);
    RecordCorrectionPairsToDisk(path, {{"鱼器渍", "语气词"}}, 2000);
    auto store = LoadCorrectionPairs(path);
    assert(store.size() == 1 && store[0].count == 2 && store[0].last_seen_ts == 2000);

    // 空提取不触碰文件（不产生空 JSON）。
    RecordCorrectionPairsToDisk(path, {}, 3000);
    assert(fs::exists(path));

    // 路径单一出处：由配置路径推导，文件名收敛一处。
    const auto derived = voicestick::CorrectionPairsPath(dir / "config.toml");
    assert(derived.parent_path() == dir);
    assert(derived.filename() == "correction_pairs.json");

    fs::remove_all(dir, ec);
}

void TestTrimCorrectionPairsForPrompt() {
    std::vector<CorrectionPair> store;
    auto add = [&store](const char* before, const char* after, int count, std::int64_t ts) {
        CorrectionPair pair;
        pair.before = before;
        pair.after = after;
        pair.count = count;
        pair.last_seen_ts = ts;
        store.push_back(pair);
    };
    add("低频", "low", 1, 100);
    add("高频旧", "old", 5, 100);
    add("高频新", "new", 5, 900);
    add("零次", "zero", 0, 999);

    // count 降序 → last_seen 降序 → 字典序；count<=0 剔除。
    const auto trimmed = TrimCorrectionPairsForPrompt(store, 8);
    assert(trimmed.size() == 3);
    assert(trimmed[0].before == "高频新");
    assert(trimmed[1].before == "高频旧");
    assert(trimmed[2].before == "低频");

    // max_pairs 截断取头部；非正值返回空。
    assert(TrimCorrectionPairsForPrompt(store, 2).size() == 2);
    assert(TrimCorrectionPairsForPrompt(store, 0).empty());
}

void TestCorrectionFewshotSection() {
    assert(voicestick::BuildCorrectionFewshotSection({}).empty());

    std::vector<CorrectionPair> pairs;
    CorrectionPair a;
    a.before = "鱼器渍";
    a.after = "语气词";
    CorrectionPair b;
    b.before = "克劳德";
    b.after = "Claude";
    pairs.push_back(a);
    pairs.push_back(b);
    const std::string expected =
        std::string(kFewshotHeader) +
        "\n- \"鱼器渍\" → \"语气词\""
        "\n- \"克劳德\" → \"Claude\"";
    assert(voicestick::BuildCorrectionFewshotSection(pairs) == expected);

    // 含引号/换行的对被格式安全过滤；全部无效返回空串。
    CorrectionPair quote;
    quote.before = "带\"引号";
    quote.after = "ok";
    CorrectionPair newline;
    newline.before = "带\n换行";
    newline.after = "ok";
    assert(voicestick::BuildCorrectionFewshotSection({quote, newline}).empty());
    assert(voicestick::BuildCorrectionFewshotSection({a, quote}) == expected.substr(0, expected.find("\n- \"克劳德")));
}

void TestBuildRefinePromptWithCorrections() {
    // 无纠错对：prompt 与既有行为逐字一致。
    const std::string base = LLMRefinementClient::BuildRefinePrompt("自定义提示词", {"Claude"});
    assert(LLMRefinementClient::BuildRefinePrompt("自定义提示词", {"Claude"}, {}) == base);

    std::vector<CorrectionPair> pairs;
    CorrectionPair pair;
    pair.before = "鱼器渍";
    pair.after = "语气词";
    pair.count = 2;
    pairs.push_back(pair);
    const std::string with_corrections = LLMRefinementClient::BuildRefinePrompt("自定义提示词", {"Claude"}, pairs);
    // few-shot 段追加在热词段之后（热词段收尾句仍在其中）。
    assert(with_corrections.find("用户常用术语热词表：Claude") != std::string::npos);
    assert(with_corrections.find(std::string(kFewshotHeader)) != std::string::npos);
    assert(with_corrections.find("\n- \"鱼器渍\" → \"语气词\"") != std::string::npos);
    assert(with_corrections.find(kFewshotHeader) > with_corrections.find("用户常用术语热词表"));
    // 无热词时 few-shot 段独立追加。
    const std::string corrections_only = LLMRefinementClient::BuildRefinePrompt("自定义提示词", {}, pairs);
    assert(corrections_only.find("用户常用术语热词表") == std::string::npos);
    assert(corrections_only.find(std::string(kFewshotHeader)) != std::string::npos);
}

void RunCorrectionPairsBatchTests() {
    TestExtractReplacementPairs();
    TestCorrectionPairStoreMergeAndCap();
    TestCorrectionPairsJsonRoundtrip();
    TestCorrectionPairsSingleWriter();
    TestTrimCorrectionPairsForPrompt();
    TestCorrectionFewshotSection();
    TestBuildRefinePromptWithCorrections();
}
