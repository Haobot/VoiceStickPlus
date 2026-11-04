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

#include "voice_edit_anchor.h"
#include "voice_targeted_domain.h"
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
    field.tail_utf16_length = VoiceTargetedUtf16Length(tail);
    // 批次 1：caret_at_end=true 时补窗口内偏移（= tail 末尾），激活 P1a/T10 等价
    // 路径；false 保持 kCaretOutsideTail（旧行为：仅 P1c/P2 + caret_at_end 兜底）。
    if (caret_at_end) field.caret_tail_utf16 = field.tail_utf16_length;
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


// ============================ 批次 1：字段内定位器（T1-T13 双端同向量）============================
// 与 macOS Tests/VoiceStickTests/VoiceEditAnchorTests.swift 同一张向量表（双端同
// 向量约定；T14/T15 为 macOS 协调器级用例，Windows 侧仅纯逻辑可达的 stage 日志点）。

namespace {

void ExpectAnchor(const VoiceEditAnchorResult& result, VoiceEditAnchor::Kind kind,
                  std::size_t start, std::size_t end, const char* name) {
    if (!result.ok || result.anchor.kind != kind || result.anchor.anchor_start != start ||
        result.anchor.anchor_end != end) {
        std::printf("FAIL anchor %s: ok=%d kind=%d start=%zu end=%zu\n", name,
                    result.ok ? 1 : 0, static_cast<int>(result.anchor.kind),
                    result.anchor.anchor_start, result.anchor.anchor_end);
        assert(false && "anchor mismatch");
    }
}

void ExpectAnchorFailure(const VoiceEditAnchorResult& result,
                         VoiceEditAnchorFailure failure, const char* name) {
    if (result.ok || result.failure != failure) {
        std::printf("FAIL anchor failure %s: ok=%d failure=%d\n", name, result.ok ? 1 : 0,
                    static_cast<int>(result.failure));
        assert(false && "anchor failure mismatch");
    }
}

std::string Anchored(const std::string& remaining, bool pressed_enter) {
    return pressed_enter ? remaining + "\n" : remaining;
}

}  // namespace

