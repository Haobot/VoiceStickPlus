#include "local_refinement_client.h"

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

LocalRefinementClient::LocalRefinementClient(std::unique_ptr<LocalLlmEngine> engine)
    : engine_(std::move(engine)) {}

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
                                   std::vector<std::string> hotwords) {
    // 每句一个短命线程（推理数百毫秒级，量级=会话数，进程内可控）；
    // 析构 join 所有线程保证回调不悬垂。
    std::lock_guard lock(threads_mutex_);
    threads_.emplace_back(
        [this, text = std::move(text), on_token = std::move(on_token),
         on_complete = std::move(on_complete), cancel = std::move(cancel),
         hotwords = std::move(hotwords)]() mutable {
            RunRefine(text, on_token, on_complete, cancel, hotwords);
        });
}

void LocalRefinementClient::RunRefine(
    const std::string& text,
    const std::function<void(std::string)>& on_token,
    const std::function<void(bool, std::string)>& on_complete,
    const std::shared_ptr<std::atomic_bool>& cancel,
    const std::vector<std::string>& hotwords) {
    // L1 规则层（微秒级，总是执行）
    const std::string rule_refined = RuleRefineText(text);

    const bool cancelled = cancel && cancel->load();
    if (cancelled || !engine_ || !engine_->IsReady()) {
        on_complete(!cancelled, rule_refined);
        return;
    }

    std::string raw;
    const bool ok = engine_->Chat(
        BuildSystemPrompt(),
        "输入：" + rule_refined + "\n输出：",
        [&on_token, &cancel](std::string piece) {
            if (cancel && cancel->load()) return false;
            if (on_token) on_token(std::move(piece));
            return true;
        },
        raw);
    if (cancel && cancel->load()) {
        on_complete(false, rule_refined);
        return;
    }
    if (!ok) {
        on_complete(true, rule_refined);  // 引擎失败：规则级兜底
        return;
    }

    const std::string stripped = StripReplyTemplate(raw);
    if (!stripped.empty() && RefineResultSafe(rule_refined, stripped, hotwords)) {
        on_complete(true, stripped);
        return;
    }
    on_complete(true, rule_refined);  // 空/守卫拦截：规则级兜底
}

} // namespace voicestick
