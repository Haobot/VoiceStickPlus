#pragma once

#include "local_llm_engine.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace voicestick {

// 本地文本精修编排（本地识别会话专用）：L1 规则 → L2 本地 LLM → L3 守卫，
// 逐层失败逐层回退，最差不劣于规则级结果（Doc/Plan/local-text-refinement.md §3）。
// 回调均在后台线程触发（与云端 RefineStream 线程语义一致，调用方自行节流）。
class LocalRefinementClient {
 public:
  // engine 可注入（生产 LlamaCppEngine / 测试 FakeEngine）；空指针允许，
  // 此时 Refine 退化为纯规则层。
  explicit LocalRefinementClient(std::unique_ptr<LocalLlmEngine> engine);

  ~LocalRefinementClient();

  LocalRefinementClient(const LocalRefinementClient&) = delete;
  LocalRefinementClient& operator=(const LocalRefinementClient&) = delete;

  // few-shot system prompt（spike 定稿版，校准依据 m0/refine/report_*.md；
  // 示例与真实 SenseVoice 输出形态对齐，勿随意改动——0.6B/1.7B 对指令式
  // prompt 均无法跟随，few-shot 是硬要求）。
  static std::string BuildSystemPrompt();

  // 纯函数：剥离模型输出可能残留的模板碎屑（“输入：”回显行、“输出：”
  // 前缀、think 标签），供守卫前清洗与单测复用。
  static std::string StripReplyTemplate(std::string_view reply);

  // 异步精修：on_token 流式增量（后台线程）；on_complete 恰好回调一次：
  // 精修成功给最终文本（守卫放行的 LLM 结果），失败/回退给规则级结果，
  // 取消给 (false, 规则级结果)。hotwords 仅用于守卫（不改 prompt，
  // 小模型热词段经 spike 评估暂不接入）。
  void Refine(std::string text,
              std::function<void(std::string)> on_token,
              std::function<void(bool, std::string)> on_complete,
              std::shared_ptr<std::atomic_bool> cancel = nullptr,
              std::vector<std::string> hotwords = {});

  bool IsReady() const { return engine_ && engine_->IsReady(); }

 private:
  void RunRefine(const std::string& text,
                 const std::function<void(std::string)>& on_token,
                 const std::function<void(bool, std::string)>& on_complete,
                 const std::shared_ptr<std::atomic_bool>& cancel,
                 const std::vector<std::string>& hotwords);

  std::unique_ptr<LocalLlmEngine> engine_;
  std::mutex threads_mutex_;
  std::vector<std::thread> threads_;
};

} // namespace voicestick