void TestVoiceEditAnchorLocator() {
    // T1 光标紧邻段尾（P1a）。
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班", 4, 0, "北京", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 2, 4, "T1 P1a");
    // T2 光标紧邻段首（P1b）。
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班", 2, 0, "北京", false),
                 VoiceEditAnchor::Kind::kCaretSuffix, 2, 4, "T2 P1b");
    // T3 字段末尾追加：光标在末尾（P1a 同位，anchorEnd=字段末尾）+ 光标在开头（P1c）。
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班", 6, 0, "上班", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 4, 6, "T3 P1a 同位");
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班", 0, 0, "上班", false),
                 VoiceEditAnchor::Kind::kFieldEnd, 4, 6, "T3b P1c");
    // T3c/d P1c 已记录换行优先/裸回落（FieldEndAnchor 专用接口）。
    {
        const auto a = LocateVoiceEditFieldEndAnchor("我去北京上班\n", "上班", true);
        assert(a.anchor_start == 4 && a.anchor_end == 7);
        const auto b = LocateVoiceEditFieldEndAnchor("我去北京上班", "上班", true);
        assert(b.anchor_start == 4 && b.anchor_end == 6);
        const auto c = LocateVoiceEditFieldEndAnchor("我去上海\n", "上班", true);
        assert(c.anchor_start == c.anchor_end);  // miss 哨兵
    }
    // T4 段唯一出现（P2，光标在别处）。
    ExpectAnchor(LocateVoiceEditAnchor("我在杭州北京出差", 7, 0, "北京", false),
                 VoiceEditAnchor::Kind::kUniqueOccurrence, 4, 6, "T4 P2");
    // T5 段出现两次且 P1 全 miss → anchor_ambiguous。
    ExpectAnchorFailure(
        LocateVoiceEditAnchor("北京很好，我去北京出差", 11, 0, "北京", false),
        VoiceEditAnchorFailure::kAnchorAmbiguous, "T5 ambiguous");
    // T6 用户改过段内文本（全 miss）→ field_mismatch。
    ExpectAnchorFailure(LocateVoiceEditAnchor("背景很好", 4, 0, "北京", false),
                        VoiceEditAnchorFailure::kFieldMismatch, "T6 mismatch");
    // T7 pressedEnter 已记录换行 + P1a：anchored 含 \n。
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班\n吗", 7, 0, "上班\n", true),
                 VoiceEditAnchor::Kind::kCaretPrefix, 4, 7, "T7 P1a 含换行");
    // T8 anchored 自身以 \n 结尾（口述多行）：不误并前缀。
    ExpectAnchor(LocateVoiceEditAnchor("序第一行\n尾", 5, 0, "第一行\n", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 1, 5, "T8 口述换行");
    {
        const auto b = LocateVoiceEditFieldEndAnchor("序你好\n", "你好\n", true);
        assert(b.anchor_start == 1 && b.anchor_end == 4);  // 裸回落锚段=remaining
    }
    // T11 surrogate 对（emoji）：UTF-16 偏移（🙂=2 码元；🇨🇳=4 码元）。
    ExpectAnchor(LocateVoiceEditAnchor("你🙂好呀", 4, 0, "🙂好", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 1, 4, "T11 emoji");
    ExpectAnchor(LocateVoiceEditAnchor("我爱🇨🇳国", 7, 0, "🇨🇳国", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 2, 7, "T11b 国旗");
    // 补充：选区非空跳过 P1a/P1b → P1c。
    ExpectAnchor(LocateVoiceEditAnchor("我去北京上班", 2, 2, "上班", false),
                 VoiceEditAnchor::Kind::kFieldEnd, 4, 6, "选区非空 → P1c");
    // 补充：P1a 固定最优先。
    ExpectAnchor(LocateVoiceEditAnchor("你好你好", 2, 0, "你好", false),
                 VoiceEditAnchor::Kind::kCaretPrefix, 0, 2, "P1a 最优先");
}

void TestVoiceEditAnchorPlanner() {
    // T10 回归底线：caret 在锚点（P1a 同位、含换行）→ 计划与批次 1 之前逐字段
    // 等价（needs_caret_move=false、anchor_end=字段末尾）。
    {
        const auto plan = PlanVoiceEdit("你好世界", true, Chars(2),
                                        VerifiedField("你好世界\n"),
                                        VoiceEditVerifyMode::kAuto, false);
        assert(plan.ok && plan.backspace_count == 3 && plan.delete_graphemes == 2 &&
               plan.delete_newline);
        assert(!plan.needs_caret_move && plan.anchor_end_utf16 == 5);
    }
    // T9 光标不在 anchorEnd（偏移可得，段中）→ 计划成功 + needs_caret_move=true
    //（协调器先移光标；移动失败零退格由协调器级用例覆盖，见
    // TestVoiceEditCoordinatorCaretMove）。
    {
        auto field = VerifiedField("你好世界");
        field.caret_tail_utf16 = 1;  // 光标移到段中
        const auto plan = PlanVoiceEdit("你好世界", false, Chars(2), field,
                                        VoiceEditVerifyMode::kAuto, false);
        assert(plan.ok && plan.backspace_count == 2);
        assert(plan.needs_caret_move && plan.anchor_end_utf16 == 4);
    }
    // T9 变体：段后手打 + 光标在末尾 → P2 锚（anchor_end=5=段尾）需移动。
    {
        auto field = VerifiedField("你好世界\n吗");
        field.caret_tail_utf16 = 7;
        const auto plan = PlanVoiceEdit("你好世界", true, Chars(2), field,
                                        VoiceEditVerifyMode::kAuto, false);
        assert(plan.ok && plan.backspace_count == 3 && plan.delete_newline);
        assert(plan.needs_caret_move && plan.anchor_end_utf16 == 5);
    }
}

void TestTargetedAnchorPlanner() {
    // T1 P1a 句中替换（字段「我去北京上班」、光标 4、锚 [2,4)）。
    {
        const VoiceEditAnchor anchor{VoiceEditAnchor::Kind::kCaretPrefix, 2, 4};
        VoiceTargetedCommand command;
        command.action.kind = VoiceTargetedActionKind::kReplace;
        command.action.replacement = "杭州";
        command.target = "北京";
        const auto plan = PlanVoiceTargetedAnchored(command, "北京", false, true,
                                                    "我去北京上班", anchor);
        assert(plan.ok && plan.new_remaining_text == "杭州");
        assert(plan.occurrence.utf16_start == 0 && plan.occurrence.utf16_length == 2);
        assert(plan.anchor_start_utf16 == 2 && plan.anchor_end_utf16 == 4);
        assert(plan.tail_prefix == "我去" && plan.anchor_after_text == "上班");
        assert(!plan.has_recorded_newline);
    }
    // T2 P1b 段首删除。
    {
        const VoiceEditAnchor anchor{VoiceEditAnchor::Kind::kCaretSuffix, 2, 4};
        VoiceTargetedCommand command;
        command.action.kind = VoiceTargetedActionKind::kDelete;
        command.target = "北京";
        const auto plan = PlanVoiceTargetedAnchored(command, "北京", false, true,
                                                    "我去北京上班", anchor);
        assert(plan.ok && plan.new_remaining_text.empty());
    }
    // T4 P2 唯一出现替换（段后内容进 anchor_after_text，终验期望拼接用）。
    {
        const VoiceEditAnchor anchor{VoiceEditAnchor::Kind::kUniqueOccurrence, 4, 6};
        VoiceTargetedCommand command;
        command.action.kind = VoiceTargetedActionKind::kReplace;
        command.action.replacement = "上海";
        command.target = "北京";
        const auto plan = PlanVoiceTargetedAnchored(command, "北京", false, true,
                                                    "我在杭州北京出差", anchor);
        assert(plan.ok && plan.new_remaining_text == "上海");
        assert(plan.tail_prefix == "我在杭州" && plan.anchor_after_text == "出差");
    }
    // T12 targeted 恒 verified 红线：字段不可读 → verify_unavailable（锚路径同样）。
    {
        const VoiceEditAnchor anchor{VoiceEditAnchor::Kind::kCaretPrefix, 2, 4};
        VoiceTargetedCommand command;
        command.action.kind = VoiceTargetedActionKind::kDelete;
        command.target = "北京";
        const auto plan = PlanVoiceTargetedAnchored(command, "北京", false, false,
                                                    "我去北京上班", anchor);
        assert(!plan.ok && plan.failure == VoiceTargetedFailure::kVerifyUnavailable);
    }
    // T13 句中句号剥离纯函数（与 macOS stripMidlinePeriod 同表）。
    assert(StripMidlinePeriod("今天天气不错。", true) == "今天天气不错");
    assert(StripMidlinePeriod("今天天气不错．", true) == "今天天气不错");
    assert(StripMidlinePeriod("收到.", true) == "收到");
    assert(StripMidlinePeriod("真的吗！", true) == "真的吗！");
    assert(StripMidlinePeriod("是吗？", true) == "是吗？");
    assert(StripMidlinePeriod("一共3.", true) == "一共3.");
    assert(StripMidlinePeriod("升级到v1.", true) == "升级到v1.");
    assert(StripMidlinePeriod("句话.", true) == "句话");
    assert(StripMidlinePeriod("今天天气不错。", false) == "今天天气不错。");
    assert(StripMidlinePeriod("", true).empty());
    assert(MidlinePeriodStrippable("不错。"));
    assert(!MidlinePeriodStrippable("3."));
    assert(!MidlinePeriodStrippable("不错！"));
    // 裸句号守卫（与标点独词输入的交叉修复：单独说「句号」→ 注入文本恰为「。」）：
    // 句号之前去掉首尾空白后为空 → 刻意注入的标点，句中也永不剥；句号前有内容
    // 照剥。预检同口径（与 macOS 同表）。
    assert(StripMidlinePeriod("。", true) == "。");
    assert(StripMidlinePeriod("．", true) == "．");
    assert(StripMidlinePeriod(".", true) == ".");
    assert(StripMidlinePeriod(" 。", true) == " 。");
    assert(StripMidlinePeriod("\n.", true) == "\n.");
    assert(StripMidlinePeriod("今天很好。", true) == "今天很好");
    assert(!MidlinePeriodStrippable("。"));
    assert(!MidlinePeriodStrippable("．"));
    assert(!MidlinePeriodStrippable("."));
    assert(!MidlinePeriodStrippable(" 。"));
    assert(!MidlinePeriodStrippable("\n."));
    assert(MidlinePeriodStrippable("今天很好。"));
    // isMidlineInsertion：光标后同一逻辑行还有非空白字符。
    assert(IsMidlineInsertion("你好世界", 2));
    assert(!IsMidlineInsertion("你好世界", 4));
    assert(!IsMidlineInsertion("你好\n世界", 2));
    assert(!IsMidlineInsertion("你好  世界", 2));
    // 翻译模式样本（判定只看插入位置，不看模式）。
    assert(StripMidlinePeriod("今天天气很好。", true) == "今天天气很好");
    assert(StripMidlinePeriod("The weather is nice today.", true) ==
           "The weather is nice today.");  // ASCII 字母后句点保留（双端一致）
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

    // 批次 1 语义更新：字段尾被用户手动删过（字段只剩前缀）——零容错拒绝
    //（旧「最长前缀」容错已移除：偏移不可靠宁拒勿错）。
    ExpectReject("你好世界", true, Chars(2), VerifiedField("你好世\n"),
                 VoiceEditVerifyMode::kAuto, false, VoiceEditRejectReason::kTextMismatch);

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

    // 批次 1 语义更新：字段尾被追加手打内容（光标在末尾）——P2 唯一出现锚命中，
    // 计划成功（协调器先移光标到锚点再退格）。
    ExpectPlan("你好世界", true, Chars(2), VerifiedField("你好世界\n吗"),
               VoiceEditVerifyMode::kAuto, false, 3, 2, true, "你好");
    // 字段文本已不在输入框（已发送/切换应用）→ 拒绝。
    ExpectReject("你好世界", false, Chars(2), VerifiedField(""),
                 VoiceEditVerifyMode::kAuto, false, VoiceEditRejectReason::kTextMismatch);
    // 批次 1 语义更新：光标不在文末（偏移不可得，caret_at_end=false 兜底）→
    // P1c 命中且 needs_caret_move=true（协调器先移光标），不再 caret_not_end。
    {
        const auto plan = PlanVoiceEdit("你好世界", false, Chars(2),
                                        VerifiedField("你好世界", false),
                                        VoiceEditVerifyMode::kAuto, false);
        assert(plan.ok && plan.backspace_count == 2 && plan.needs_caret_move);
        assert(plan.anchor_end_utf16 == 4);
    }
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
    // 批次 2（整框域）：Fake 直接把 tail 当全文（测试字段都远小于上限）。
    bool ReadFocusedFieldFullText(VoiceEditFieldState* out) override {
        if (out == nullptr) return false;
        *out = state;
        if (!state.readable) return false;
        out->full_text = state.tail_text;
        out->full_utf16_length = state.tail_utf16_length;
        out->full_truncated = false;
        if (out->caret_tail_utf16 != kCaretOutsideTail) {
            out->caret_abs_utf16 = out->caret_tail_utf16;  // Fake：tail 覆盖全文
        }
        return true;
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
        // 批次 1（字段内定位）：小字段 tail 覆盖全文，tail 相对偏移 = 绝对偏移。
        const std::size_t caret = field->sel_start + field->sel_len;
        out->caret_tail_utf16 = caret <= tail_start + tail_len && caret >= tail_start
                                    ? caret - tail_start
                                    : kCaretOutsideTail;
        return true;
    }
    // 批次 2（整框域）：Fake 读全文（测试字段都远小于 200k 上限）。
    bool ReadFocusedFieldFullText(VoiceEditFieldState* out) override {
        if (out == nullptr) return false;
        *out = VoiceEditFieldState{};
        if (!readable || field == nullptr) return false;
        out->readable = true;
        out->full_text = Utf8FromUtf16ForTest(std::wstring_view(field->doc));
        out->full_utf16_length = field->doc.size();
        out->full_truncated = false;
        out->selection_collapsed = field->sel_len == 0;
        out->caret_at_end = field->sel_start + field->sel_len == field->doc.size();
        out->caret_abs_utf16 = field->sel_start + field->sel_len;
        // 尾窗字段同步（既有消费方保持可用；小字段 tail = 全文）。
        constexpr std::size_t kTail = 512;
        const std::size_t tail_len = std::min(kTail, field->doc.size());
        const std::size_t tail_start = field->doc.size() - tail_len;
        out->tail_text = Utf8FromUtf16ForTest(
            std::wstring_view(field->doc).substr(tail_start, tail_len));
        out->tail_utf16_length = tail_len;
        out->caret_tail_utf16 = out->caret_abs_utf16 >= tail_start
                                    ? out->caret_abs_utf16 - tail_start
                                    : kCaretOutsideTail;
        return true;
    }
    VoiceTargetedFakeField* field = nullptr;
    bool readable = true;
};

class VoiceTargetedFakeEditor : public ITargetedFieldEditor {
public:
    // ---- 写/读分离（读回轮询，2026-11-04 对齐 macOS 780d1a2）----
    bool SelectTailRange(std::size_t tail_relative_start_utf16,
                         std::size_t length_utf16) override {
        if (field == nullptr || select_fails) return false;
        ops->push_back("select");
        // swallow_select：模拟 Chromium UIA Select() 返回 S_OK 但选区写零落地
        // （GetSelection 恒折叠光标，2026-11-04 复测 got_len=0 实锤形态）。
        if (swallow_select) return true;
        constexpr std::size_t kTail = 512;
        const std::size_t tail_len = std::min(kTail, field->doc.size());
        const std::size_t tail_start = field->doc.size() - tail_len;
        if (tail_relative_start_utf16 + length_utf16 > tail_len) return false;
        field->sel_start = tail_start + tail_relative_start_utf16;
        field->sel_len = length_utf16;
        return true;
    }
    bool SelectAbsoluteRange(std::size_t abs_start_utf16,
                             std::size_t length_utf16) override {
        if (field == nullptr || select_fails) return false;
        ops->push_back("select");
        if (swallow_select) return true;  // 同上：S_OK 但不落地
        if (abs_start_utf16 + length_utf16 > field->doc.size()) return false;
        field->sel_start = abs_start_utf16;
        field->sel_len = length_utf16;
        return true;
    }
    // 读回当前选中文本；stale_reads > 0 时返回漂移旧值并递减——模拟 Chromium 系
    // 目标选区写经 renderer 异步生效（写后前 N 次读回仍是写入前旧选区内容）。
    bool ReadSelectedText(std::string* selected_text) override {
        if (selected_text == nullptr || field == nullptr) return false;
        ops->push_back("read");
        if (stale_reads > 0) {
            --stale_reads;
            *selected_text = "北";  // 选区写未在目标落地（读回 ≠ target）
            return true;
        }
        *selected_text = Utf8FromUtf16ForTest(
            std::wstring_view(field->doc).substr(field->sel_start, field->sel_len));
        return true;
    }

    bool MoveCaretTo(std::size_t tail_relative_utf16) override {
        if (field == nullptr) return false;
        constexpr std::size_t kTail = 512;
        const std::size_t tail_len = std::min(kTail, field->doc.size());
        const std::size_t tail_start = field->doc.size() - tail_len;
        // 尾删前置光标移动（批次 1）：记录 op 与终验末尾恢复区分（end/move）。
        ops->push_back(tail_start + tail_relative_utf16 >= field->doc.size() ? "end" : "move");
        if (move_fails) return false;
        field->sel_start = tail_start + tail_relative_utf16;
        field->sel_len = 0;
        return true;
    }
    bool MoveCaretToEnd() override {
        ops->push_back("end");
        if (field == nullptr) return false;
        field->sel_start = field->doc.size();
        field->sel_len = 0;
        return true;
    }
    bool MoveCaretToAbsolute(std::size_t abs_utf16) override {
        if (field == nullptr) return false;
        ops->push_back(abs_utf16 >= field->doc.size() ? "end" : "abs");
        if (move_fails) return false;
        field->sel_start = abs_utf16;
        field->sel_len = 0;
        return true;
    }
    VoiceTargetedFakeField* field = nullptr;
    std::vector<std::string>* ops = nullptr;
    bool move_fails = false;  // 批次 1：模拟 UIA 移动失败（caret_move_failed 路径）
    // 读回轮询脚本（对齐 macOS Chromium 异步生效语义）：stale_reads = 尚未落地的
    // 读回次数（每次读回返回旧值并递减）；select_fails = 模拟 UIA 写选区失败。
    int stale_reads = 0;
    bool select_fails = false;
    // swallow_select：SelectTailRange/SelectAbsoluteRange 返回 true 但不落位选区
    // （模拟 Chromium UIA「S_OK no-op」，键盘选区兜底用例的触发开关）。
    bool swallow_select = false;
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
            return;
        }
        if (field == nullptr) return;
        // 键盘选区兜底序列落位（真实编辑器语义：非 Shift 移动折叠光标并更新锚；
        // Shift 在锚基础上扩展/收缩选区）。
        if (spec.display_text == "Ctrl+End") {
            field->sel_start = field->doc.size() - (keyboard_drift ? 1 : 0);
            field->sel_len = 0;
            sel_anchor_ = field->sel_start;
        } else if (spec.display_text == "Ctrl+Home") {
            field->sel_start = 0;
            field->sel_len = 0;
            sel_anchor_ = 0;
        } else if (spec.display_text == "Left") {
            if (field->sel_len > 0) {
                field->sel_len = 0;  // 折叠到选区起点
            } else if (field->sel_start > 0) {
                field->sel_start -= 1;
            }
            sel_anchor_ = field->sel_start;
        } else if (spec.display_text == "Right") {
            if (field->sel_len > 0) {
                field->sel_start += field->sel_len;  // 折叠到选区终点
                field->sel_len = 0;
            } else if (field->sel_start < field->doc.size()) {
                field->sel_start += 1;
            }
            sel_anchor_ = field->sel_start;
        } else if (spec.display_text == "Shift+Left") {
            if (field->sel_len == 0) {
                if (field->sel_start > 0) {
                    field->sel_start -= 1;
                    field->sel_len = 1;
                }
            } else if (field->sel_start == sel_anchor_) {
                field->sel_len -= 1;  // 向右选区收缩右端
            } else {
                field->sel_start -= 1;  // 向左选区扩大左端
                field->sel_len += 1;
            }
        } else if (spec.display_text == "Shift+Right") {
            if (field->sel_len == 0) {
                if (field->sel_start < field->doc.size()) {
                    field->sel_len = 1;  // 向右选 1（锚在左端）
                }
            } else if (field->sel_start == sel_anchor_) {
                field->sel_len += 1;  // 向右选区扩大右端
            } else {
                field->sel_start += 1;  // 向左选区收缩左端
                field->sel_len -= 1;
            }
        }
    }
    VoiceTargetedFakeField* field = nullptr;
    std::vector<std::string>* ops = nullptr;
    bool swallow_backspace = false;
    // keyboard_drift：Ctrl+End 锚点定位偏 1——模拟兜底键盘序列落位漂移
    // （grapheme 算术错位形态），读回验证必失败 → 兜底拒绝路径。
    bool keyboard_drift = false;
    std::size_t sel_anchor_ = 0;  // 最近一次非 Shift 光标位（Shift 选区锚）
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
    // 直通协调器 UpdateConfig（清注入记录 + 代际 +1）：测试里唯一能赶在 200ms
    // 终验窗口前使在途回调变 stale 的触发源（会话路径受 500ms 最短时长约束）。
    void UpdateConfig(AppConfig config) { coordinator_->UpdateConfig(std::move(config)); }

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


// 批次 1（字段内定位）T9：尾删前置光标移动——光标在段中 → MoveCaretTo(anchorEnd)
// 轮询确认后退格；移动失败 → caret_move_failed 零退格、记录保留。
// TODO(win): 待 Windows 机编译+CTest 验证（与 macOS CoordinatorFsmTests T9 同语义）。
void TestVoiceEditCoordinatorCaretMove() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("你好世界", 171));
    // 光标移到段中（1）→ P1c 锚 anchor_end=4 → move(4) → 退格 1 次删「界」。
    harness.field().sel_start = 1;
    harness.field().sel_len = 0;
    harness.ops().clear();
    assert(harness.RunCommand("删除", 172));
    // Fake 的 MoveCaretTo 记录规则：目标==字段末尾归一化为 "end"（anchor_end=4
    // == doc.size()），故期望 {"end","backspace"}；终态光标 4 == anchorEnd（与
    // macOS CoordinatorFsmTests T9 断言的 caretUTF16==4 同语义）。
    assert((harness.ops() == std::vector<std::string>{"end", "backspace"}));
    assert(harness.field().doc == L"你好世");
    assert(harness.field().sel_start == 4 && harness.field().sel_len == 0);
    assert(harness.field().doc == L"你好世");
    assert(harness.input().sent_key_combos.size() == 1);

    // 移动失败（Fake editor 拒绝）→ caret_move_failed，零退格、记录保留。
    VoiceTargetedHarness h2(config);
    assert(h2.PasteText("你好世界", 173));
    h2.field().sel_start = 1;
    h2.field().sel_len = 0;
    h2.editor().move_fails = true;
    h2.ops().clear();
    assert(h2.RunCommand("删除", 174));
    assert(std::find(h2.ops().begin(), h2.ops().end(), "backspace") == h2.ops().end());
    assert(h2.input().sent_key_combos.empty());
    assert(h2.field().doc == L"你好世界");  // 字段未被改动
    assert(h2.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceEditCaretMoveFailed, h2.language()).c_str()));
    // 记录保留：解除移动失败后同命令重试直接生效（remaining 仍完整）。
    h2.editor().move_fails = false;
    h2.ops().clear();
    assert(h2.RunCommand("删除", 175));
    // 同上：anchor_end=4 == doc.size() → Fake 归一化记录 "end"。
    assert((h2.ops() == std::vector<std::string>{"end", "backspace"}));
    assert(h2.field().doc == L"你好世");
}

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
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace", "end"}));
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
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace", "end"}));
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

