#include "llama_cpp_engine.h"

#include <llama.h>

#include <cstdint>
#include <string>
#include <vector>

namespace voicestick {
namespace {

// ChatML（Qwen3 系）手拼模板：llama_chat_apply_template 不走 jinja，
// Qwen3 按官方 transformers 模板等价手拼；关闭 thinking 的标准做法是
// assistant 段起始注入空 think 块（enable_thinking=False 的模板行为）。
constexpr const char* kChatmlSystemHeader = "<|im_start|>system\n";
constexpr const char* kChatmlUserHeader = "<|im_end|>\n<|im_start|>user\n";
constexpr const char* kChatmlAssistantHeader =
    "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";

// 上下文预算：few-shot system 前缀(~800) + user(≤512) + 生成上限(≤256)
constexpr std::uint32_t kCtxTokens = 2048;

std::vector<llama_token> Tokenize(const llama_vocab* vocab, const std::string& text) {
    const auto need = llama_tokenize(vocab, text.c_str(),
                                     static_cast<int32_t>(text.size()),
                                     nullptr, 0, /*add_special=*/false,
                                     /*parse_special=*/true);
    // llama.cpp 约定：缓冲不足时返回负的所需长度（非错误）；0 才是空文本。
    if (need == 0) return {};
    const auto capacity = need < 0 ? -need : need;
    std::vector<llama_token> tokens(static_cast<std::size_t>(capacity));
    const auto n = llama_tokenize(vocab, text.c_str(),
                                  static_cast<int32_t>(text.size()),
                                  tokens.data(), static_cast<int32_t>(tokens.size()),
                                  false, true);
    if (n <= 0) return {};
    tokens.resize(static_cast<std::size_t>(n));
    return tokens;
}

// s 是否结束在完整 UTF-8 序列边界（无悬空前导字节），流式回调按边界切分
bool EndsOnUtf8Boundary(const std::string& s) {
    if (s.empty()) return true;
    std::size_t i = s.size() - 1;
    std::size_t cont = 0;
    while (true) {
        const auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80 || (b & 0xc0) == 0xc0) {
            std::size_t len = 1;
            if ((b & 0xe0) == 0xc0) len = 2;
            else if ((b & 0xf0) == 0xe0) len = 3;
            else if ((b & 0xf8) == 0xf0) len = 4;
            return cont < len;
        }
        ++cont;
        if (cont > 3 || i == 0) return false;
        --i;
    }
}

bool DecodeBatch(llama_context* ctx, const std::vector<llama_token>& tokens,
                 llama_pos pos_base) {
    llama_batch batch = llama_batch_init(static_cast<int32_t>(tokens.size()), 0, 1);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        batch.token[i] = tokens[i];
        batch.pos[i] = pos_base + static_cast<llama_pos>(i);
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = (i == tokens.size() - 1) ? 1 : 0;
    }
    batch.n_tokens = static_cast<int32_t>(tokens.size());
    const int rc = llama_decode(ctx, batch);
    llama_batch_free(batch);
    return rc == 0;
}

} // namespace

struct LlamaCppEngine::Impl {
    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const llama_vocab* vocab = nullptr;
    int max_gen_tokens = 256;
    std::string cached_prefix;
    std::size_t prefix_len = 0;  // 前缀 token 数（KV 复用切分点）

    // 会话续写状态（仅 ChatSessionTurn 维护；任何 Chat 调用使其作废）。
    // KV 只是缓存：失效即按调用方传入的 history 全量重建重放。
    bool session_active = false;
    std::size_t session_history_count = 0;  // 已编入 KV 的历史轮数
    llama_pos session_end_pos = 0;          // 会话 KV 末位置（本轮 decode 起点）

    ~Impl() {
        if (ctx) llama_free(ctx);
        if (model) llama_model_free(model);
    }
};

LlamaCppEngine::~LlamaCppEngine() = default;

std::unique_ptr<LlamaCppEngine> LlamaCppEngine::Create(const std::string& model_path,
                                                       int num_threads,
                                                       int max_gen_tokens) {
    llama_backend_init();  // CPU-only 幂等
    auto engine = std::unique_ptr<LlamaCppEngine>(new LlamaCppEngine());
    engine->impl_ = std::make_unique<Impl>();

    llama_model_params mp = llama_model_default_params();
    engine->impl_->model = llama_model_load_from_file(model_path.c_str(), mp);
    if (!engine->impl_->model) return nullptr;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kCtxTokens;
    cp.n_seq_max = 1;
    cp.n_threads = num_threads;
    cp.n_threads_batch = num_threads;
    engine->impl_->ctx = llama_init_from_model(engine->impl_->model, cp);
    if (!engine->impl_->ctx) return nullptr;

    engine->impl_->vocab = llama_model_get_vocab(engine->impl_->model);
    engine->impl_->max_gen_tokens = max_gen_tokens > 0 ? max_gen_tokens : 256;
    return engine;
}

