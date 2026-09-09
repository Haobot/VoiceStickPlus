#pragma once

#include <functional>
#include <string>

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

  // 模型是否加载就绪（懒加载实现首次 Chat 前可能为 false）
  virtual bool IsReady() const = 0;
};

} // namespace voicestick