// 选区读回不符用例已由 TestTargetedCoordinatorReadbackPollExhausts 覆盖
// （2026-11-04 读回轮询改造：drift 恒漂移 = stale_reads 耗尽语义的特例）。

// 读回轮询收敛（Chromium 异步生效，对齐 macOS 780d1a2）：选区写经 renderer
// 异步落地，前 2 次读回仍是旧值，第 3 次读回命中后才执行动作——写一次选区、
// 轮询只追加读操作；原生目标（stale_reads=0）首次读即命中，零重试。
void TestTargetedCoordinatorReadbackPollConverges() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 221));
    harness.editor().stale_reads = 2;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 222));
    // 1 次写选区 + 3 次读回（前 2 次旧值）→ 命中 → 一次 Backspace → 终验恢复。
    assert((harness.ops() == std::vector<std::string>{"select", "read", "read", "read",
                                                      "backspace", "end"}));
    assert(harness.input().sent_key_combos.size() == 1);
    assert(harness.field().doc == L"我去上班");
    std::string expected = Tr(StringId::kVoiceTargetedDeleted, harness.language());
    const auto marker = expected.find("%d");
    expected.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == TargetedToast(expected.c_str()));
}

// 读回轮询耗尽：目标选区写彻底不生效（5 次读回全旧值）→ 零改动拒绝（零退格/
// 零粘贴、恢复末尾光标、TextChanged toast）；字段未被改动 → 记录保留，同命令
// 重试直接生效（安全模型：验证不过绝不盲改）。
void TestTargetedCoordinatorReadbackPollExhausts() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 231));
    harness.editor().stale_reads = 99;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 232));
    // UIA 写路径耗尽（5 次读回全旧值）→ 兜底读回仍漂移（stale 未耗尽）→ 失败：
    // 光标恢复改走 Ctrl+End 键盘（不再有 MoveCaretToEnd 的 "end" op）；兜底
    // 验证轮询同样 5 次读回（read×10）。
    assert((harness.ops() ==
            std::vector<std::string>{"select", "read", "read", "read", "read", "read",
                                     "read", "read", "read", "read", "read"}));
    assert(std::find(harness.input().sent_key_combos.begin(),
                     harness.input().sent_key_combos.end(),
                     "Ctrl+End") != harness.input().sent_key_combos.end());
    assert(harness.input().sent_key_combos.back() == "Ctrl+End");
    // 零动作语义（兜底发的是选区键盘序列）：无 Backspace、无粘贴。
    assert(std::find(harness.input().sent_key_combos.begin(),
                     harness.input().sent_key_combos.end(),
                     "Backspace") == harness.input().sent_key_combos.end());
    assert(harness.input().pasted_text == "我去北京上班");  // 命令轮零粘贴
    assert(harness.field().doc == L"我去北京上班");  // 零改动
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceTargetedTextChanged, harness.language()).c_str()));
    // 记录保留：轮询恢复（stale 清零）后同命令重试直接生效。
    harness.editor().stale_reads = 0;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 233));
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace", "end"}));
    assert(harness.field().doc == L"我去上班");
}

