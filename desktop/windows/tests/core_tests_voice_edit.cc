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

#include <algorithm>

#include "voice_targeted_edit.h"

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

// ======================= targeted 编辑：协调器级 Fake 验证 =======================
// 与 macOS Tests/VoiceStickTests/CoordinatorFsmTests.swift 的 targeted 段同向量：
// 完整调用顺序（select → 读回 → 一次 delete/paste(false) → 恢复末尾 → 终验 → 更新）、
// 读回不符零修改、终验失败记录失效、stale generation 不覆盖新记录、off 仍拒绝盲改。

std::wstring Utf16FromUtf8ForTest(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length);
    return out;
}

std::string Utf8FromUtf16ForTest(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), length, nullptr, nullptr);
    return out;
}

// 模拟焦点应用字段（UTF-16 文档 + 选区）。
class VoiceTargetedFakeField {
public:
    std::wstring doc;
    std::size_t sel_start = 0;  // UTF-16
    std::size_t sel_len = 0;

    // Backspace：选区非空删选区，否则删最后一个字符（surrogate 对整体）。
    void ApplyBackspace() {
        if (sel_len > 0) {
            doc.erase(sel_start, sel_len);
        } else if (!doc.empty()) {
            std::size_t count = 1;
            if (doc.size() >= 2 &&
                (doc[doc.size() - 1] & 0xFC00) == 0xDC00 &&   // low surrogate
                (doc[doc.size() - 2] & 0xFC00) == 0xD800) {  // high surrogate
                count = 2;
            }
            doc.erase(doc.size() - count, count);
        }
        sel_len = 0;
    }
};

class VoiceTargetedFakeFocusReader : public IFocusTextReader {
public:
    bool ReadFocusedField(VoiceEditFieldState* out) override {
        if (out == nullptr) return false;
        *out = VoiceEditFieldState{};
        if (!readable || field == nullptr) return false;
        constexpr std::size_t kTail = 512;
        const std::size_t tail_len = std::min(kTail, field->doc.size());
        const std::size_t tail_start = field->doc.size() - tail_len;
        out->readable = true;
        out->tail_text = Utf8FromUtf16ForTest(
            std::wstring_view(field->doc).substr(tail_start, tail_len));
        out->tail_utf16_length = tail_len;
        out->selection_collapsed = field->sel_len == 0;
        out->caret_at_end = field->sel_start + field->sel_len == field->doc.size();
        return true;
    }
    VoiceTargetedFakeField* field = nullptr;
    bool readable = true;
};

class VoiceTargetedFakeEditor : public ITargetedFieldEditor {
public:
    bool SelectAndReadBack(std::size_t tail_relative_start_utf16, std::size_t length_utf16,
                           std::string* selected_text) override {
        if (selected_text == nullptr || field == nullptr) return false;
        ops->push_back("select");
        constexpr std::size_t kTail = 512;
        const std::size_t tail_len = std::min(kTail, field->doc.size());
        const std::size_t tail_start = field->doc.size() - tail_len;
        if (tail_relative_start_utf16 + length_utf16 > tail_len) return false;
        field->sel_start = tail_start + tail_relative_start_utf16;
        field->sel_len = length_utf16;
        if (drift) {
            *selected_text = "北";  // 模拟平台读回漂移（≠ target）
        } else {
            *selected_text = Utf8FromUtf16ForTest(
                std::wstring_view(field->doc).substr(field->sel_start, field->sel_len));
        }
        return true;
    }
    bool MoveCaretToEnd() override {
        ops->push_back("end");
        if (field == nullptr) return false;
        field->sel_start = field->doc.size();
        field->sel_len = 0;
        return true;
    }
    VoiceTargetedFakeField* field = nullptr;
    std::vector<std::string>* ops = nullptr;
    bool drift = false;
};

class VoiceTargetedInputInjector : public FakeInputInjector {
public:
    void Paste(const std::string& text, bool press_enter) override {
        FakeInputInjector::Paste(text, press_enter);
        if (field == nullptr) return;
        const std::wstring w = Utf16FromUtf8ForTest(text);
        field->doc.replace(field->sel_start, field->sel_len, w);
        field->sel_start += w.size();
        field->sel_len = 0;
        if (press_enter) {
            field->doc.push_back(L'\n');
            ++field->sel_start;
        }
    }
    void SendKeyCombo(const KeySpec& spec) override {
        FakeInputInjector::SendKeyCombo(spec);
        if (spec.display_text == "Backspace") {
            ops->push_back("backspace");
            if (swallow_backspace) {
                field->sel_len = 0;  // 吞掉注入：字段保持原样（终验失败路径）
            } else {
                field->ApplyBackspace();
            }
        }
    }
    VoiceTargetedFakeField* field = nullptr;
    std::vector<std::string>* ops = nullptr;
    bool swallow_backspace = false;
};