bool LlamaCppEngine::IsReady() const {
    return impl_ && impl_->ctx;
}

bool LlamaCppEngine::Chat(const std::string& system_prompt,
                          const std::string& user_text,
                          const std::function<bool(const std::string&)>& on_token,
                          std::string& completion) {
    if (!impl_ || !impl_->ctx) return false;
    completion.clear();
    // Chat 是无会话语义：任何续写会话自此作废（重建路径会清全 KV）
    impl_->session_active = false;

    const std::string prefix =
        std::string(kChatmlSystemHeader) + system_prompt + kChatmlUserHeader;
    const auto memory = llama_get_memory(impl_->ctx);
    if (impl_->cached_prefix != prefix) {
        // 前缀变化（如未来接入热词段）：全量重建前缀 KV
        llama_memory_seq_rm(memory, 0, 0, -1);
        const auto prefix_tokens = Tokenize(impl_->vocab, prefix);
        if (prefix_tokens.empty()) return false;
        if (!DecodeBatch(impl_->ctx, prefix_tokens, 0)) return false;
        impl_->cached_prefix = prefix;
        impl_->prefix_len = prefix_tokens.size();
    } else {
        // 前缀命中：只清上一句的 user+生成 KV，保住前缀（复用关键路径）
        llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(impl_->prefix_len), -1);
    }

    const std::string suffix = user_text + kChatmlAssistantHeader;
    const auto user_tokens = Tokenize(impl_->vocab, suffix);
    if (user_tokens.empty()) return false;
    if (impl_->prefix_len + user_tokens.size() +
            static_cast<std::size_t>(impl_->max_gen_tokens) >
        kCtxTokens) {
        return false;  // 超上下文预算（异常长输入），交由调用方回退
    }
    if (!DecodeBatch(impl_->ctx, user_tokens,
                     static_cast<llama_pos>(impl_->prefix_len))) {
        return false;
    }

    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    std::string out;
    std::string pending;  // 流式 UTF-8 边界缓冲
    bool cancelled = false;
    const llama_pos gen_pos_base =
        static_cast<llama_pos>(impl_->prefix_len + user_tokens.size());
    // idx 语义（b10868 llama-context.cpp output_resolve_row）：负值 = 倒数第
    // |idx| 个 output 行。我们每次 decode 仅末 token 带 logits（唯一 output），
    // -1 恒指它；传非负 batch index 会按 token 位查（0 号位无 logits → fatal）。
    llama_token token = llama_sampler_sample(smpl, impl_->ctx, -1);
    for (int i = 0;
         i < impl_->max_gen_tokens && !llama_vocab_is_eog(impl_->vocab, token);
         ++i) {
        char buf[256];
        const int n = llama_token_to_piece(impl_->vocab, token, buf,
                                           static_cast<int32_t>(sizeof(buf)), 0,
                                           /*special=*/false);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (on_token) {
                pending.append(buf, static_cast<std::size_t>(n));
                if (EndsOnUtf8Boundary(pending)) {
                    if (!on_token(pending)) {
                        cancelled = true;
                        break;
                    }
                    pending.clear();
                }
            }
        }
        if (!DecodeBatch(impl_->ctx, {token}, gen_pos_base + i)) {
            llama_sampler_free(smpl);
            return false;
        }
        token = llama_sampler_sample(smpl, impl_->ctx, -1);
    }
    llama_sampler_free(smpl);
    if (cancelled) return false;
    if (on_token && !pending.empty()) on_token(pending);
    completion = std::move(out);
    return true;
}

void LlamaCppEngine::ResetLlmSession() {
    if (!impl_) return;
    impl_->session_active = false;
    impl_->session_history_count = 0;
    // KV 不动：下一次 ChatSessionTurn 检测 session_active=false 自行重建；
    // 单轮 Chat 仍可复用 cached_prefix。
}

