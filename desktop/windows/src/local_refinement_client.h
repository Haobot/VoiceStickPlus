#pragma once

#include "local_llm_engine.h"
#include "refine_history.h"

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
  // 此时 Refine 退化为纯规则层。system_prompt 为空时用 BuildSystemPrompt()
  // 内置 few-shot 默认（设置页「编辑提示词」语义：空 = 默认，非空 = 用户
  // 自定义 few-shot；改动经 SyncLocalRefiner 幂等键触发引擎重建，KV 前缀
  // 随新前缀重算）。log 可选诊断回调：每句记 in=（规则级输入）与归因行
  // （llm ok / guard blocked / llm fail / llm empty），协调器注入
  // LogCoordinatorLine 落 VoiceStickApp.log 供实测排查。
  explicit LocalRefinementClient(std::unique_ptr<LocalLlmEngine> engine,
                                 std::string system_prompt = {},
                                 std::function<void(std::string_view)> log = {});

  ~LocalRefinementClient();

  LocalRefinementClient(const LocalRefinementClient&) = delete;
  LocalRefinementClient& operator=(const LocalRefinementClient&) = delete;

  // few-shot system prompt（spike 定稿版，校准依据 m0/refine/report_*.md；
  // 示例与真实 SenseVoice 输出形态对齐，勿随意改动——0.6B/1.7B 对指令式
  // prompt 均无法跟随，few-shot 是硬要求）。
  static std::string BuildSystemPrompt();

  // 跨轮纠正指令 system prompt（M0 spike C 组定稿，4B GO 口径，勿随意改：
  // 模型只输出「错词→纠正词 / 待删片段」指令行，执行在
  // ApplyPinyinCorrections 代码层守卫下，安全性不依赖模型）。
  static std::string BuildCorrectionSystemPrompt();

  // 纯函数：剥离模型输出可能残留的模板碎屑（“输入：”回显行、“输出：”
  // 前缀、think 标签），供守卫前清洗与单测复用。
  static std::string StripReplyTemplate(std::string_view reply);

  // 跨轮上下文（M2，方案 §3.2/3.3）：turns 非空时启用纠正指令管线——
  // system 用 BuildCorrectionSystemPrompt()，本轮 user 只含「输入：{规则级
  // 文本}」+「处理：」，历史经引擎 ChatSessionTurn 承载（真引擎 KV 续写，
  // 失效自愈重放；默认实现拼同构续写块「输入：…处理：…」）。turns 的
  // refined 同时构成守卫查找域（纠正词必须在上文出现过）；instruction
  // 是引擎历史重放的 assistant 侧（形态自洽，见 RefineTurn 注释）。
  struct RefineContext {
    std::vector<RefineTurn> turns;  // 各轮 {raw_asr, refined, instruction}
  };

  // 异步精修：on_token 流式增量（后台线程）；on_complete 恰好回调一次：
  // 精修成功给最终文本（守卫放行的 LLM 结果），失败/回退给规则级结果，
  // 取消给 (false, 规则级结果)。hotwords 仅用于守卫（不改 prompt，
  // 小模型热词段经 spike 评估暂不接入）。context.turns 非空走跨轮纠正
  // 指令管线，为空走现行 few-shot 生成管线。
  void Refine(std::string text,
              std::function<void(std::string)> on_token,
              std::function<void(bool, std::string)> on_complete,
              std::shared_ptr<std::atomic_bool> cancel = nullptr,
              std::vector<std::string> hotwords = {},
              RefineContext context = {});

  // 跨轮版完成回调多带当轮模型指令输出（M3 协调器存 RefineHistory 的
  // instruction 字段——KV 续写重放 assistant 侧需形态自洽，重放 refined
  // 文本实测 3 轮起模型漂移为文本输出）。非跨轮管线恒给空串。
  using RefineCompleteWithInstruction =
      std::function<void(bool, std::string, std::string)>;
  void Refine(std::string text,
              std::function<void(std::string)> on_token,
              RefineCompleteWithInstruction on_complete,
              std::shared_ptr<std::atomic_bool> cancel = nullptr,
              std::vector<std::string> hotwords = {},
              RefineContext context = {});

  bool IsReady() const { return engine_ && engine_->IsReady(); }

 private:
  void RunRefine(const std::string& text,
                 const std::function<void(std::string)>& on_token,
                 const RefineCompleteWithInstruction& on_complete,
                 const std::shared_ptr<std::atomic_bool>& cancel,
                 const std::vector<std::string>& hotwords,
                 const RefineContext& context);

  std::unique_ptr<LocalLlmEngine> engine_;
  std::string system_prompt_;
  std::function<void(std::string_view)> log_;
  std::mutex threads_mutex_;
  std::vector<std::thread> threads_;
};

} // namespace voicestick