// 键盘选区兜底（Chromium UIA Select() 写不落地，2026-11-04 复测 got_len=0 实锤）：
// UIA 写路径读回耗尽 → 键盘序列选区 → 读回命中 → 执行动作（验证不过绝不盲改）。
void TestTargetedCoordinatorKeyboardFallbackSucceeds() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 261));
    harness.editor().swallow_select = true;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 262));
    // 「我去北京上班」G=6 target[2,4)：末尾锚 → Ctrl+End + Left×2（移到右端）+
    // Shift+Left×2（选中 [2,4)）→ 读回命中 → Backspace。fake 键盘落位使读回真实命中。
    const std::vector<std::string> expected_seq = {
        "Ctrl+End", "Left", "Left", "Shift+Left", "Shift+Left", "Backspace"};
    assert(harness.input().sent_key_combos.size() >= expected_seq.size());
    assert(std::equal(expected_seq.begin(), expected_seq.end(),
                      harness.input().sent_key_combos.begin()));
    assert(harness.field().doc == L"我去上班");
    std::string expected = Tr(StringId::kVoiceTargetedDeleted, harness.language());
    const auto marker = expected.find("%d");
    expected.replace(marker, 2, "2");
    assert(harness.ui().timed_messages.back() == TargetedToast(expected.c_str()));
    // 记录保留：swallow 解除后回归 UIA 原生路径（原生目标零回归底线）。
    harness.editor().swallow_select = false;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的上班", 263));
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace", "end"}));
    assert(harness.field().doc == L"我去");
}

