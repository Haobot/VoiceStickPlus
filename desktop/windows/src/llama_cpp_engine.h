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

 private:
  LlamaCppEngine() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace voicestick