class VoiceTargetedHarness {
public:
    explicit VoiceTargetedHarness(const AppConfig& config) : config_(config) {
        auto ble = std::make_unique<FakeBleCentral>();
        ble_ = ble.get();
        auto asr = std::make_unique<FakeAsrClient>();
        asr_ = asr.get();
        auto probe = std::make_unique<VoiceEditFakeForegroundProbe>();
        probe_ = probe.get();
        auto reader = std::make_unique<VoiceTargetedFakeFocusReader>();
        reader_ = reader.get();
        auto editor = std::make_unique<VoiceTargetedFakeEditor>();
        editor_ = editor.get();
        coordinator_ = std::make_unique<VoiceStickCoordinator>(
            config_, std::move(ble), std::move(asr), &ui_, &input_);
        coordinator_->SetForegroundProbe(std::move(probe));
        coordinator_->SetFocusTextReader(std::move(reader));
        coordinator_->SetTargetedFieldEditor(std::move(editor));
        reader_->field = &field_;
        editor_->field = &field_;
        editor_->ops = &ops_;
        input_.field = &field_;
        input_.ops = &ops_;
        coordinator_->SetUiDispatcher([this](std::function<void()> action) {
            std::lock_guard lock(queue_mutex_);
            queue_.push_back(std::move(action));
        });
        coordinator_->Start();
    }
    ~VoiceTargetedHarness() {
        if (coordinator_) coordinator_->Shutdown();
    }

    FakeBleCentral& ble() { return *ble_; }
    FakeAsrClient& asr() { return *asr_; }
    FakeUi& ui() { return ui_; }
    VoiceTargetedInputInjector& input() { return input_; }
    VoiceTargetedFakeField& field() { return field_; }
    std::vector<std::string>& ops() { return ops_; }
    VoiceTargetedFakeEditor& editor() { return *editor_; }
    VoiceTargetedFakeFocusReader& reader() { return *reader_; }
    UiLanguage language() const { return EffectiveUiLanguage(config_.ui_language); }

    bool PumpOnce() {
        std::function<void()> action;
        {
            std::lock_guard lock(queue_mutex_);
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
    void RecordAndStop(std::uint32_t session_id) {
        ble_->on_state_event("5A74", ButtonEvent("button_down", "primary", session_id));
        ble_->on_audio_frame("5A74", AudioDataFrame(session_id, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        ble_->on_state_event("5A74", ButtonEvent("button_up", "primary", session_id));
        ble_->on_audio_frame("5A74", EmptyEndFrame(session_id, 2));
    }
    bool PasteText(const std::string& text, std::uint32_t session_id) {
        RecordAndStop(session_id);
        asr_->on_final(text);
        return PumpUntil([this, text] { return input_.pasted_text == text; });
    }
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
    VoiceTargetedInputInjector input_;
    VoiceTargetedFakeField field_;
    std::vector<std::string> ops_;
    VoiceEditFakeForegroundProbe* probe_ = nullptr;
    VoiceTargetedFakeFocusReader* reader_ = nullptr;
    VoiceTargetedFakeEditor* editor_ = nullptr;
    FakeBleCentral* ble_ = nullptr;
    FakeAsrClient* asr_ = nullptr;
    std::mutex queue_mutex_;
    std::vector<std::function<void()>> queue_;
    std::unique_ptr<VoiceStickCoordinator> coordinator_;
};

std::string TargetedToast(const char* text) { return std::string(text) + ":3000"; }

// 基本删除：选区 → 读回 → 一次 Backspace → 恢复末尾 → 终验 → 记录更新；
// 命令轮不 Paste 指令本身；第二次 targeted 命令基于更新后的 remaining 继续执行。
void TestTargetedCoordinatorDelete() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    harness.field().doc = L"";
    assert(harness.PasteText("我去北京上班", 121));
    // 粘贴后 fake 字段与记录一致（reader 从字段读，tail=全文）。
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 122));
    assert((harness.ops() == std::vector<std::string>{"select", "backspace", "end"}));
    assert(harness.input().sent_key_combos.size() == 1);
    assert(harness.input().pasted_text == "我去北京上班");  // 命令轮不粘贴
    assert(harness.field().doc == L"我去上班");
    std::string expected = Tr(StringId::kVoiceTargetedDeleted, harness.language());
    const auto marker = expected.find("%d");
    expected.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == TargetedToast(expected.c_str()));

    // 记录已更新（remaining="我去上班"）：第二次删「上班」→ 字段「我去」。
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的上班", 123));
    assert((harness.ops() == std::vector<std::string>{"select", "backspace", "end"}));
    assert(harness.field().doc == L"我去");
}

