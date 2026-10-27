// 语音编辑指令（Doc/Plan/voice-edit-commands.md）：纯逻辑解析器/删除计划单测。
// 与 macOS Tests/VoiceStickTests/VoiceEditCommandTests.swift 保持同一张向量表
//（延续 correction_pairs 双端同向量约定）；纯逻辑零 Win32 依赖，本机 macOS 已用
// clang++ -std=c++20 链接临时 main 跑通同一组向量。
#include "voice_edit_command.h"
#ifdef _WIN32
#include "app_config.h"
#include "focus_text_reader.h"
#include "test_support.h"  // 协调器 Fake（FakeBleCentral/FakeAsrClient/FakeUi/FakeInputInjector…）

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#endif

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace voicestick;

namespace {

void ExpectParse(std::string_view text, VoiceEditAction action, int count,
                 VoiceEditCommandSource source, int default_chars = 1,
                 int max_chars = 20) {
    const auto parsed = ParseVoiceEditCommand(text, default_chars, max_chars);
    if (!parsed) {
        std::printf("FAIL voice edit parse (unexpected nullopt): %s\n",
                    std::string(text).c_str());
        assert(false && "expected voice edit command");
    }
    if (parsed->action != action || parsed->count != count || parsed->source != source) {
        std::printf("FAIL voice edit parse: %s action=%d count=%d source=%d\n",
                    std::string(text).c_str(), static_cast<int>(parsed->action),
                    parsed->count, static_cast<int>(parsed->source));
        assert(false && "voice edit parse mismatch");
    }
}

void ExpectNoParse(std::string_view text, int default_chars = 1, int max_chars = 20) {
    if (ParseVoiceEditCommand(text, default_chars, max_chars)) {
        std::printf("FAIL voice edit parse (unexpected command): %s\n",
                    std::string(text).c_str());
        assert(false && "expected no voice edit command");
    }
}

VoiceEditCommand Chars(int n) {
    return VoiceEditCommand{VoiceEditAction::kDeleteLastChars, n,
                            VoiceEditCommandSource::kExplicitCount};
}

VoiceEditCommand Sentence(int n) {
    return VoiceEditCommand{VoiceEditAction::kDeleteLastSentences, n,
                            VoiceEditCommandSource::kExplicitCount};
}

VoiceEditCommand All() {
    return VoiceEditCommand{VoiceEditAction::kDeleteAll, 0, VoiceEditCommandSource::kAll};
}

VoiceEditFieldState VerifiedField(const std::string& tail, bool caret_at_end = true,
                                  bool collapsed = true) {
    VoiceEditFieldState field;
    field.readable = true;
    field.caret_at_end = caret_at_end;
    field.selection_collapsed = collapsed;
    field.tail_text = tail;
    return field;
}

void ExpectPlan(const std::string& remaining, bool pressed_enter,
                const VoiceEditCommand& command, const VoiceEditFieldState& field,
                VoiceEditVerifyMode mode, bool unverified_allowed, int expect_count,
                int expect_graphemes, bool expect_newline,
                const std::string& expect_remaining,
                bool expect_verified = true, bool expect_pressed_enter = false) {
    const auto plan = PlanVoiceEdit(remaining, pressed_enter, command, field, mode,
                                    unverified_allowed);
    if (!plan.ok) {
        std::printf("FAIL voice edit plan rejected: %s reason=%s\n", remaining.c_str(),
                    std::string(VoiceEditRejectReasonName(plan.reject_reason)).c_str());
        assert(false && "expected voice edit plan ok");
    }
    if (plan.backspace_count != expect_count ||
        plan.delete_graphemes != expect_graphemes ||
        plan.delete_newline != expect_newline ||
        plan.new_remaining_text != expect_remaining) {
        std::printf(
            "FAIL voice edit plan: remaining=%s got count=%d glyph=%d nl=%d rem=[%s] "
            "want count=%d glyph=%d nl=%d rem=[%s]\n",
            remaining.c_str(), plan.backspace_count, plan.delete_graphemes,
            plan.delete_newline ? 1 : 0, plan.new_remaining_text.c_str(), expect_count,
            expect_graphemes, expect_newline ? 1 : 0, expect_remaining.c_str());
        assert(false && "voice edit plan mismatch");
    }
    assert(plan.verified == expect_verified);
    assert(plan.new_pressed_enter == expect_pressed_enter);
}

void ExpectReject(const std::string& remaining, bool pressed_enter,
                  const VoiceEditCommand& command, const VoiceEditFieldState& field,
                  VoiceEditVerifyMode mode, bool unverified_allowed,
                  VoiceEditRejectReason reason) {
    const auto plan = PlanVoiceEdit(remaining, pressed_enter, command, field, mode,
                                    unverified_allowed);
    if (plan.ok || plan.reject_reason != reason) {
        std::printf("FAIL voice edit reject: %s got ok=%d reason=%s want=%s\n",
                    remaining.c_str(), plan.ok ? 1 : 0,
                    std::string(VoiceEditRejectReasonName(plan.reject_reason)).c_str(),
                    std::string(VoiceEditRejectReasonName(reason)).c_str());
        assert(false && "voice edit reject mismatch");
    }
}

void TestVoiceEditParserPositive() {
    // 与 macOS Tests/VoiceStickTests/VoiceEditCommandTests.swift 同一张正例表
    //（双端同向量；任一侧增删必须同步）。

    // 单独「删除/删掉/去掉」→ 默认 1 字（决策点 D1）。
    ExpectParse("删除", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("删掉", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("去掉", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("删掉一下", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("删除一下", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("请删除", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);

    // 删最后 N 个字（中文数字/阿拉伯数字/两/俩、字/字符/汉字/文字）。
    ExpectParse("删掉最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删掉最后 2 个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("去掉最后两个字符", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后两个汉字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后两个文字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除十二个字", VoiceEditAction::kDeleteLastChars, 12,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除二十个字", VoiceEditAction::kDeleteLastChars, 20,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后俩字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 把字句 / 后置动词 / 常见句式（D6）。
    ExpectParse("最后两个字删掉", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("把最后两个字删掉", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("把最后两个字删除", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 删最后 N 句（含「这句话」「一句话」等变体）。
    ExpectParse("删掉最后一句", VoiceEditAction::kDeleteLastSentences, 1,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后这句话", VoiceEditAction::kDeleteLastSentences, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("删掉最后两句话", VoiceEditAction::kDeleteLastSentences, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除两句话", VoiceEditAction::kDeleteLastSentences, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("把最后一句删掉", VoiceEditAction::kDeleteLastSentences, 1,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("把最后两句话删掉", VoiceEditAction::kDeleteLastSentences, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 整段删除（撤销/撤回 = 本次输入整段，决策点 D1）。
    ExpectParse("删除刚才的", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);
    ExpectParse("删掉全部", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);
    ExpectParse("全部删掉", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);
    ExpectParse("删除本次输入", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);
    ExpectParse("清空", VoiceEditAction::kDeleteAll, 0, VoiceEditCommandSource::kAll);
    ExpectParse("撤销", VoiceEditAction::kDeleteAll, 0, VoiceEditCommandSource::kAll);
    ExpectParse("撤回", VoiceEditAction::kDeleteAll, 0, VoiceEditCommandSource::kAll);
    ExpectParse("都删掉", VoiceEditAction::kDeleteAll, 0, VoiceEditCommandSource::kAll);
    ExpectParse("删除刚才说的", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);
    ExpectParse("把刚才的删掉", VoiceEditAction::kDeleteAll, 0,
                VoiceEditCommandSource::kAll);

    // 归一化：首尾空白/尾部标点/全角数字/全角空格。
    ExpectParse("删除最后两个字。", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后两个字。！？", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("\u3000删除最后两个字\u3000", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("  删除最后两个字。 ", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后\uFF12个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 位置词变体。
    ExpectParse("删掉末尾两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除结尾2个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("去掉后面两个字符", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 礼貌前缀 / 语气尾。
    ExpectParse("请删除最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("帮我删掉最后一句", VoiceEditAction::kDeleteLastSentences, 1,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("麻烦删除", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount);
    ExpectParse("请帮我删除最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后两个字吧", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删掉最后两个字啊", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删除最后一句呀", VoiceEditAction::kDeleteLastSentences, 1,
                VoiceEditCommandSource::kExplicitCount);

    // 其它白名单动词（带数量时；“取消/清除/移除”只作带宾语动词，单独出现不识别）。
    ExpectParse("移除最后两个字符", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("清除最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删去最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("取消最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);
    ExpectParse("删两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount);

    // 配置参数：default_chars / max_chars。
    ExpectParse("删除", VoiceEditAction::kDeleteLastChars, 3,
                VoiceEditCommandSource::kDefaultCount, /*default_chars=*/3);
    ExpectParse("删除", VoiceEditAction::kDeleteLastChars, 1,
                VoiceEditCommandSource::kDefaultCount, /*default_chars=*/0);
    ExpectParse("删除", VoiceEditAction::kDeleteLastChars, 20,
                VoiceEditCommandSource::kDefaultCount, /*default_chars=*/50,
                /*max_chars=*/20);
    ExpectParse("删除最后两个字", VoiceEditAction::kDeleteLastChars, 2,
                VoiceEditCommandSource::kExplicitCount, /*default_chars=*/5);
    ExpectParse("删除三个字", VoiceEditAction::kDeleteLastChars, 3,
                VoiceEditCommandSource::kExplicitCount, /*default_chars=*/1,
                /*max_chars=*/3);
}

// 与 macOS deleteAllForms 39 条逐条对拍（Lead 独立 parity 用例）。
void TestVoiceEditParserDeleteAllForms() {
    const std::vector<std::string> mac_forms = {
        "删除刚才的", "删除刚才", "删掉刚才的", "删掉刚才", "删除刚才的内容",
        "删掉刚才的内容", "删除刚才说的", "删掉刚才说的", "删除刚才输入",
        "删除刚才输入的", "删掉刚才输入的", "删除刚输入的", "删掉刚输入的",
        "删除上次输入", "删掉上次输入", "删除本次输入", "删掉本次输入",
        "删除这次输入", "删掉这次输入", "删除全部", "删掉全部", "全部删除",
        "全部删掉", "都删掉", "都删除", "删除所有", "删掉所有", "清空",
        "清空刚才的", "撤销", "撤回", "撤销刚才的", "撤回刚才的",
        "把刚才的删掉", "把刚才删掉", "把刚才的删除", "把刚才输入的删掉",
        "把本次输入删掉", "把这次输入删掉",
    };
    assert(mac_forms.size() == 39);
    for (const auto& form : mac_forms) {
        const auto parsed = ParseVoiceEditCommand(form);
        if (!parsed || parsed->action != VoiceEditAction::kDeleteAll ||
            parsed->source != VoiceEditCommandSource::kAll) {
            std::printf("FAIL deleteAllForms parity: %s\n", form.c_str());
            assert(false && "macOS deleteAllForms parity");
        }
    }
}

void TestVoiceEditParserNegative() {
    // 与 macOS Tests/VoiceStickTests/VoiceEditCommandTests.swift 同一张负例表。

    // 混合正文 / 包含式 / 否定 / 问句：一律按普通文本上屏。
    ExpectNoParse("请把删除按钮改成红色");
    ExpectNoParse("你好世界删除最后两个字");
    ExpectNoParse("把文件删除");
    ExpectNoParse("把所有内容删除");
    ExpectNoParse("别删除");
    ExpectNoParse("不要删除最后两个字");
    ExpectNoParse("要不要删除");
    ExpectNoParse("删不删");
    ExpectNoParse("删除功能");
    ExpectNoParse("清空输入框");
    ExpectNoParse("撤销删除");
    ExpectNoParse("删掉这个文件");
    ExpectNoParse("删除最后两个字然后粘贴");
    ExpectNoParse("最后两个字删掉了吗");
    ExpectNoParse("删掉最后两个字符呢");

    // 不支持的位置词/单位/动作。
    ExpectNoParse("删除前面两个字");
    ExpectNoParse("删除开头两个字");
    ExpectNoParse("删除第一个字");
    ExpectNoParse("删除最后一段");
    ExpectNoParse("删除最后一行");
    ExpectNoParse("删除最后一个词");
    ExpectNoParse("删除最后两个");

    // 数量越界/非法。
    ExpectNoParse("删除 100 个字");
    ExpectNoParse("删除二十一个字");
    ExpectNoParse("删除零个字");
    ExpectNoParse("删除五个字", /*default_chars=*/1, /*max_chars=*/3);
    ExpectNoParse("删除 2 个文件");

    // 空/纯标点/单独「删」/单独「移除」「清除」「取消」。
    ExpectNoParse("");
    ExpectNoParse("   ");
    ExpectNoParse("。。。");
    ExpectNoParse("删");
    ExpectNoParse("移除");
    ExpectNoParse("清除");
    ExpectNoParse("取消");
    ExpectNoParse("取消一下");
    ExpectNoParse("删了");
    ExpectNoParse("删去");

    // 长度上限（kVoiceEditMaxCommandChars=24）：剥掉语气尾后本可命中，超长即不识别。
    std::string long_tail = "删除最后两个字";
    for (int i = 0; i < 20; ++i) long_tail += "吧";
    ExpectNoParse(long_tail);
}

void TestVoiceEditGraphemes() {
    assert(GraphemeCount("你好世界") == 4);
    assert(GraphemeCount("") == 0);
    assert(GraphemeCount("ab") == 2);
    assert(GraphemeCount("😀") == 1);
    assert(GraphemeCount("👨‍👩‍👧") == 1);   // ZWJ 家庭
    assert(GraphemeCount("👍🏽") == 1);     // emoji + 肤色修饰符
    assert(GraphemeCount("🇨🇳") == 1);     // 国旗（区域指示符对）
    assert(GraphemeCount("e\u0301") == 1);  // e + 组合重音
    assert(IsValidUtf8("你好😀"));
    assert(!IsValidUtf8(std::string("\xF0\x28\x8C\x28")));
    assert(DropLastGraphemes("你好世界", 0) == "你好世界");
    assert(DropLastGraphemes("你好世界", 2) == "你好");
    assert(DropLastGraphemes("你好世界", 4).empty());
    assert(DropLastGraphemes("你好世界", 99).empty());
    assert(DropLastGraphemes("a😀b", 1) == "a😀");
    assert(DropLastGraphemes("👨‍👩‍👧x", 1) == "👨‍👩‍👧");
    assert(NormalizeVoiceEditLineBreaks("a\r\nb\rc\n") == "a\nb\nc\n");
    const auto sentences = VoiceEditSentenceGraphemes("你好。世界！再见");
    assert((sentences == std::vector<std::size_t>{3, 3, 2}));
    assert(VoiceEditSentenceGraphemes("没有标点").size() == 1);
    assert(VoiceEditSentenceGraphemes("多标点！？好").size() == 2);
    assert(VoiceEditSentenceGraphemes("换行\n下一句").size() == 2);
}

void TestVoiceEditPlannerVerified() {
    // Lead 真机 spike 确认的基准向量：remaining="你好世界"、pressedEnter=true、
    // 字段尾 "你好世界"+LF、光标在文末 → 1 换行 + 2 字 = 3 次退格。
    ExpectPlan("你好世界", true, Chars(2), VerifiedField("你好世界\n"),
               VoiceEditVerifyMode::kAuto, false, /*count=*/3, /*graphemes=*/2,
               /*newline=*/true, "你好");

    // 字段尾被用户手动删过：字段 "你好世"+LF → eff=3、count=3 → 剩余 "你"。
    ExpectPlan("你好世界", true, Chars(2), VerifiedField("你好世\n"),
               VoiceEditVerifyMode::kAuto, false, 3, 2, true, "你");

    // 无 auto_enter：字段 = 注入文本、光标在文末 → count=2。
    ExpectPlan("你好世界", false, Chars(2), VerifiedField("你好世界"),
               VoiceEditVerifyMode::kAuto, false, 2, 2, false, "你好");

    // 单说“删除”（默认 1 字）+ auto_enter → count=2。
    const auto bare_delete = ParseVoiceEditCommand("删除");
    assert(bare_delete.has_value());
    ExpectPlan("你好世界", true, *bare_delete, VerifiedField("你好世界\n"),
               VoiceEditVerifyMode::kAuto, false, 2, 1, true, "你好世");

    // 句子删除：整句（含句末标点）。
    ExpectPlan("你好。世界！", false, Sentence(1), VerifiedField("你好。世界！"),
               VoiceEditVerifyMode::kAuto, false, 3, 3, false, "你好。");
    ExpectPlan("你好。世界！再见", false, Sentence(2),
               VerifiedField("你好。世界！再见"), VoiceEditVerifyMode::kAuto, false, 5, 5,
               false, "你好。");

    // 整段删除（含 auto_enter 换行）。
    ExpectPlan("你好世界", true, All(), VerifiedField("你好世界\n"),
               VoiceEditVerifyMode::kAuto, false, 5, 4, true, "");

    // N 超过剩余 → clamp。
    ExpectPlan("你好世界", false, Chars(10), VerifiedField("你好世界"),
               VoiceEditVerifyMode::kAuto, false, 4, 4, false, "");

    // emoji 按 1 个 grapheme 计（自动回车：删 1 emoji + 换行 = 2 次）。
    ExpectPlan("你好😀", true, Chars(1), VerifiedField("你好😀\n"),
               VoiceEditVerifyMode::kAuto, false, 2, 1, true, "你好");

    // 字段前还有用户既有内容：只要求字段尾部匹配注入文本。
    ExpectPlan("你好世界", false, Chars(2), VerifiedField("前面的话你好世界"),
               VoiceEditVerifyMode::kAuto, false, 2, 2, false, "你好");

    // 字段尾被追加手打内容 → 拒绝。
    ExpectReject("你好世界", true, Chars(2), VerifiedField("你好世界\n吗"),
                 VoiceEditVerifyMode::kAuto, false, VoiceEditRejectReason::kTextMismatch);
    // 字段文本已不在输入框（已发送/切换应用）→ 拒绝。
    ExpectReject("你好世界", false, Chars(2), VerifiedField(""),
                 VoiceEditVerifyMode::kAuto, false, VoiceEditRejectReason::kTextMismatch);
    // 光标不在文末 → 拒绝。
    ExpectReject("你好世界", false, Chars(2), VerifiedField("你好世界", false),
                 VoiceEditVerifyMode::kAuto, false,
                 VoiceEditRejectReason::kCaretNotAtEnd);
    // 选区非空 → 拒绝。
    ExpectReject("你好世界", false, Chars(2), VerifiedField("你好世界", true, false),
                 VoiceEditVerifyMode::kAuto, false,
                 VoiceEditRejectReason::kSelectionActive);
    // 密码框 → 拒绝。
    {
        auto field = VerifiedField("你好世界");
        field.is_password = true;
        ExpectReject("你好世界", false, Chars(2), field, VoiceEditVerifyMode::kAuto, false,
                     VoiceEditRejectReason::kPasswordField);
    }
    // 空记录 → 拒绝。
    ExpectReject("", false, Chars(2), VerifiedField(""),
                 VoiceEditVerifyMode::kAuto, false, VoiceEditRejectReason::kEmptyTarget);
}

void TestVoiceEditPlannerUnverified() {
    VoiceEditFieldState unreadable;
    // strict：不可读一律拒绝。
    ExpectReject("你好世界", false, Chars(2), unreadable, VoiceEditVerifyMode::kStrict,
                 true, VoiceEditRejectReason::kFieldUnreadable);
    // auto：unverified 窗口内允许不可验证分支（verified=false）；窗口外拒绝。
    ExpectPlan("你好世界", false, Chars(2), unreadable, VoiceEditVerifyMode::kAuto, true, 2,
               2, false, "你好", /*expect_verified=*/false);
    ExpectReject("你好世界", false, Chars(2), unreadable, VoiceEditVerifyMode::kAuto, false,
                 VoiceEditRejectReason::kUnverifiedNotAllowed);
    // auto 不可读 + auto_enter：多发 1 次退格清换行（Doc §6.5）。
    ExpectPlan("你好世界", true, Chars(2), unreadable, VoiceEditVerifyMode::kAuto, true, 3,
               2, true, "你好", /*expect_verified=*/false);
    // off：不校验，按不可验证分支执行。
    ExpectPlan("你好世界", true, All(), unreadable, VoiceEditVerifyMode::kOff, false, 5, 4,
               true, "", /*expect_verified=*/false);
    // 不可验证但用户已手动删光：仍按记录删（短时间窗风险，Doc §6.5）。
    ExpectPlan("你好世界", false, Chars(6), unreadable, VoiceEditVerifyMode::kAuto, true, 4,
               4, false, "", /*expect_verified=*/false);
}

void TestVoiceEditVerifyModeNames() {
    assert(VoiceEditVerifyModeFromName("strict", VoiceEditVerifyMode::kAuto) ==
           VoiceEditVerifyMode::kStrict);
    assert(VoiceEditVerifyModeFromName("auto", VoiceEditVerifyMode::kStrict) ==
           VoiceEditVerifyMode::kAuto);
    assert(VoiceEditVerifyModeFromName("off", VoiceEditVerifyMode::kAuto) ==
           VoiceEditVerifyMode::kOff);
    assert(VoiceEditVerifyModeFromName("bogus", VoiceEditVerifyMode::kAuto) ==
           VoiceEditVerifyMode::kAuto);
    assert(VoiceEditVerifyModeName(VoiceEditVerifyMode::kStrict) == "strict");
    assert(VoiceEditVerifyModeName(VoiceEditVerifyMode::kAuto) == "auto");
    assert(VoiceEditVerifyModeName(VoiceEditVerifyMode::kOff) == "off");
}


#ifdef _WIN32
// ======================= Windows 集成：配置持久化 + 协调器接线 =======================
// 纯逻辑部分在本机 macOS 可独立编译对拍；Windows-only 部分（配置读写/UIA Fake/
// 协调器推进状态机）用 _WIN32 包裹，避免污染纯逻辑的对拍编译。

void TestVoiceEditConfigDefaultsAndPersistence() {
    // 产品默认值（已确认）：功能默认开启；单独“删除”删 1 字；verify_mode=auto；
    // 不可读仅 8s 内允许。WriteTo 白名单必须含 [voice_edit]（否则重启回默认，
    // 参考 xiaomi_gateway_repeat_interval_ms 只 load 不 save 的既有坑）。
    const AppConfig defaults = AppConfig::Defaults();
    assert(defaults.voice_edit.enabled);
    assert(defaults.voice_edit.default_chars == 1);
    assert(defaults.voice_edit.max_chars == 20);
    assert(defaults.voice_edit.scope_seconds == 120);
    assert(defaults.voice_edit.verify_mode == VoiceEditVerifyMode::kAuto);
    assert(defaults.voice_edit.unverified_seconds == 8);

    const auto temp =
        std::filesystem::temp_directory_path() / "voicestick_voice_edit_config_test.toml";
    std::filesystem::remove(temp);
    {
        std::ofstream out(temp);
        out << "[voice_edit]\n";
        out << "enabled = false\n";
        out << "default_chars = 9\n";
        out << "max_chars = 0\n";              // 越界 → 钳到 1
        out << "scope_seconds = 1\n";          // 越界 → 钳到 10
        out << "verify_mode = \"bogus\"\n";    // 未知 → 回落 auto
        out << "unverified_seconds = 99\n";    // 越界 → 钳到 30
    }
    AppConfig loaded = AppConfig::Load(temp);
    assert(!loaded.voice_edit.enabled);
    assert(loaded.voice_edit.default_chars == 9);
    assert(loaded.voice_edit.max_chars == 1);
    assert(loaded.voice_edit.scope_seconds == 10);
    assert(loaded.voice_edit.verify_mode == VoiceEditVerifyMode::kAuto);
    assert(loaded.voice_edit.unverified_seconds == 30);

    // Save → Load 往返（WriteTo 白名单覆盖全部 6 个字段）。
    AppConfig config = AppConfig::Defaults();
    config.voice_edit.enabled = true;
    config.voice_edit.default_chars = 4;
    config.voice_edit.max_chars = 30;
    config.voice_edit.scope_seconds = 300;
    config.voice_edit.verify_mode = VoiceEditVerifyMode::kStrict;
    config.voice_edit.unverified_seconds = 5;
    config.Save(temp);
    const AppConfig roundtrip = AppConfig::Load(temp);
    assert(roundtrip.voice_edit.enabled);
    assert(roundtrip.voice_edit.default_chars == 4);
    assert(roundtrip.voice_edit.max_chars == 30);
    assert(roundtrip.voice_edit.scope_seconds == 300);
    assert(roundtrip.voice_edit.verify_mode == VoiceEditVerifyMode::kStrict);
    assert(roundtrip.voice_edit.unverified_seconds == 5);
    std::filesystem::remove(temp);
}

class VoiceEditFakeForegroundProbe : public IForegroundProcessProbe {
public:
    bool IsForegroundHigherIntegrity(std::wstring& process_name) override {
        process_name = elevated_process;
        return elevated;
    }
    std::uintptr_t ForegroundWindow() override { return window; }
    std::string ForegroundProcessName() override { return process; }

    std::uintptr_t window = 0x1234;
    std::string process = "notepad.exe";
    bool elevated = false;
    std::wstring elevated_process = L"admin.exe";
};

class VoiceEditFakeFocusReader : public IFocusTextReader {
public:
    bool ReadFocusedField(VoiceEditFieldState* out) override {
        if (out == nullptr) return false;
        *out = state;
        return state.readable;
    }
    VoiceEditFieldState state;
};

// 协调器级集成测试台：Fake BLE/ASR/UI/Injector/ForegroundProbe/FocusReader + 可泵
// 的 UI dispatcher（生产 RunOnUiThread 的等价物，保证状态机/断言都在测试线程上）。
class VoiceEditHarness {
public:
    explicit VoiceEditHarness(const AppConfig& config) : config_(config) {
        auto ble = std::make_unique<FakeBleCentral>();
        ble_ = ble.get();
        auto asr = std::make_unique<FakeAsrClient>();
        asr_ = asr.get();
        auto probe = std::make_unique<VoiceEditFakeForegroundProbe>();
        probe_ = probe.get();
        auto focus = std::make_unique<VoiceEditFakeFocusReader>();
        focus_ = focus.get();
        coordinator_ = std::make_unique<VoiceStickCoordinator>(
            config_, std::move(ble), std::move(asr), &ui_, &input_);
        coordinator_->SetForegroundProbe(std::move(probe));
        coordinator_->SetFocusTextReader(std::move(focus));
        coordinator_->SetUiDispatcher([this](std::function<void()> action) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(action));
        });
        coordinator_->Start();
    }
    ~VoiceEditHarness() {
        if (coordinator_) coordinator_->Shutdown();
    }

    FakeBleCentral& ble() { return *ble_; }
    FakeAsrClient& asr() { return *asr_; }
    FakeUi& ui() { return ui_; }
    FakeInputInjector& input() { return input_; }
    VoiceEditFakeForegroundProbe& probe() { return *probe_; }
    VoiceEditFakeFocusReader& focus() { return *focus_; }
    UiLanguage language() const { return EffectiveUiLanguage(config_.ui_language); }

    bool PumpOnce() {
        std::function<void()> action;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (queue_.empty()) return false;
            action = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        action();
        return true;
    }
    bool PumpUntil(const std::function<bool()>& done, int timeout_ms = 3000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            if (done()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return done();
            if (!PumpOnce()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    // 一轮正常录音：button_down → 首帧 → 过 0.5s 最短门槛 → button_up → END 帧
    //（同步进入 finalizing），随后由调用方触发 ASR final。
    void RecordAndStop(std::uint32_t session_id) {
        ble_->on_state_event("5A74", ButtonEvent("button_down", "primary", session_id));
        ble_->on_audio_frame("5A74", AudioDataFrame(session_id, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        ble_->on_state_event("5A74", ButtonEvent("button_up", "primary", session_id));
        ble_->on_audio_frame("5A74", EmptyEndFrame(session_id, 2));
    }

    // 普通文本会话：泵到粘贴完成。
    bool PasteText(const std::string& text, std::uint32_t session_id) {
        RecordAndStop(session_id);
        asr_->on_final(text);
        return PumpUntil([this, text] { return input_.pasted_text == text; });
    }

    // 指令会话：命令轮不粘贴，泵到出现一条 timed message（成功/拒绝都会有）。
    bool RunCommand(const std::string& command_text, std::uint32_t session_id) {
        const std::size_t toasts_before = ui_.timed_messages.size();
        RecordAndStop(session_id);
        asr_->on_final(command_text);
        return PumpUntil([this, toasts_before] {
            return ui_.timed_messages.size() > toasts_before;
        });
    }

private:
    AppConfig config_;
    FakeUi ui_;
    FakeInputInjector input_;
    VoiceEditFakeForegroundProbe* probe_ = nullptr;
    VoiceEditFakeFocusReader* focus_ = nullptr;
    FakeBleCentral* ble_ = nullptr;
    FakeAsrClient* asr_ = nullptr;
    std::mutex queue_mutex_;
    std::vector<std::function<void()>> queue_;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;  // 最后析构（先于 queue_）
};

std::string WithToastDuration(std::string message) {
    return message + ":3000";
}

// U1：注入“你好世界”+ auto_enter → 说“删掉最后两个字” → 3 次退格（换行+2 字），
// 不调用 Paste、状态回 ready、记录更新（后续“删除”按剩余文本继续删）。
void TestVoiceEditCoordinatorDeletesWithBackspace() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = true;
    VoiceEditHarness harness(config);
    harness.focus().state = VerifiedField("你好世界\n");
    assert(harness.PasteText("你好世界", 21));
    assert(harness.input().pasted_enter);

    assert(harness.RunCommand("删掉最后两个字", 22));
    assert(harness.input().sent_key_combos.size() == 3);
    for (const auto& combo : harness.input().sent_key_combos) {
        assert(combo == "Backspace");
    }
    // 命令轮不得写正文（Paste 只有首轮一次）、状态回 ready。
    assert(harness.input().pasted_text == "你好世界");
    assert(harness.ui().statuses.back() == "Ready");
    assert(HasUiState(harness.ble(), "ready", "5A74"));
    std::string expected_chars = Tr(StringId::kVoiceEditDeletedChars, harness.language());
    const auto marker = expected_chars.find("%d");
    assert(marker != std::string::npos);
    expected_chars.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == WithToastDuration(expected_chars));

    // 记录已更新为 remaining="你好"、pressedEnter=false（换行已消费）：字段同步为
    // “你好”后再说“删除”（默认 1 字）→ 只 1 次退格。
    harness.focus().state = VerifiedField("你好");
    assert(harness.RunCommand("删除", 23));
    assert(harness.input().sent_key_combos.size() == 4);
    assert(harness.input().sent_key_combos.back() == "Backspace");

    // 普通语音输入回归：非指令 final 仍照常粘贴。
    harness.focus().state = VerifiedField("");
    assert(harness.PasteText("今天天气不错", 24));
    assert(harness.input().pasted_text == "今天天气不错");
}

// L0：没有注入记录 → 拒绝且不注入任何按键（命令文本也不得作为正文粘贴）。
void TestVoiceEditCoordinatorRejectsWithoutRecord() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceEditHarness harness(config);
    harness.focus().state = VerifiedField("");
    const std::size_t toasts_before = harness.ui().timed_messages.size();
    harness.RecordAndStop(31);
    harness.asr().on_final("删除");
    assert(harness.PumpUntil([&] {
        return harness.ui().timed_messages.size() > toasts_before;
    }));
    assert(harness.ui().timed_messages.back() ==
           WithToastDuration(Tr(StringId::kVoiceEditNoTarget, harness.language())));
    assert(harness.input().sent_key_combos.empty());
    assert(harness.input().pasted_text.empty());
    assert(HasUiState(harness.ble(), "ready", "5A74"));
}

// L0：注入后前台窗口变化（切应用/文本已提交到别处）→ 拒绝。
void TestVoiceEditCoordinatorRejectsChangedWindow() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceEditHarness harness(config);
    harness.focus().state = VerifiedField("你好世界");
    assert(harness.PasteText("你好世界", 41));
    harness.probe().window = 0x9999;
    const std::size_t toasts_before = harness.ui().timed_messages.size();
    harness.RecordAndStop(42);
    harness.asr().on_final("删除");
    assert(harness.PumpUntil([&] {
        return harness.ui().timed_messages.size() > toasts_before;
    }));
    assert(harness.ui().timed_messages.back() ==
           WithToastDuration(Tr(StringId::kVoiceEditWrongWindow, harness.language())));
    assert(harness.input().sent_key_combos.empty());
    assert(harness.input().pasted_text == "你好世界");
}

// L0：提权前台（UIPI 会吞 SendInput）→ 拒绝并提示。
void TestVoiceEditCoordinatorRejectsElevatedForeground() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceEditHarness harness(config);
    harness.focus().state = VerifiedField("你好世界");
    assert(harness.PasteText("你好世界", 51));
    harness.probe().elevated = true;
    const std::size_t toasts_before = harness.ui().timed_messages.size();
    harness.RecordAndStop(52);
    harness.asr().on_final("删除");
    assert(harness.PumpUntil([&] {
        return harness.ui().timed_messages.size() > toasts_before;
    }));
    assert(harness.ui().timed_messages.back() ==
           WithToastDuration(Tr(StringId::kVoiceEditElevated, harness.language())));
    assert(harness.input().sent_key_combos.empty());
}

// verify_mode=auto 且 UIA 不可读：8s 窗口内按记录盲删（unverified）。
void TestVoiceEditCoordinatorUnverifiedWindowExecutes() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    config.voice_edit.verify_mode = VoiceEditVerifyMode::kAuto;
    config.voice_edit.unverified_seconds = 8;
    VoiceEditHarness harness(config);
    harness.focus().state = VoiceEditFieldState{};  // 不可验证
    assert(harness.PasteText("你好世界", 61));
    assert(harness.RunCommand("删掉最后两个字", 62));
    assert(harness.input().sent_key_combos.size() == 2);  // 无 auto_enter：2 字
    assert(harness.input().pasted_text == "你好世界");
}

// verify_mode=strict 且 UIA 不可读：一律拒绝（无任何注入）。
void TestVoiceEditCoordinatorStrictRejectsUnreadable() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    config.voice_edit.verify_mode = VoiceEditVerifyMode::kStrict;
    VoiceEditHarness harness(config);
    harness.focus().state = VoiceEditFieldState{};
    assert(harness.PasteText("你好世界", 71));
    const std::size_t toasts_before = harness.ui().timed_messages.size();
    harness.RecordAndStop(72);
    harness.asr().on_final("删除");
    assert(harness.PumpUntil([&] {
        return harness.ui().timed_messages.size() > toasts_before;
    }));
    assert(harness.ui().timed_messages.back() ==
           WithToastDuration(Tr(StringId::kVoiceEditUnreadable, harness.language())));
    assert(harness.input().sent_key_combos.empty());
}

// [voice_edit].enabled=false：完全回到现状，命令文本按普通文本上屏。
void TestVoiceEditCoordinatorDisabledPastesNormally() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    config.voice_edit.enabled = false;
    VoiceEditHarness harness(config);
    harness.focus().state = VerifiedField("你好世界");
    assert(harness.PasteText("你好世界", 81));
    assert(harness.PasteText("删除", 82));  // 非拦截：作为正文粘贴
    assert(harness.input().pasted_text == "删除");
    assert(harness.input().sent_key_combos.empty());
    assert(harness.ui().timed_messages.empty());
}

#endif  // _WIN32

}  // namespace

void RunVoiceEditTests() {
    TestVoiceEditParserPositive();
    TestVoiceEditParserDeleteAllForms();
    TestVoiceEditParserNegative();
    TestVoiceEditGraphemes();
    TestVoiceEditPlannerVerified();
    TestVoiceEditPlannerUnverified();
    TestVoiceEditVerifyModeNames();
#ifdef _WIN32
    TestVoiceEditConfigDefaultsAndPersistence();
    TestVoiceEditCoordinatorDeletesWithBackspace();
    TestVoiceEditCoordinatorRejectsWithoutRecord();
    TestVoiceEditCoordinatorRejectsChangedWindow();
    TestVoiceEditCoordinatorRejectsElevatedForeground();
    TestVoiceEditCoordinatorUnverifiedWindowExecutes();
    TestVoiceEditCoordinatorStrictRejectsUnreadable();
    TestVoiceEditCoordinatorDisabledPastesNormally();
    std::printf(">> voice edit coordinator integration tests done\n");
#endif
    std::printf(">> voice edit pure logic tests done\n");
}
