#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace voicestick {

// 本地 LLM 推理引擎抽象（TDD 注入点，对称 SenseVoiceEngine 先例）。
// 约定：单实例串行调用（精修客户端保证一次一条）；on_token 在推理线程触发。
class LocalLlmEngine {
 public:
  virtual ~LocalLlmEngine() = default;

  // 单轮对话补全。system_prompt 鼓励跨调用恒定（实现缓存其 KV 前缀，
  // 变化时实现自行重建前缀）。on_token 逐段回调推理增量（可为 null，
  // 段以完整 UTF-8 边界切分）；其返回 false 时尽快中止并返回 false。
  // 成功返回 true 且 completion 为完整 assistant 文本（已剥模板标记）；
  // 失败/取消返回 false。
  virtual bool Chat(const std::string& system_prompt,
                    const std::string& user_text,
                    const std::function<bool(const std::string&)>& on_token,
                    std::string& completion) = 0;

  // 续写会话轮（跨轮纠错，方案 §3.2 KV 续写）：history_turns 为跨轮上文
  // （各轮 {ASR 原文, 当轮模型指令输出} 对，调用方维护滑窗/TTL），user_text
  // 为本轮新内容（不含历史）。真实现（LlamaCppEngine）把历史轮的
  // [user+生成] 留在 KV 续写；会话失效（前缀变化/历史轮数变动/预算
  // 不足/期间发生过 Chat）时按传入历史全量重建重放——KV 只是缓存，
  // history_turns 是唯一事实源。
  // 默认实现退化为无会话单轮：user 拼成与 KV 重放同构的续写块
  // （逐轮「输入：{原文}\n处理：{指令}」+ 当句），FakeEngine 等实现语义
  // 正确、只损失 KV 复用性能。
  virtual bool ChatSessionTurn(
      const std::string& system_prompt,
      const std::vector<std::pair<std::string, std::string>>& history_turns,
      const std::string& user_text,
      const std::function<bool(const std::string&)>& on_token,
      std::string& completion) {
      std::string user;
      for (const auto& turn : history_turns) {
          user += "输入：" + turn.first + "\n处理：" + turn.second + "\n";
      }
      user += user_text;
      return Chat(system_prompt, user, on_token, completion);
  }

  // 会话状态重置（历史过期/管线切换时协调器调用；幂等，无会话时 no-op）。
  virtual void ResetLlmSession() {}

  // 模型是否加载就绪（懒加载实现首次 Chat 前可能为 false）
  virtual bool IsReady() const = 0;
};

} // namespace voicestick