// 替换：paste(pressEnter=false) + 终验新文本。
void TestTargetedCoordinatorReplace() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 131));
    assert(harness.RunCommand("把刚才的北京改成上海", 132));
    assert(harness.input().pasted_text == "上海");
    assert(harness.input().pasted_enter == false);
    assert(harness.field().doc == L"我去上海上班");
    std::string expected = Tr(StringId::kVoiceTargetedReplaced, harness.language());
    const auto marker = expected.find("%d");
    expected.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == TargetedToast(expected.c_str()));
}

// auto_enter 换行保留：字段「你好世界\n」，删「世界」→ 「你好\n」。
void TestTargetedCoordinatorKeepsAutoEnterNewline() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = true;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("你好世界", 141));
    assert(harness.field().doc == L"你好世界\n");
    assert(harness.RunCommand("删除刚才的世界", 142));
    assert(harness.field().doc == L"你好\n");
}

// 选区读回不符：零删除/零粘贴 + 恢复末尾光标 + TextChanged + 记录保留。
void TestTargetedCoordinatorSelectionReadbackMismatch() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 151));
    harness.editor().drift = true;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 152));
    assert((harness.ops() == std::vector<std::string>{"select", "end"}));  // 无 backspace
    assert(harness.field().doc == L"我去北京上班");
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceTargetedTextChanged, harness.language()).c_str()));
    // 记录保留：漂移关闭后同命令可再执行。
    harness.editor().drift = false;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 153));
    assert((harness.ops() == std::vector<std::string>{"select", "backspace", "end"}));
    assert(harness.field().doc == L"我去上班");
}

// 终验失败（应用吞掉退格）：不重试、记录失效、提示检查。
void TestTargetedCoordinatorFinalVerifyFailsInvalidates() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 161));
    harness.input().swallow_backspace = true;
    assert(harness.RunCommand("删除刚才的北京", 162));
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceTargetedUnconfirmed, harness.language()).c_str()));
    // 记录已失效：再发 targeted 命令 → 没有可删除的语音输入。
    harness.input().swallow_backspace = false;
    assert(harness.RunCommand("删除刚才的北京", 163));
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceEditNoTarget, harness.language()).c_str()));
}

// verify_mode=off 且 UIA 不可读：targeted 恒 verified → 拒绝（严禁盲改）。
void TestTargetedCoordinatorOffModeUnreadableRejects() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    config.voice_edit.verify_mode = VoiceEditVerifyMode::kOff;
    VoiceTargetedHarness harness(config);
    harness.reader().readable = false;
    assert(harness.PasteText("我去北京上班", 171));
    assert(harness.RunCommand("删除刚才的北京", 172));
    assert(harness.input().sent_key_combos.empty());
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceEditUnreadable, harness.language()).c_str()));
}

// stale generation：命令执行后、终验前发生新注入 → 旧回调不覆盖新记录。
void TestTargetedCoordinatorStaleGeneration() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 181));
    // 只泵到 backspace 已发、延时终验（200ms）未触发。
    harness.RecordAndStop(182);
    harness.asr().on_final("删除刚才的北京");
    assert(harness.PumpUntil([&] { return harness.input().sent_key_combos.size() == 1; }));
    // 新注入（覆盖记录、generation +1）。stale 终验未运行（光标未回末尾），
    // 直接重置 fake 字段为干净的新注入态。
    assert(harness.PasteText("全新输入", 183));
    harness.field().doc = L"全新输入";
    harness.field().sel_start = 4;
    harness.field().sel_len = 0;
    // 等过 200ms 终验窗口：stale 回调被丢弃（无 "end" op、无成功 toast）。
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    while (harness.PumpOnce()) {
    }
    assert((harness.ops() == std::vector<std::string>{"select", "backspace"}));
    std::string expected = Tr(StringId::kVoiceTargetedDeleted, harness.language());
    for (const auto& toast : harness.ui().timed_messages) {
        assert(toast.find(expected.substr(0, expected.find("%d"))) == std::string::npos);
    }
    // 新记录未被旧 new_remaining 覆盖：基于「全新输入」再编辑可用。
    assert(harness.RunCommand("删除刚才的全新", 184));
    assert(harness.field().doc == L"输入");
}

// 旧解析器优先：「删除刚才的」仍走旧整段删除（退格×N，不走 targeted 选区）。
void TestTargetedCoordinatorOldParserPriority() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = true;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("你好世界", 191));
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的", 192));
    assert(harness.input().sent_key_combos.size() == 5);  // 4 字 + 换行
    assert(std::find(harness.ops().begin(), harness.ops().end(), "select") ==
           harness.ops().end());
}

// 无作用域简写 → 普通文本上屏；invalid 结构 → 吞掉提示、不粘贴。
void TestTargetedCoordinatorScopeAndInvalid() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("把北京改成上海", 201));  // 非指令：作为正文
    assert(harness.input().pasted_text == "把北京改成上海");
    assert(harness.PasteText("我去北京上班", 202));
    assert(harness.RunCommand("把刚才的改成上海", 203));  // invalid：吞掉不粘贴
    assert(harness.input().pasted_text == "我去北京上班");
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceTargetedInvalid, harness.language()).c_str()));
}