// 兜底验证失败（锚点漂移形态）：键盘选区落位不符 → 读回耗尽 → Ctrl+End 恢复 +
// fieldMismatch；零改动、记录保留（安全模型：验证不过绝不盲改）。
void TestTargetedCoordinatorKeyboardFallbackVerifyFails() {
    AppConfig config = AppConfig::Defaults();
    config.refine_enabled = false;
    config.auto_enter = false;
    VoiceTargetedHarness harness(config);
    assert(harness.PasteText("我去北京上班", 271));
    harness.editor().swallow_select = true;
    harness.input().keyboard_drift = true;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 272));
    assert(std::find(harness.ops().begin(), harness.ops().end(), "backspace") ==
           harness.ops().end());
    assert(harness.field().doc == L"我去北京上班");  // 零改动
    assert(harness.ui().timed_messages.back() ==
           TargetedToast(Tr(StringId::kVoiceTargetedTextChanged, harness.language()).c_str()));
    // 记录保留：drift 关闭后同命令重试，兜底直接生效。
    harness.input().keyboard_drift = false;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 273));
    assert(harness.field().doc == L"我去上班");
}

// 终验不符（应用吞掉退格）：不重试、提示检查；动作未落地（字段仍为动作前
// 原样 → still_old）按 §17.2-7 2026-10-31 修订**记录保留**，允许同命令直接
// 重试（与 macOS 同构：VoiceStickCoordinator.swift "记录保留，允许同命令直接
// 重试"；Mac 侧批次 2 已按此语义更新对应用例「未落地重试 select(2,0)」）。
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
    // 记录保留（动作未落地）：swallow 关闭后同命令重试直接生效（批次 2 整框域
    // 绝对坐标路径：绝对选区 → 退格 → 光标平移回编辑点，对应 Mac「未落地重试
    // select(2,0)」同款语义）。
    harness.input().swallow_backspace = false;
    harness.ops().clear();
    assert(harness.RunCommand("删除刚才的北京", 163));
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace", "abs"}));
    assert(harness.field().doc == L"我去上班");
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
    // Windows 时序事实：会话最短 500ms（kMinimumRecordingDurationSeconds），下一次
    // 注入最早 +520ms 落地，必然晚于 +200ms 终验——macOS 测试「注入先于终验触发
    // stale」的时序在本平台经会话路径不可达。真实可赶在终验前的 stale 触发源是
    // UpdateConfig（清记录 + 代际 +1，macOS 同款语义），借它使在途终验失效。
    harness.UpdateConfig(config);
    // 新注入（覆盖记录、代际再 +1）。stale 终验被丢弃（光标未回末尾），
    // 直接重置 fake 字段为干净的新注入态。
    assert(harness.PasteText("全新输入", 183));
    harness.field().doc = L"全新输入";
    harness.field().sel_start = 4;
    harness.field().sel_len = 0;
    // 等过 200ms 终验窗口：stale 回调被丢弃（无 "end" op、无成功 toast）。
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    while (harness.PumpOnce()) {
    }
    assert((harness.ops() == std::vector<std::string>{"select", "read", "backspace"}));
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
    // notFound / 不可读（targeted 恒 verified，无 verify_mode 分支）。
    auto p10 = PlanVoiceTargeted(TargetedDelete("上海"), "我去北京上班", false, true,
                                 "我去北京上班", true, true);
    assert(!p10.ok && p10.failure == VoiceTargetedFailure::kNotFound);
    // 光标/选区不拦截（2026-10-31 放宽：双击核对目标词是常态动作）→ 计划成功。
    auto p11 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班", false, true);
    assert(p11.ok && p11.new_remaining_text == "我去上班");
    auto p12 = PlanVoiceTargeted(del, "我去北京上班", false, true, "我去北京上班", true, false);
    assert(p12.ok && p12.new_remaining_text == "我去上班");
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

// ======================= 批次 2（追问 + 整框）纯逻辑（双端同向量） =======================