bool LlamaCppEngine::ChatSessionTurn(
    const std::string& system_prompt,
    const std::vector<std::pair<std::string, std::string>>& history_turns,
    const std::string& user_text,
    const std::function<bool(const std::string&)>& on_token,
    std::string& completion) {
    if (!impl_ || !impl_->ctx) return false;
    completion.clear();

    const std::string prefix =
        std::string(kChatmlSystemHeader) + system_prompt + kChatmlUserHeader;
    const auto memory = llama_get_memory(impl_->ctx);

    const auto suffix_tokens =
        Tokenize(impl_->vocab, user_text + kChatmlAssistantHeader);
    if (suffix_tokens.empty()) return false;

    // 会话可用性：前缀一致 + 历史轮数一致（调用方滑窗/过期/TTL 清空、
    // 期间 Chat 干扰、上轮中止，都经此判定自愈重建）
    const bool usable = impl_->session_active &&
                        impl_->cached_prefix == prefix &&
                        impl_->session_history_count == history_turns.size();
    if (!usable) {
        llama_memory_seq_rm(memory, 0, 0, -1);
        const auto prefix_tokens = Tokenize(impl_->vocab, prefix);
        if (prefix_tokens.empty()) return false;
        if (!DecodeBatch(impl_->ctx, prefix_tokens, 0)) return false;
        impl_->cached_prefix = prefix;
        impl_->prefix_len = prefix_tokens.size();
        llama_pos pos = static_cast<llama_pos>(impl_->prefix_len);
        // 历史轮重放为「输入：raw\n处理：][refined」续例链：与正常轮
        // token 形态同构（assistant 侧用精修结果文本——指令执行后的正确
        // 形态，语义比当时的指令行更规范）；轮间 <|im_end|> 由下一轮
        // user_header 开头承担，与生成路径一致。
        for (const auto& turn : history_turns) {
            const auto replay = Tokenize(
                impl_->vocab,
                std::string(kChatmlUserHeader) + "输入：" + turn.first +
                    "\n处理：" + kChatmlAssistantHeader + turn.second);
            if (replay.empty() || !DecodeBatch(impl_->ctx, replay, pos)) {
                impl_->session_active = false;
                return false;
            }
            pos += static_cast<llama_pos>(replay.size());
        }
        impl_->session_history_count = history_turns.size();
        impl_->session_end_pos = pos;
    }

    if (static_cast<std::size_t>(impl_->session_end_pos) + suffix_tokens.size() +
            static_cast<std::size_t>(impl_->max_gen_tokens) >
        kCtxTokens) {
        // 重放后仍超预算（单轮异常长输入）：作废会话，交调用方回退
        impl_->session_active = false;
        return false;
    }
    if (!DecodeBatch(impl_->ctx, suffix_tokens, impl_->session_end_pos)) {
        impl_->session_active = false;
        return false;
    }

    auto sparams = llama_sampler_chain_default_params();
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    std::string out;
    std::string pending;  // 流式 UTF-8 边界缓冲
    bool cancelled = false;
    const llama_pos gen_pos_base =
        impl_->session_end_pos + static_cast<llama_pos>(suffix_tokens.size());
    llama_token token = llama_sampler_sample(smpl, impl_->ctx, -1);
    int generated = 0;
    for (int i = 0;
         i < impl_->max_gen_tokens && !llama_vocab_is_eog(impl_->vocab, token);
         ++i) {
        char buf[256];
        const int n = llama_token_to_piece(impl_->vocab, token, buf,
                                           static_cast<int32_t>(sizeof(buf)), 0,
                                           /*special=*/false);
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (on_token) {
                pending.append(buf, static_cast<std::size_t>(n));
                if (EndsOnUtf8Boundary(pending)) {
                    if (!on_token(pending)) {
                        cancelled = true;
                        break;
                    }
                    pending.clear();
                }
            }
        }
        if (!DecodeBatch(impl_->ctx, {token}, gen_pos_base + i)) {
            llama_sampler_free(smpl);
            impl_->session_active = false;
            return false;
        }
        ++generated;
        token = llama_sampler_sample(smpl, impl_->ctx, -1);
    }
    llama_sampler_free(smpl);
    if (cancelled) {
        // 中止轮次的 token 已进 KV，位置链不再可信：作废会话由下轮重建
        impl_->session_active = false;
        return false;
    }
    if (on_token && !pending.empty()) on_token(pending);

    // 会话推进：末位置 = 本轮 user 段 + 生成段；EOG 未 decode（下轮
    // user_header 的 <|im_end|> 补位，与重放路径一致）
    impl_->session_end_pos = gen_pos_base + static_cast<llama_pos>(generated);
    impl_->session_active = true;
    completion = std::move(out);
    return true;
}

} // namespace voicestick