// 用户实报场景：两轮独立语音，自然简写在目标存在时执行，无目标时回退普通文本。
void TestTargetedCoordinatorUnscopedReplace() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我今天去北京上班。", 206));
    assert(harness.RunCommand("把北京改成上海。", 207));
    assert(harness.field().doc == L"我今天去上海上班。");
    assert(harness.input().pasted_text == "上海");
    // 最近注入已是“我今天去上海上班。”，杭州指令中的“上海”虽存在，但目标
    // “广州”不存在时必须作为普通正文，不得吞掉或误改。
    assert(harness.PasteText("把广州改成杭州", 208));
    assert(harness.input().pasted_text == "把广州改成杭州");
    assert(harness.field().doc == L"我今天去上海上班。把广州改成杭州");
}

// 长文档终验：tail 恰为 512 窗口、替换变长使 expected_tail=514（>512）时，公共后缀
// 比较必须判成功并更新记录（修复前 tail.size() >= expected_tail.size() 必误判
// resultUnconfirmed）。
void TestTargetedCoordinatorLongDocLengtheningReplace() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 211));
    // 构造 512 UTF-16 前缀 + 注入文本（tail 窗口恰好满 512）。
    harness.field().doc = std::wstring(506, L'前') + harness.field().doc;
    harness.field().sel_start = harness.field().doc.size();
    harness.field().sel_len = 0;
    assert(harness.RunCommand("把刚才的北京改成上海上海", 212));
    const std::wstring expected_doc =
        std::wstring(506, L'前') + L"我去上海上海上班";
    assert(harness.field().doc == expected_doc);
    std::string expected = Tr(StringId::kVoiceTargetedReplaced, harness.language());
    const auto marker = expected.find("%d");
    expected.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == TargetedToast(expected.c_str()));
    // 记录已更新（remaining=我去上海上海上班）：基于新 remaining 再编辑可用。
    assert(harness.RunCommand("把刚才的上海上海改成北京", 213));
    assert(harness.field().doc == std::wstring(506, L'前') + L"我去北京上班");
}

#endif  // _WIN32

// ======================= targeted 语音编辑（纯逻辑，双端同向量） =======================
// 与 macOS Tests/VoiceStickTests/VoiceTargetedEditTests.swift 使用同一张向量表
//（VoiceTargetedEdit.swift <-> voice_targeted_edit.cc 语法表逐字一致）。

void ExpectTargetedCommand(std::string_view text, VoiceTargetedActionKind kind,
                           std::string_view target, std::string_view replacement,
                           VoiceTargetedPosition pos, const char* name,
                           bool has_explicit_scope = true) {
    const auto r = ParseVoiceTargetedCommand(text);
    if (!r || r->invalid) {
        std::printf("FAIL targeted parse (expected command): %s [%s]\n", name,
                    std::string(text).c_str());
        assert(false && "expected targeted command");
    }
    if (r->command.target != target || r->command.position != pos ||
        r->command.action.kind != kind ||
        r->command.has_explicit_scope != has_explicit_scope) {
        std::printf("FAIL targeted parse: %s\n", name);
        assert(false && "targeted command mismatch");
    }
    if (kind == VoiceTargetedActionKind::kReplace &&
        r->command.action.replacement != replacement) {
        std::printf("FAIL targeted replacement: %s got [%s] want [%s]\n", name,
                    r->command.action.replacement.c_str(), std::string(replacement).c_str());
        assert(false && "targeted replacement mismatch");
    }
}

void ExpectTargetedInvalid(std::string_view text, const char* name) {
    const auto r = ParseVoiceTargetedCommand(text);
    if (!r || !r->invalid) {
        std::printf("FAIL targeted parse (expected invalid): %s [%s]\n", name,
                    std::string(text).c_str());
        assert(false && "expected targeted invalid");
    }
}

void ExpectNoTargeted(std::string_view text, const char* name) {
    if (ParseVoiceTargetedCommand(text).has_value()) {
        std::printf("FAIL targeted parse (expected non-command): %s [%s]\n", name,
                    std::string(text).c_str());
        assert(false && "expected no targeted command");
    }
}

VoiceTargetedCommand TargetedDelete(std::string target,
                                    VoiceTargetedPosition pos = VoiceTargetedPosition::kUnspecified) {
    VoiceTargetedCommand cmd;
    cmd.action = VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}};
    cmd.target = std::move(target);
    cmd.position = pos;
    return cmd;
}