// 位置限定扩展：第 N 个 / 倒数第 N 个（解析 + 选择 + 越界）。
void TestTargetedPositionParse() {
    ExpectTargetedCommand("删除刚才的第二个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kOrdinal, "第二个");
    assert(ParseVoiceTargetedCommand("删除刚才的第二个北京")->command.position_n == 2);
    ExpectTargetedCommand("删除刚才的第2个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kOrdinal, "第2个");
    assert(ParseVoiceTargetedCommand("删除刚才的第2个北京")->command.position_n == 2);
    ExpectTargetedCommand("删除刚才的第十一个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kOrdinal, "第十一个");
    assert(ParseVoiceTargetedCommand("删除刚才的第十一个北京")->command.position_n == 11);
    ExpectTargetedCommand("删除刚才的第二十三个北京", VoiceTargetedActionKind::kDelete, "北京",
                          "", VoiceTargetedPosition::kOrdinal, "二十三个");
    assert(ParseVoiceTargetedCommand("删除刚才的第二十三个北京")->command.position_n == 23);
    ExpectTargetedCommand("删除刚才的第二处北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kOrdinal, "第N处");
    ExpectTargetedCommand("删除刚才的倒数第二个北京", VoiceTargetedActionKind::kDelete, "北京",
                          "", VoiceTargetedPosition::kFromEnd, "倒数第二个");
    assert(ParseVoiceTargetedCommand("删除刚才的倒数第二个北京")->command.position_n == 2);
    ExpectTargetedCommand("删除刚才的倒数第3个北京", VoiceTargetedActionKind::kDelete, "北京",
                          "", VoiceTargetedPosition::kFromEnd, "倒数阿拉伯");
    ExpectTargetedCommand("删除刚才最后一个北京", VoiceTargetedActionKind::kDelete, "北京", "",
                          VoiceTargetedPosition::kLast, "最后一个（保留专词）");
    ExpectTargetedCommand("把刚才第二个北京改成上海", VoiceTargetedActionKind::kReplace, "北京",
                          "上海", VoiceTargetedPosition::kOrdinal, "把字句第二个");
    ExpectTargetedCommand("删除刚才的第一百个北京", VoiceTargetedActionKind::kDelete,
                          "第一百个北京", "", VoiceTargetedPosition::kUnspecified,
                          "第一百个不识别（进目标）");
    ExpectTargetedCommand("删除刚才的第100个北京", VoiceTargetedActionKind::kDelete,
                          "第100个北京", "", VoiceTargetedPosition::kUnspecified,
                          "阿拉伯 100 不识别（进目标）");
    ExpectTargetedCommand("删除刚才的第零个北京", VoiceTargetedActionKind::kDelete,
                          "第零个北京", "", VoiceTargetedPosition::kUnspecified, "零不识别");
    ExpectTargetedCommand("删除刚才的倒数第0个北京", VoiceTargetedActionKind::kDelete,
                          "倒数第0个北京", "", VoiceTargetedPosition::kUnspecified,
                          "倒数 0 不识别");
    // 计划：序数选择 + 越界（域内 3 处；TargetedDelete 不带 n，单独构造）。
    VoiceTargetedCommand ordinal2;
    ordinal2.action = VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}};
    ordinal2.target = "北京";
    ordinal2.position = VoiceTargetedPosition::kOrdinal;
    ordinal2.position_n = 2;
    auto second2 = PlanVoiceTargeted(ordinal2, "北京和北京再北京", false, true,
                                     "北京和北京再北京", true, true);
    assert(second2.ok && second2.occurrence.index == 1 &&
           second2.occurrence.utf16_start == 3 && second2.new_remaining_text == "北京和再北京");
    VoiceTargetedCommand fromEnd2 = ordinal2;
    fromEnd2.position = VoiceTargetedPosition::kFromEnd;
    auto fromEnd = PlanVoiceTargeted(fromEnd2, "北京和北京再北京", false, true,
                                     "北京和北京再北京", true, true);
    assert(fromEnd.ok && fromEnd.occurrence.index == 1);
    VoiceTargetedCommand ordinal4 = ordinal2;
    ordinal4.position_n = 4;
    auto oor = PlanVoiceTargeted(ordinal4, "北京和北京再北京", false, true,
                                 "北京和北京再北京", true, true);
    assert(!oor.ok && oor.failure == VoiceTargetedFailure::kPositionOutOfRange);
}

// 域解析全规则表 + 光标平移 + 整框映射（与 macOS VoiceTargetedDomainTests 同向量）。
void TestTargetedDomainResolve() {
    const std::string field = "手打开头北京和北京以及手打北京结尾";
    const auto recent = [](std::string_view remaining, std::size_t anchor_start) {
        return VoiceTargetedRecentDomain{
            std::string(remaining),
            VoiceEditAnchor{VoiceEditAnchor::Kind::kFieldEnd, anchor_start,
                            anchor_start + VoiceTargetedUtf16Length(remaining)}};
    };
    // 无显式作用域（自然简写语义；TargetedDelete 默认 has_explicit_scope=true）。
    const auto plainDelete = [](std::string_view target) {
        VoiceTargetedCommand cmd;
        cmd.action = VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}};
        cmd.target = std::string(target);
        cmd.has_explicit_scope = false;
        return cmd;
    };
    // Tier 1 命中。
    {
        const auto domain = recent("今天去了北京一趟", 6);
        auto out = ResolveVoiceTargetedDomain(plainDelete("北京"), &domain, field);
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kExecute &&
               out.domain == VoiceTargetedDomainKind::kLastInjection &&
               out.selected.utf16_start == 4);
    }
    // Tier 1 不唯一 → 整框追问。
    {
        const auto domain = recent("北京和北京", 4);
        auto out = ResolveVoiceTargetedDomain(plainDelete("北京"), &domain, field);
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kClarify &&
               out.domain == VoiceTargetedDomainKind::kWholeField && out.count == 3);
    }
    // 注入不可定位 → 整框唯一执行。
    {
        auto out = ResolveVoiceTargetedDomain(plainDelete("北京"), nullptr, "开头有北京结尾");
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kExecute &&
               out.domain == VoiceTargetedDomainKind::kWholeField &&
               out.selected.utf16_start == 3);
    }
    // 显式作用域：recent 可用 → 域内追问；目标不在域内 → not_found；不可用 → 整框。
    {
        const auto domain = recent("北京和北京", 4);
        VoiceTargetedCommand scoped = TargetedDelete("北京");
        scoped.has_explicit_scope = true;
        auto out = ResolveVoiceTargetedDomain(scoped, &domain, field);
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kClarify &&
               out.domain == VoiceTargetedDomainKind::kLastInjection && out.count == 2);
        const auto miss = recent("今天去了上海一趟", 6);
        auto out2 = ResolveVoiceTargetedDomain(scoped, &miss, "开头有北京结尾");
        assert(out2.kind == VoiceTargetedDomainOutcome::Kind::kFailure &&
               out2.failure == VoiceTargetedFailure::kNotFound);
        auto out3 = ResolveVoiceTargetedDomain(scoped, nullptr, "开头有北京结尾");
        assert(out3.kind == VoiceTargetedDomainOutcome::Kind::kExecute &&
               out3.domain == VoiceTargetedDomainKind::kWholeField);
    }
    // 无作用域 + 位置 → 整框按文档序（recent 可用也不启用 Tier 1）。
    {
        const auto domain = recent("今天去了北京一趟", 6);
        VoiceTargetedCommand ordinal2;
        ordinal2.action = VoiceTargetedAction{VoiceTargetedActionKind::kDelete, {}};
        ordinal2.target = "北京";
        ordinal2.position = VoiceTargetedPosition::kOrdinal;
        ordinal2.position_n = 2;
        ordinal2.has_explicit_scope = false;  // 无作用域 + 位置 → 恒整框
        auto out = ResolveVoiceTargetedDomain(ordinal2, &domain, field);
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kExecute &&
               out.domain == VoiceTargetedDomainKind::kWholeField &&
               out.selected.utf16_start == 7);
        // 越界。
        VoiceTargetedCommand ordinal5 = ordinal2;
        ordinal5.position_n = 5;
        auto oor = ResolveVoiceTargetedDomain(ordinal5, nullptr, field);
        assert(oor.kind == VoiceTargetedDomainOutcome::Kind::kOutOfRange && oor.count == 3);
    }
    // field_too_long（> 200,000 UTF-16）。
    {
        std::string huge;
        for (int i = 0; i < 200001; ++i) huge += "\xe5\xad\x97";  // UTF-8「字」×200001
        auto out = ResolveVoiceTargetedDomain(plainDelete("北京"), nullptr, huge);
        assert(out.kind == VoiceTargetedDomainOutcome::Kind::kFailure &&
               out.failure == VoiceTargetedFailure::kFieldTooLong);
        // Tier 1 命中不受限。
        const auto domain = recent("今天去了北京一趟", 6);
        auto tier1 = ResolveVoiceTargetedDomain(plainDelete("北京"), &domain, huge);
        assert(tier1.kind == VoiceTargetedDomainOutcome::Kind::kExecute &&
               tier1.domain == VoiceTargetedDomainKind::kLastInjection);
    }
}

