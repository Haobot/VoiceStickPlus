#include "local_refinement_client.h"

#include "pinyin_guard.h"
#include "selection_correction.h"
#include "text_refiner.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

LocalRefinementClient::LocalRefinementClient(std::unique_ptr<LocalLlmEngine> engine,
                                             std::string system_prompt,
                                             std::function<void(std::string_view)> log)
    : engine_(std::move(engine)),
      system_prompt_(system_prompt.empty() ? BuildSystemPrompt()
                                           : std::move(system_prompt)),
      log_(std::move(log)) {}

LocalRefinementClient::~LocalRefinementClient() {
    std::lock_guard lock(threads_mutex_);
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

std::string LocalRefinementClient::BuildSystemPrompt() {
    // few-shot 定稿（spike 校准：指令式 prompt 对 0.6B/1.7B 均无法跟随；
    // 示例覆盖句首语气词/连接口头语/叠字/中英混合/干净句照抄）。修改前
    // 先跑 m0/refine 评测集（Doc/Plan/local-text-refinement.md §2）。
    return
        "清理语音识别文本：删除无意义的填充词、连接口头语和口吃重复"
        "（嗯/啊/呃/哦/那个/这个/就是/然后/呢/吧/uh/um/you know/叠字），"
        "修正标点与多余空格，保留原意、语种、专有名词与数字，不增删实义内容。"
        "只输出清理后的文本。\n"
        "\n"
        "输入：嗯，帮我把这个文件重命名一下。\n"
        "输出：帮我把这个文件重命名一下。\n"
        "\n"
        "输入：啊，那个，我们还是用那个 SenseVoice 吧。\n"
        "输出：我们还是用 SenseVoice 吧。\n"
        "\n"
        "输入：嗯，明天下午三点提醒我开会。\n"
        "输出：明天下午三点提醒我开会。\n"
        "\n"
        "输入：然后呢，我们下午再去那个超市吧。\n"
        "输出：我们下午再去超市吧。\n"
        "\n"
        "输入：你把那个设置页面里的 debug 那个开关打开就行了。\n"
        "输出：你把设置页面里的 debug 开关打开就行了。\n"
        "\n"
        "输入：行行行，我明白了。\n"
        "输出：行，我明白了。\n"
        "\n"
        "输入：我我我想去吃火锅。\n"
        "输出：我想去吃火锅。\n"
        "\n"
        "输入：嗯 呃 这个项目 嗯 用的是 BLE 连接\n"
        "输出：这个项目用的是 BLE 连接。\n"
        "\n"
        "输入：I think um we should uh use the model.\n"
        "输出：I think we should use the model.\n"
        "\n"
        "输入：帮我在 GitHub 上搜一下 llama.cpp 这个项目。\n"
        "输出：帮我在 GitHub 上搜一下 llama.cpp 这个项目。";
}

std::string LocalRefinementClient::BuildCorrectionSystemPrompt() {
    // M0 spike C 组定稿（4B GO 口径，m0/refine/run_cross_turn_spike.py
    // SYSTEM_PROMPT_CORRECT 逐字移植）：受限指令形态——模型只出建议，
    // 执行全在 ApplyPinyinCorrections 代码层守卫下。教学示例勿与线上
    // 高频真实案例雷同（spike C05 教学复读污染教训）。
    return
        "参考上文，找出语音识别文本中与上文词汇写法不一致的同音错字，"
        "以及无意义的填充词。只输出处理指令，每行一个：\n"
        "错词→纠正词（纠正词必须在上文或热词中出现过）；或直接原样摘录要删除的"
        "片段（含紧邻的逗号）。没有需要处理的内容时只输出：无。\n"
        "\n"
        "上文：我们刚才测了语气词过滤。\n"
        "输入：嗯，那些鱼器渍都被过滤掉了。\n"
        "处理：\n"
        "嗯，\n"
        "鱼器渍→语气词\n"
        "\n"
        "上文：帮我用蓝牙遥控器测试一下。\n"
        "输入：这个蓝崖遥控器手感不错。\n"
        "处理：\n"
        "蓝崖→蓝牙\n"
        "\n"
        "上文：明天三点开会别忘了。\n"
        "输入：那个会议改成三点办了。\n"
        "处理：\n"
        "那个\n"
        "办→半\n"
        "\n"
        "上文：优化一下这段流程。\n"
        "输入：这个算法流要重写。\n"
        "热词：算法流程\n"
        "处理：\n"
        "算法流→算法流程\n"
        "\n"
        "上文：帮我把垃圾倒一下。\n"
        "输入：帮我把垃圾倒一下。\n"
        "处理：\n"
        "无";
}

std::string LocalRefinementClient::StripReplyTemplate(std::string_view reply) {
    // std::regex 无 dotall 标志（ECMAScript 方言限制），用 [\s\S] 字符类
    // 等价表达“任意字符含换行”，让 think 块可跨行整体剥除。
    static const std::regex think_re("<think>[\\s\\S]*?</think>");
    static const std::regex think_tag_re("</?think>");
    std::string text(reply);
    text = std::regex_replace(text, think_re, "");
    text = std::regex_replace(text, think_tag_re, "");
    // 逐行剥离开头残留的模板行：“输入：…” 回显、“输出：” 前缀
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    lines.push_back(current);
    std::vector<std::string> kept;
    for (auto& line : lines) {
        // 去首尾空白
        std::size_t b = 0, e = line.size();
        while (b < e && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
        std::string trimmed = line.substr(b, e - b);
        if (trimmed.empty()) continue;
        if (trimmed.rfind("输入：", 0) == 0 || trimmed.rfind("Input:", 0) == 0) {
            continue;
        }
        if (trimmed.rfind("输出：", 0) == 0) {
            trimmed = trimmed.substr(9);  // “输出：”= 3 个 UTF-8 汉字共 9 字节
        } else if (trimmed.rfind("Output:", 0) == 0) {
            trimmed = trimmed.substr(7);
        }
        std::size_t b2 = 0, e2 = trimmed.size();
        while (b2 < e2 && std::isspace(static_cast<unsigned char>(trimmed[b2]))) ++b2;
        while (e2 > b2 && std::isspace(static_cast<unsigned char>(trimmed[e2 - 1]))) --e2;
        trimmed = trimmed.substr(b2, e2 - b2);
        if (!trimmed.empty()) kept.push_back(std::move(trimmed));
    }
    std::string out;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (i != 0) out.push_back('\n');
        out += kept[i];
    }
    return out;
}

void LocalRefinementClient::Refine(std::string text,
                                   std::function<void(std::string)> on_token,
                                   std::function<void(bool, std::string)> on_complete,
                                   std::shared_ptr<std::atomic_bool> cancel,
                                   std::vector<std::string> hotwords,
                                   RefineContext context) {
    // 兼容包装：丢弃当轮指令（单句管线/存量调用方不需要指令语义）
    Refine(std::move(text), std::move(on_token),
           [on_complete = std::move(on_complete)](bool ok, std::string s,
                                                  std::string) mutable {
               on_complete(ok, std::move(s));
           },
           std::move(cancel), std::move(hotwords), std::move(context));
}

void LocalRefinementClient::Refine(std::string text,
                                   std::function<void(std::string)> on_token,
                                   RefineCompleteWithInstruction on_complete,
                                   std::shared_ptr<std::atomic_bool> cancel,
                                   std::vector<std::string> hotwords,
                                   RefineContext context) {
    // 每句一个短命线程（推理数百毫秒级，量级=会话数，进程内可控）；
    // 析构 join 所有线程保证回调不悬垂。
    std::lock_guard lock(threads_mutex_);
    threads_.emplace_back(
        [this, text = std::move(text), on_token = std::move(on_token),
         on_complete = std::move(on_complete), cancel = std::move(cancel),
         hotwords = std::move(hotwords), context = std::move(context)]() mutable {
            RunRefine(text, on_token, on_complete, cancel, hotwords, context);
        });
}

void LocalRefinementClient::RunRefine(
    const std::string& text,
    const std::function<void(std::string)>& on_token,
    const RefineCompleteWithInstruction& on_complete,
    const std::shared_ptr<std::atomic_bool>& cancel,
    const std::vector<std::string>& hotwords,
    const RefineContext& context) {
    // L1 规则层（微秒级，总是执行）
    const std::string rule_refined = RuleRefineText(text);
    if (log_) log_("in='" + rule_refined + "'");

    const bool cancelled = cancel && cancel->load();
    if (cancelled || !engine_ || !engine_->IsReady()) {
        on_complete(!cancelled, rule_refined, "");
        return;
    }

    // 跨轮纠正指令管线（cross_turn 开关信号，M0 spike C 组形态）：本轮 user
    // 只含当句+处理锚，历史经 ChatSessionTurn 承载（真引擎 KV 续写，
    // FakeEngine 默认实现拼同构续写块——两种引擎形态行为等价，性能不同）；
    // 关闭走现行 few-shot 生成管线。
    const bool cross = context.cross_turn;
    std::vector<std::pair<std::string, std::string>> history;
    std::string context_text;  // 守卫查找域：各轮 refined 拼接
    for (const auto& turn : context.turns) {
        // 引擎历史 assistant 侧 = 当轮模型指令输出（形态自洽重放：重放
        // refined 文本实测 3 轮起模型漂移为文本输出，smoke 2026-09-10）。
        // 空指令轮次归一化为「无」（干净句的真实输出形态）。
        history.emplace_back(
            turn.raw_asr, turn.instruction.empty() ? "无" : turn.instruction);
        if (!context_text.empty()) context_text += "。";
        context_text += turn.refined;
    }
    std::string user_text;
    if (cross) {
        // 触类旁通（生成侧）：热词表进 4B 视野——跨轮上文从未出现正确
        // 写法时（ASR 每轮都错成「水池」），模型仍可往热词上出纠正指令；
        // 执行侧由守卫的近音子序列对齐放行少字变体。
        user_text = "输入：" + rule_refined;
        if (!hotwords.empty()) {
            user_text += "\n热词：";
            for (std::size_t i = 0; i < hotwords.size(); ++i) {
                if (i != 0) user_text += "，";
                user_text += hotwords[i];
            }
        }
        user_text += "\n处理：";
    } else {
        user_text = "输入：" + rule_refined + "\n输出：";
    }
    const std::string& sys_prompt = cross ? BuildCorrectionSystemPrompt()
                                          : system_prompt_;

    std::string raw;
    const auto chat_lambda = [&on_token, &cancel](std::string piece) {
        if (cancel && cancel->load()) return false;
        if (on_token) on_token(std::move(piece));
        return true;
    };
    bool ok = false;
    {
        // 引擎调用串行化（与 GenerateCandidates 共用实例；llama.cpp 非线程
        // 安全，快速连发语音本就应排队）
        std::lock_guard engine_lock(engine_mutex_);
        ok = cross ? engine_->ChatSessionTurn(sys_prompt, history, user_text,
                                              chat_lambda, raw)
                   : engine_->Chat(sys_prompt, user_text, chat_lambda, raw);
    }
    if (cancel && cancel->load()) {
        on_complete(false, rule_refined, "");
        return;
    }
    if (!ok) {
        if (log_) log_("llm fail -> rule");
        on_complete(true, rule_refined, "");  // 引擎失败：规则级兜底
        return;
    }

    const std::string stripped = StripReplyTemplate(raw);
    if (cross) {
        const auto outcome =
            ApplyPinyinCorrections(rule_refined, stripped, context_text, hotwords);
        // 热词保护：指令误删原文热词（中文热词可过删除守卫的字母数字闸）→ 回退
        for (const auto& hotword : hotwords) {
            if (rule_refined.find(hotword) != std::string::npos &&
                outcome.text.find(hotword) == std::string::npos) {
                if (log_) log_("hotword blocked '" + stripped + "' -> rule");
                on_complete(true, rule_refined, "");
                return;
            }
        }
        if (log_) {
            if (outcome.rejected.empty()) {
                log_(outcome.text == rule_refined
                         ? "correct none"
                         : "correct ok: '" + outcome.text + "'");
            } else {
                std::string why;
                for (const auto& rj : outcome.rejected) {
                    if (!why.empty()) why += "; ";
                    why += rj;
                }
                log_("correct partial (rejected: " + why + "): '" +
                     outcome.text + "'");
            }
        }
        // 指令语义归一：模型直答「无」与空输出在历史重放中同形（协调器
        // 原样存入 RefineTurn.instruction，client 读出时统一归一化）
        const std::string instruction =
            stripped.empty() ? "无" : stripped;
        on_complete(true, outcome.text, instruction);
        return;
    }

    if (!stripped.empty() && RefineResultSafe(rule_refined, stripped, hotwords)) {
        if (log_) log_("llm ok: '" + stripped + "'");
        on_complete(true, stripped, "");
        return;
    }
    // 空输出与守卫拦截都回退规则级，但归因不同（前者引擎/提示词问题，
    // 后者是守卫按设计拦下危险删除）。
    if (log_) {
        log_(stripped.empty() ? "llm empty -> rule"
                              : "guard blocked '" + stripped + "' -> rule");
    }
    on_complete(true, rule_refined, "");
}

void LocalRefinementClient::GenerateCandidates(const std::string& wrong_text,
                                               const std::string& context,
                                               const std::vector<std::string>& hotwords,
                                               CandidatesComplete on_done) {
    // 与 Refine 同款短命线程模型；析构 join 保证回调不悬垂。
    std::lock_guard lock(threads_mutex_);
    threads_.emplace_back(
        [this, wrong_text, context, hotwords,
         on_done = std::move(on_done)]() mutable {
            RunGenerateCandidates(wrong_text, context, hotwords, on_done);
        });
}

void LocalRefinementClient::RunGenerateCandidates(
    const std::string& wrong_text, const std::string& context,
    const std::vector<std::string>& hotwords,
    const CandidatesComplete& on_done) {
    if (!engine_ || !engine_->IsReady()) {
        on_done(false, {});
        return;
    }
    std::string raw;
    bool ok = false;
    {
        std::lock_guard engine_lock(engine_mutex_);
        ok = engine_->Chat(BuildCandidatesSystemPrompt(),
                           BuildCorrectionCandidatesPrompt(wrong_text, context,
                                                           hotwords),
                           nullptr, raw);
    }
    if (log_) log_("candidates in='" + wrong_text + "' out='" + raw + "'");
    if (!ok) {
        on_done(false, {});
        return;
    }
    on_done(true, FilterCandidates(
                      wrong_text,
                      ParseCandidateLines(StripReplyTemplate(raw))));
}

} // namespace voicestick
