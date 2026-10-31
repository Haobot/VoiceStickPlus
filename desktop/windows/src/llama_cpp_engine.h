#pragma once

#include "local_llm_engine.h"

#include <memory>
#include <string>

namespace voicestick {

// llama.cpp 生产引擎（Qwen3 系 ChatML 模板）：懒加载或构造即加载 GGUF、
// 贪心解码、system+few-shot 前缀 KV 跨调用复用（每句只 prefill user 部分，
// 实测 1.7B-Q4_K_M 净延迟 0.4~0.9s/句，见 Doc/Plan/local-text-refinement.md）。
// 构造失败（文件缺失/损坏）返回 nullptr；Chat 线程不安全，串行调用。
class LlamaCppEngine : public LocalLlmEngine {
 public:
  // model_path: GGUF 文件；num_threads: CPU 推理线程（默认 6，留余量给
  // SenseVoice/UI）；max_gen_tokens: 单次生成上限（精修场景 256 足够）。
  static std::unique_ptr<LlamaCppEngine> Create(const std::string& model_path,
                                                int num_threads = 6,
                                                int max_gen_tokens = 256);
  ~LlamaCppEngine() override;

  LlamaCppEngine(const LlamaCppEngine&) = delete;
  LlamaCppEngine& operator=(const LlamaCppEngine&) = delete;

  bool Chat(const std::string& system_prompt,
            const std::string& user_text,
            const std::function<bool(const std::string&)>& on_token,
            std::string& completion) override;
  bool IsReady() const override;

  // KV 续写会话轮（方案 §3.2）：历史轮按 {ASR 原文, 精修结果} 对重放进
  // KV（[输入：raw\n处理：][refined] 续例链），本轮 user_text 只含当句。
  // 会话失效（前缀变化/历史轮数变动/预算不足/期间发生过 Chat/上轮中止）
  // 时按传入历史全量重建——KV 是缓存，history_turns 是唯一事实源，
  // 历史过期或滑窗由调用方（RefineHistory）决定，引擎自愈跟随。
  // Chat 与本方法共享 KV：Chat 会使会话作废。
  bool ChatSessionTurn(
      const std::string& system_prompt,
      const std::vector<std::pair<std::string, std::string>>& history_turns,
      const std::string& user_text,
      const std::function<bool(const std::string&)>& on_token,
      std::string& completion) override;
  void ResetLlmSession() override;

 private:
  LlamaCppEngine() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace voicestick