void TestTargetedCursorRestore() {
    assert(VoiceTargetedRestoredCaret(2, 5, 3, 4) == 2);   // 区间前不变
    assert(VoiceTargetedRestoredCaret(5, 5, 3, 4) == 5);   // 恰在起点不变
    assert(VoiceTargetedRestoredCaret(8, 5, 3, 4) == 9);   // 区间后扩长
    assert(VoiceTargetedRestoredCaret(10, 5, 3, 0) == 7);  // 区间后收缩
    assert(VoiceTargetedRestoredCaret(8, 5, 3, 3) == 8);   // 等长替换不变
    assert(VoiceTargetedRestoredCaret(6, 5, 3, 4) == 9);   // 区间内→编辑后末尾
    assert(VoiceTargetedRestoredCaret(6, 5, 3, 0) == 5);   // 区间内→折叠点
    assert(VoiceTargetedRestoredCaret(5, 1, 2, 0) == 3);   // emoji 删除平移
    assert(VoiceTargetedRestoredCaret(6, 2, 2, 0) == 4);   // 末尾追加删除=批次 1
    assert(VoiceTargetedRestoredCaret(6, 2, 2, 2) == 6);   // 末尾追加等长=批次 1
    // 整框编辑 → remaining 映射（UTF-16 拼接）。
    assert(MapWholeFieldEditToRemaining("我去北京上班", 2, 2, "") == "我去上班");
    assert(MapWholeFieldEditToRemaining("我去北京上班", 2, 2, "上海") == "我去上海上班");
}

// 键盘选区兜底规划（grapheme 口径 + 双锚事件数取短；Chromium UIA Select() 写
// 不落地场景，对齐 macOS「AX 写彻底不可用目标的最后手段」）。
void TestKeyboardSelectionPlanner() {
    // 纯中文中段目标，两端事件数平局 → 取末尾锚。
    // 「我去北京上班」G=6，target[2,4)：from_end=1+2+2=5；from_home=1+2+2=5。
    {
        const auto plan = PlanKeyboardSelection("我去北京上班", 2, 4);
        assert(plan.ok && plan.from_end);
        assert(plan.plain_moves == 6 - 4);      // Left 移到目标右端 ge
        assert(plan.shifted_moves == 4 - 2);    // Shift+Left 选中 [gs,ge)
    }
    // 目标靠近末尾：末尾锚事件更少。「我去北京上班吃烤鸭」= 9 字 G=9，
    // target[6,9)「吃烤鸭」：from_end=1+0+3=4 < from_home=1+6+3=10。
    {
        const auto plan = PlanKeyboardSelection("我去北京上班吃烤鸭", 6, 9);
        assert(plan.ok && plan.from_end);
        assert(plan.plain_moves == 0);
        assert(plan.shifted_moves == (9 - 9) + 3);
    }
    // emoji 尾缀（surrogate 对 = 1 簇 2 码元）使末尾变贵 → 取首锚。
    // 「我去北京上班🎉」G=7，target[2,4)：from_end=1+3+2=6 > from_home=1+2+2=5。
    {
        const auto plan = PlanKeyboardSelection("我去北京上班\xF0\x9F\x8E\x89", 2, 4);
        assert(plan.ok && !plan.from_end);
        assert(plan.plain_moves == 2);
        assert(plan.shifted_moves == 2);
    }
    // CRLF 归一为 LF 的字段：\n 自成 1 簇。「你好\n世界」G=5，target[3,5)：
    // from_end=1+0+2=3 < from_home=1+3+2=6。
    {
        const auto plan = PlanKeyboardSelection("你好\n世界", 3, 5);
        assert(plan.ok && plan.from_end);
        assert(plan.plain_moves == 0);
        assert(plan.shifted_moves == 2);
    }
    // 区间越界（UTF-16 超全文长度）→ 拒绝。
    assert(!PlanKeyboardSelection("我去上班", 2, 99).ok);
    // 起点/终点落在 surrogate 对中间（非 grapheme 边界）→ 拒绝（选区算术会漂）。
    {
        const std::string emoji = "\xF0\x9F\x8E\x89\xE5\x8E\xBB";  // 🎉去（u16 长 3）
        assert(!PlanKeyboardSelection(emoji, 1, 3).ok);   // u16=1 在 🎉 内
        assert(PlanKeyboardSelection(emoji, 0, 3).ok);    // u16=3 = 全文末尾（合法边界）
        assert(PlanKeyboardSelection(emoji, 0, 2).ok);    // u16=2 = 🎉|去 簇边界
    }
    // 双端事件数都超上限（超长字段 + 居中目标）→ 不可行。
    {
        const std::string huge(5000, 'a');
        assert(!PlanKeyboardSelection(huge, 2500, 2501).ok);
    }
    // 空区间 / 倒序 → 拒绝。
    assert(!PlanKeyboardSelection("我去上班", 2, 2).ok);
    assert(!PlanKeyboardSelection("我去上班", 4, 2).ok);
}