VoiceTargetedCommand TargetedReplace(std::string target, std::string replacement,
                                     VoiceTargetedPosition pos = VoiceTargetedPosition::kUnspecified) {
    VoiceTargetedCommand cmd;
    cmd.action = VoiceTargetedAction{VoiceTargetedActionKind::kReplace, std::move(replacement)};
    cmd.target = std::move(target);
    cmd.position = pos;
    return cmd;
}

void TestTargetedParserPositive() {
    ExpectTargetedCommand("删除刚才的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "删除刚才的北京");
    ExpectTargetedCommand("删掉刚才的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "删掉刚才的北京");
    ExpectTargetedCommand("删去刚才的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "删去刚才的北京");
    ExpectTargetedCommand("去掉刚才的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "去掉刚才的北京");
    ExpectTargetedCommand("删除刚刚的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "刚刚作用域");
    ExpectTargetedCommand("删除刚才北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "无「的」作用域");
    ExpectTargetedCommand("删除刚才第一个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kFirst, "第一个");
    ExpectTargetedCommand("删除刚才最后一个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kLast, "最后一个");
    ExpectTargetedCommand("把刚才第一个北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kFirst, "把字句第一个");
    ExpectTargetedCommand("把刚才最后一个北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kLast, "把字句最后一个");
    ExpectTargetedCommand("把刚才的最后一个北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kLast, "的最后一个语序");
    ExpectTargetedCommand("把刚才的北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "把字句删掉");
    ExpectTargetedCommand("把刚才的北京删除", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "把字句删除");
    ExpectTargetedCommand("把刚才的北京去掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "把字句去掉");
    ExpectTargetedCommand("把刚才的北京删去", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "把字句删去");
    ExpectTargetedCommand("请把刚才的北京删除", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "句尾删除动词（LastTokenIndex 边界）");
    // 分隔符优先：句中/句尾出现删除动词仍按替换解析（最小复现回归）。
    ExpectTargetedCommand("把刚才的删除按钮改成红色", VoiceTargetedActionKind::kReplace,
                          "删除按钮", "红色", VoiceTargetedPosition::kUnspecified,
                          "目标含删除动词的替换");
    ExpectTargetedCommand("把刚才的北京改成删除", VoiceTargetedActionKind::kReplace, "北京",
                          "删除", VoiceTargetedPosition::kUnspecified, "替换值为删除动词");
    // 目标本身含删除动词、句尾动词的纯删除。
    ExpectTargetedCommand("把刚才的删除删掉", VoiceTargetedActionKind::kDelete, "删除", "",
                          VoiceTargetedPosition::kUnspecified, "目标=删除");
    ExpectTargetedCommand("把刚才的删掉删除", VoiceTargetedActionKind::kDelete, "删掉", "",
                          VoiceTargetedPosition::kUnspecified, "目标=删掉");
    ExpectTargetedCommand("把刚才的北京改成上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "改成");
    ExpectTargetedCommand("把刚才的北京改为上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "改为");
    ExpectTargetedCommand("把刚才的北京换成上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "换成");
    ExpectTargetedCommand("把刚才的北京替换为上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "替换为");
    ExpectTargetedCommand("把刚才的北京替换成上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "替换成");
    ExpectTargetedCommand("把刚才最后一个北京改成上海", VoiceTargetedActionKind::kReplace, "北京",
                          "上海", VoiceTargetedPosition::kLast, "最后一个+改成");
    ExpectTargetedCommand("删除刚才的北京大学", VoiceTargetedActionKind::kDelete, "北京大学", "",
                          VoiceTargetedPosition::kUnspecified, "多字目标");
    ExpectTargetedCommand("把刚才的北京大学改成清华大学", VoiceTargetedActionKind::kReplace,
                          "北京大学", "清华大学", VoiceTargetedPosition::kUnspecified,
                          "多字替换");
    ExpectTargetedCommand("把刚才的new york改成上海", VoiceTargetedActionKind::kReplace,
                          "new york", "上海", VoiceTargetedPosition::kUnspecified,
                          "内部空白保留");
    ExpectTargetedCommand("删除刚才的北京。", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "尾部句号");
    ExpectTargetedCommand("  把刚才的北京改成上海。 ", VoiceTargetedActionKind::kReplace,
                          "北京", "上海", VoiceTargetedPosition::kUnspecified, "空白+句号");
    ExpectTargetedCommand("请把刚才的北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "请前缀");
    ExpectTargetedCommand("请帮我删除刚才的北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "请帮我前缀");
    ExpectTargetedCommand("麻烦把刚才的北京改成上海", VoiceTargetedActionKind::kReplace, "北京",
                          "上海", VoiceTargetedPosition::kUnspecified, "麻烦前缀");
    ExpectTargetedCommand("把刚才的北京删掉吧", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "吧语气尾");
    ExpectTargetedCommand("把刚才的北京改成上海吧", VoiceTargetedActionKind::kReplace, "北京",
                          "上海", VoiceTargetedPosition::kUnspecified, "替换+吧");
    ExpectTargetedCommand("删除刚才的北京啊", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "啊语气尾");
    ExpectTargetedCommand("删除刚才的吧", VoiceTargetedActionKind::kDelete, "吧", "",
                          VoiceTargetedPosition::kUnspecified, "target=吧");
    std::string t32, t33, r64, r65;
    for (int i = 0; i < 32; ++i) t32 += "字";
    for (int i = 0; i < 33; ++i) t33 += "字";
    for (int i = 0; i < 64; ++i) r64 += "替";
    for (int i = 0; i < 65; ++i) r65 += "替";
    ExpectTargetedCommand("删除刚才的" + t32, VoiceTargetedActionKind::kDelete, t32, "",
                          VoiceTargetedPosition::kUnspecified, "目标32边界");
    ExpectTargetedCommand("把刚才的" + t32 + "改成" + r64, VoiceTargetedActionKind::kReplace,
                          t32, r64, VoiceTargetedPosition::kUnspecified, "替换64边界");
    ExpectTargetedInvalid("删除刚才的" + t33, "目标33超限");
    ExpectTargetedInvalid("把刚才的" + t32 + "改成" + r65, "替换65超限");
}

void TestTargetedParserNegative() {
    // 自然简写：解析为 has_explicit_scope=false 的候选，协调器仅在目标存在时接管。
    ExpectTargetedCommand("把北京改成上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kUnspecified, "自然简写替换", false);
    ExpectTargetedCommand("删除北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "自然简写删除", false);
    ExpectTargetedCommand("把北京删掉", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kUnspecified, "自然简写把字句删除", false);
    ExpectTargetedCommand("把第一个北京改成上海", VoiceTargetedActionKind::kReplace, "北京", "上海",
                          VoiceTargetedPosition::kFirst, "自然简写带位置", false);
    ExpectTargetedCommand("请把删除按钮改成红色", VoiceTargetedActionKind::kReplace, "删除按钮", "红色",
                          VoiceTargetedPosition::kUnspecified, "礼貌前缀自然简写", false);
    ExpectNoTargeted("把改成上海", "简写空目标");
    ExpectNoTargeted("把北京改成", "简写空替换");
    ExpectNoTargeted("把北京改成上海改成广州", "简写多分隔符");
    ExpectNoTargeted("删除刚才的", "旧整段删除优先");
    ExpectTargetedCommand("删除最后两个字", VoiceTargetedActionKind::kDelete, "最后两个字", "",
                          VoiceTargetedPosition::kUnspecified, "简写候选但协调器旧解析优先", false);
    ExpectNoTargeted("删除", "单独删除");
    ExpectNoTargeted("你好删除刚才的北京", "混说");
    ExpectNoTargeted("把刚才的文件删除一下可以吗", "疑问夹带");
    ExpectNoTargeted("不删除刚才的北京", "否定前导");
    ExpectTargetedInvalid("把刚才的北京不要删掉", "否定把字句");
    ExpectTargetedInvalid("把刚才的别删掉", "别删掉");
    ExpectNoTargeted("把刚才的北京删掉吗", "疑问吗");
    ExpectNoTargeted("删除刚才的北京好吗", "疑问好吗");
    ExpectTargetedInvalid("把刚才的改成上海", "空目标");
    ExpectTargetedInvalid("把刚才的改成", "空目标无替换");
    ExpectTargetedInvalid("把刚才的北京改成", "空替换");
    ExpectTargetedInvalid("把刚才的 改成上海", "空白空目标");
    ExpectTargetedInvalid("把刚才的北京改成上海改成广州", "多分隔符");
    ExpectTargetedInvalid("把刚才的改成改成上海", "目标含分隔符");
    ExpectTargetedInvalid("把刚才的第一个改成上海", "位置后无目标");
    ExpectTargetedInvalid("把刚才的北京改成改成", "空替换多分隔符");
    ExpectTargetedInvalid("删除刚才的北\n京", "多行目标");
    ExpectTargetedInvalid("把刚才的北京改成上\n海", "多行替换");
    ExpectTargetedInvalid("删除刚才的北\r京", "CR 目标（单行约束）");
    ExpectTargetedInvalid("把刚才的北京改成上\r海", "CR 替换（单行约束）");
    ExpectTargetedInvalid("删除刚才的别删", "动宾否定别");
    ExpectTargetedInvalid("删除刚才的北京不要", "动宾否定不要");
    ExpectNoTargeted("", "空串");
    ExpectNoTargeted("把", "单字");
    ExpectNoTargeted("把刚才的", "作用域后无内容");
    ExpectNoTargeted("把刚才的北京", "把字句无动词");
    ExpectNoTargeted("把刚才的北京删掉快点", "动词后夹带");
    ExpectTargetedCommand("把刚才的北京改成上海然后继续", VoiceTargetedActionKind::kReplace,
                          "北京", "上海然后继续", VoiceTargetedPosition::kUnspecified,
                          "替换取剩余全文");
    std::string huge = "删除刚才的" + std::string(130, 'x');
    ExpectNoTargeted(huge, "超长指令");
}

void TestTargetedMatcher() {
    const auto cjk = FindTargetedOccurrences("北京", "我去北京上班");
    assert(cjk.size() == 1);
    assert(cjk[0].grapheme_start == 2 && cjk[0].grapheme_end == 4);
    assert(cjk[0].utf16_start == 2 && cjk[0].utf16_length == 2);
    assert(cjk[0].total == 1);

    const auto ascii = FindTargetedOccurrences("ab", "xabcab");
    assert(ascii.size() == 2);
    assert(ascii[0].grapheme_start == 1 && ascii[1].grapheme_start == 4);
    assert(ascii[1].utf16_start == 4);

    // 非重叠："aa" in "aaaa" → 2 处（0、2），不是 3 处。
    const auto overlap = FindTargetedOccurrences("aa", "aaaa");
    assert(overlap.size() == 2);
    assert(overlap[0].grapheme_start == 0 && overlap[1].grapheme_start == 2);

    // emoji（surrogate pair = 2 UTF-16，1 grapheme）。
    const auto emoji = FindTargetedOccurrences("🙂", "好🙂吗🙂");
    assert(emoji.size() == 2);
    assert(emoji[0].utf16_start == 1 && emoji[0].utf16_length == 2);
    assert(emoji[1].utf16_start == 4);

    // 组合字符（e + U+0301 = 2 UTF-16，1 grapheme）。
    const auto combining = FindTargetedOccurrences("é", "caféx");
    assert(combining.size() == 1);
    assert(combining[0].utf16_start == 3 && combining[0].utf16_length == 2);

    // 开头/中间/末尾与空输入。
    assert(FindTargetedOccurrences("你", "你好").size() == 1);
    assert(FindTargetedOccurrences("你", "好你").front().grapheme_start == 1);
    assert(FindTargetedOccurrences("你", "他好").empty());
    assert(FindTargetedOccurrences("你", "").empty());
    assert(FindTargetedOccurrences("", "你好").empty());
}

void TestTargetedPlanner() {
    const auto del = TargetedDelete("北京");
    const auto rep = TargetedReplace("北京", "上海");

    auto p1 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班", true, true);
    assert(p1.ok && p1.new_remaining_text == "我去上班");
    assert(p1.target_graphemes == 2);
    assert(p1.occurrence.utf16_start == 2 && p1.occurrence.utf16_length == 2);
    assert(p1.tail_prefix.empty() && !p1.has_recorded_newline);
    assert(p1.remaining_utf16_length == 6);

    auto p2 = PlanVoiceTargeted(rep, "我去北京上班", false, true, "我去北京上班", true, true);
    assert(p2.ok && p2.new_remaining_text == "我去上海上班");

    // auto_enter 换行：pressedEnter=true、tail 以 \n 结尾 → 通过且换行保留。
    auto p3 = PlanVoiceTargeted(del, "我去北京上班", true, true, "我去北京上班\n", true, true);
    assert(p3.ok && p3.has_recorded_newline && p3.new_remaining_text == "我去上班");
    // 未记录换行（pressedEnter=false 但 tail 多 \n）→ field_mismatch。
    auto p4 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班\n", true, true);
    assert(!p4.ok && p4.failure == VoiceTargetedFailure::kFieldMismatch);
    // 字段前缀（用户既有内容）。
    auto p5 = PlanVoiceTargeted(del, "我去北京上班", false, true, "前面的话我去北京上班", true, true);
    assert(p5.ok && p5.tail_prefix == "前面的话" && p5.new_remaining_text == "我去上班");
    // 手工改动 → 不用旧「最长前缀」容错。
    auto p6 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北上班", true, true);
    assert(!p6.ok && p6.failure == VoiceTargetedFailure::kFieldMismatch);
    auto p6b = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班吗", true, true);
    assert(!p6b.ok && p6b.failure == VoiceTargetedFailure::kFieldMismatch);
    // 歧义 / 第一个 / 最后一个。
    auto p7 = PlanVoiceTargeted(del, "北京和北京", false, true, "北京和北京", true, true);
    assert(!p7.ok && p7.failure == VoiceTargetedFailure::kAmbiguous);
    auto p8 = PlanVoiceTargeted(TargetedDelete("北京", VoiceTargetedPosition::kFirst),
                                "北京和北京", false, true, "北京和北京", true, true);
    assert(p8.ok && p8.new_remaining_text == "和北京" && p8.occurrence.index == 0);
    auto p9 = PlanVoiceTargeted(TargetedReplace("北京", "上海", VoiceTargetedPosition::kLast),
                                "北京和北京", false, true, "北京和北京", true, true);
    assert(p9.ok && p9.new_remaining_text == "北京和上海" && p9.occurrence.index == 1);
    // notFound / 光标 / 选区 / 不可读（targeted 恒 verified，无 verify_mode 分支）。
    auto p10 = PlanVoiceTargeted(TargetedDelete("上海"), "我去北京上班", false, true,
                                 "我去北京上班", true, true);
    assert(!p10.ok && p10.failure == VoiceTargetedFailure::kNotFound);
    auto p11 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班", false, true);
    assert(!p11.ok && p11.failure == VoiceTargetedFailure::kCaretNotEnd);
    auto p12 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班", true, false);
    assert(!p12.ok && p12.failure == VoiceTargetedFailure::kSelectionPresent);
    auto p13 = PlanVoiceTargeted(del, "我去北京上班", false, false, "", true, true);
    assert(!p13.ok && p13.failure == VoiceTargetedFailure::kVerifyUnavailable);
    // 连续两次编辑（计划级）。
    auto p14 = PlanVoiceTargeted(TargetedDelete("上班"), p1.new_remaining_text, false, true,
                                 "我去上班", true, true);
    assert(p14.ok && p14.new_remaining_text == "我去");
    // emoji 删除选区（UTF-16 长度 2）。
    auto p15 = PlanVoiceTargeted(TargetedDelete("🙂"), "好🙂吗", false, true, "好🙂吗", true, true);
    assert(p15.ok && p15.occurrence.utf16_start == 1 && p15.occurrence.utf16_length == 2 &&
           p15.new_remaining_text == "好吗");
    // remaining 自身以换行结尾（口述多行文本）：pressedEnter 优先按 remaining+"\n"
    // 判定，避免误配裸后缀（与 macOS 协调器拆解同顺序）。
    auto p16 = PlanVoiceTargeted(TargetedDelete("你好"), "你好\n", true, true, "你好\n\n", true, true);
    assert(p16.ok && p16.new_remaining_text == "\n" && p16.occurrence.utf16_start == 0);
    assert(p16.has_recorded_newline && p16.tail_prefix.empty());
    // 无 auto_enter 换行（tail 恰为 remaining 裸后缀）同样通过。
    auto p17 = PlanVoiceTargeted(TargetedDelete("你好"), "你好\n", true, true, "你好\n", true, true);
    assert(p17.ok && p17.new_remaining_text == "\n" && !p17.has_recorded_newline);
    // pressedEnter=false 但 tail 多一个换行 → 来历不明，拒绝。
    auto p18 = PlanVoiceTargeted(TargetedDelete("你好"), "你好\n", false, true, "你好\n\n", true, true);
    assert(!p18.ok && p18.failure == VoiceTargetedFailure::kFieldMismatch);
}

}  // namespace

void RunVoiceEditTests() {
    TestVoiceEditParserPositive();
    TestVoiceEditParserDeleteAllForms();
    TestVoiceEditParserNegative();
    TestVoiceEditGraphemes();
    TestVoiceEditPlannerVerified();
    TestVoiceEditPlannerUnverified();
    TestVoiceEditVerifyModeNames();
    TestTargetedParserPositive();
    TestTargetedParserNegative();
    TestTargetedMatcher();
    TestTargetedPlanner();
#ifdef _WIN32
    TestVoiceEditConfigDefaultsAndPersistence();
    TestVoiceEditCoordinatorDeletesWithBackspace();
    TestVoiceEditCoordinatorRejectsWithoutRecord();
    TestVoiceEditCoordinatorRejectsChangedWindow();
    TestVoiceEditCoordinatorRejectsElevatedForeground();
    TestVoiceEditCoordinatorUnverifiedWindowExecutes();
    TestVoiceEditCoordinatorStrictRejectsUnreadable();
    TestVoiceEditCoordinatorDisabledPastesNormally();
    TestTargetedCoordinatorDelete();
    TestTargetedCoordinatorReplace();
    TestTargetedCoordinatorKeepsAutoEnterNewline();
    TestTargetedCoordinatorSelectionReadbackMismatch();
    TestTargetedCoordinatorFinalVerifyFailsInvalidates();
    TestTargetedCoordinatorOffModeUnreadableRejects();
    TestTargetedCoordinatorStaleGeneration();
    TestTargetedCoordinatorOldParserPriority();
    TestTargetedCoordinatorScopeAndInvalid();
    TestTargetedCoordinatorUnscopedReplace();
    TestTargetedCoordinatorLongDocLengtheningReplace();
    std::printf(">> voice edit + targeted coordinator integration tests done\n");
#endif
    std::printf(">> voice edit pure logic tests done\n");
}