// 序数应答解析（与 macOS VoiceEditOrdinalReplyTests 同向量）。
void TestVoiceEditOrdinalReply() {
    const auto parse = [](std::string_view text, std::string_view target) {
        return ParseVoiceEditOrdinalReply(text, target);
    };
    auto expect = [](std::string_view text, std::string_view target,
                     VoiceEditOrdinalReply::Kind kind, int n, const char* name) {
        const auto r = ParseVoiceEditOrdinalReply(text, target);
        if (!r || r->kind != kind || r->n != n) {
            std::printf("FAIL ordinal reply: %s\n", name);
            assert(false && "ordinal reply mismatch");
        }
    };
    auto expectNil = [](std::string_view text, std::string_view target, const char* name) {
        if (ParseVoiceEditOrdinalReply(text, target).has_value()) {
            std::printf("FAIL ordinal reply (expected nil): %s\n", name);
            assert(false && "ordinal reply should be nil");
        }
    };
    expect("第二个", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "第二个");
    expect("第2个", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "阿拉伯");
    expect("第十一", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 11, "裸第N");
    expect("第十一处", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 11, "第N处");
    expect("倒数第二个", "北京", VoiceEditOrdinalReply::Kind::kFromEnd, 2, "倒数第二");
    expect("最后一个", "北京", VoiceEditOrdinalReply::Kind::kFromEnd, 1, "最后一个");
    expect("最后那个", "北京", VoiceEditOrdinalReply::Kind::kFromEnd, 1, "最后那个（尾缀剥那个）");
    expect("第二处", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "第N处简写");
    expect("第二个北京", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "第N个+目标");
    expectNil("第二个上海", "北京", "目标不一致");
    expect("就第二个", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "前缀就");
    expect("就是第二个", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "前缀就是");
    expect("选第三个", "", VoiceEditOrdinalReply::Kind::kOrdinal, 3, "前缀选");
    expect("要2", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "前缀要+裸数字");
    expect("第二个吧", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "尾缀吧");
    expect("第二个。", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "尾部句号");
    expect("第二个就行", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "尾缀就行");
    expect("就第二个北京吧", "北京", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "前缀+目标+尾缀");
    expect("第２个", "", VoiceEditOrdinalReply::Kind::kOrdinal, 2, "全角数字");
    expect("五", "", VoiceEditOrdinalReply::Kind::kOrdinal, 5, "裸中文五");
    expect("二十", "", VoiceEditOrdinalReply::Kind::kOrdinal, 20, "裸二十（上限）");
    expect("3", "", VoiceEditOrdinalReply::Kind::kOrdinal, 3, "裸阿拉伯");
    expectNil("二十一", "", "裸二十一超限");
    expectNil("0", "", "裸 0");
    expect("取消", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "取消");
    expect("算了", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "算了");
    expect("算了吧", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "取消+尾缀");
    expect("不改了", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "不改了");
    expect("不用了", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "不用了");
    expect("不要了", "", VoiceEditOrdinalReply::Kind::kCancel, 0, "不要了");
    expectNil("你好", "", "普通文本");
    expectNil("把北京改成上海", "", "完整指令");
    expectNil("", "", "空串");
    (void)parse;
}

// 撤销短语白名单 + 尾删光标降级（与 macOS 同向量）。
void TestVoiceEditUndoPhraseAndFallback() {
    assert(IsVoiceEditUndoPhrase("撤销"));
    assert(IsVoiceEditUndoPhrase("撤回"));
    assert(IsVoiceEditUndoPhrase("撤销刚才的"));
    assert(IsVoiceEditUndoPhrase("撤回刚才的"));
    assert(IsVoiceEditUndoPhrase(" 撤销。"));
    assert(!IsVoiceEditUndoPhrase("删除刚才的"));
    assert(!IsVoiceEditUndoPhrase("清空"));
    assert(!IsVoiceEditUndoPhrase("撤销一下"));

    // 尾删光标降级：remaining「你好世界」在字段「你好世」锚定全 miss。
    VoiceEditCommand chars_cmd;
    chars_cmd.action = VoiceEditAction::kDeleteLastChars;
    chars_cmd.count = 2;
    chars_cmd.source = VoiceEditCommandSource::kExplicitCount;
    VoiceEditFieldState field;
    field.readable = true;
    field.selection_collapsed = true;
    field.tail_text = "你好世";
    field.tail_utf16_length = 3;
    field.caret_tail_utf16 = 3;
    auto fallback = PlanVoiceEdit("你好世界", false, chars_cmd, field,
                                  VoiceEditVerifyMode::kAuto, false, /*field_scope=*/true);
    assert(fallback.ok && fallback.caret_fallback && fallback.backspace_count == 2 &&
           fallback.anchor_end_utf16 == 3 && !fallback.needs_caret_move &&
           fallback.new_remaining_text == "你好世界" && fallback.new_pressed_enter == false);
    // clamp：N 超过光标前字数（光标 2、删 5）→ 只删 2。
    field.caret_tail_utf16 = 2;
    chars_cmd.count = 5;
    auto clamped = PlanVoiceEdit("你好世界", false, chars_cmd, field,
                                 VoiceEditVerifyMode::kAuto, false, true);
    assert(clamped.ok && clamped.backspace_count == 2);
    // deleteAll 不降级。
    VoiceEditCommand all_cmd;
    all_cmd.action = VoiceEditAction::kDeleteAll;
    auto all = PlanVoiceEdit("你好世界", false, all_cmd, field, VoiceEditVerifyMode::kAuto,
                             false, true);
    assert(!all.ok && all.reject_reason == VoiceEditRejectReason::kTextMismatch);
    // field_scope=false 不降级（批次 1 行为）。
    chars_cmd.count = 2;
    field.caret_tail_utf16 = 3;
    auto off = PlanVoiceEdit("你好世界", false, chars_cmd, field, VoiceEditVerifyMode::kAuto,
                             false, false);
    assert(!off.ok && off.reject_reason == VoiceEditRejectReason::kTextMismatch);
    // 选区非空不降级（selectionPresent 先拒绝）。
    field.selection_collapsed = false;
    auto selection = PlanVoiceEdit("你好世界", false, chars_cmd, field,
                                   VoiceEditVerifyMode::kAuto, false, true);
    assert(!selection.ok && selection.reject_reason == VoiceEditRejectReason::kSelectionActive);
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
    TestVoiceEditAnchorLocator();
    TestVoiceEditAnchorPlanner();
    TestTargetedAnchorPlanner();
    TestTargetedParserPositive();
    TestTargetedParserNegative();
    TestTargetedMatcher();
    TestTargetedPlanner();
    TestTargetedPositionParse();
    TestTargetedDomainResolve();
    TestTargetedCursorRestore();
    TestKeyboardSelectionPlanner();
    TestVoiceEditOrdinalReply();
    TestVoiceEditUndoPhraseAndFallback();
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
    TestTargetedCoordinatorReadbackPollConverges();
    TestTargetedCoordinatorReadbackPollExhausts();
    TestTargetedCoordinatorKeyboardFallbackSucceeds();
    TestTargetedCoordinatorKeyboardFallbackVerifyFails();
    TestTargetedCoordinatorFinalVerifyFailsInvalidates();
    TestTargetedCoordinatorOffModeUnreadableRejects();
    TestTargetedCoordinatorStaleGeneration();
    TestTargetedCoordinatorOldParserPriority();
    TestVoiceEditCoordinatorCaretMove();
    TestTargetedCoordinatorScopeAndInvalid();
    TestTargetedCoordinatorUnscopedReplace();
    TestTargetedCoordinatorLongDocLengtheningReplace();
    std::printf(">> voice edit + targeted coordinator integration tests done\n");
#endif
    std::printf(">> voice edit pure logic tests done\n");
}
