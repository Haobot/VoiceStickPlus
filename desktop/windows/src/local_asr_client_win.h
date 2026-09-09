// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地离线 ASR 客户端：sherpa-onnx SenseVoice-Small int8。
// 实现标准 AsrClient 接口——协调器经 asr_factory 切换本地/云端，音频管线零改动。
//
// 数据流：SendOggOpusChunk 攒 Ogg 字节流（与 BLE 路径同格式），worker 线程按
// 600ms 节流周期对"当前全量音频"增量 Opus 解码 + SenseVoice 滚动重解码，录音
// 期间持续触发 on_partial（空文本不上报）；is_last 时最终推理触发 on_final——
// 无新增样本则复用上次推理结果。回调在 worker 线程触发（与 AsrClientWin 的
// 网络线程语义一致，协调器侧回调已按异步设计）。

#ifndef VOICESTICK_LOCAL_ASR_CLIENT_WIN_H_
#define VOICESTICK_LOCAL_ASR_CLIENT_WIN_H_

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "voice_stick_coordinator.h"

namespace voicestick {

// 校验 SenseVoice 模型目录是否含 model.int8.onnx 与 tokens.txt：有效返回
// nullopt，无效返回中文错误描述（与 LocalAsrClient::Start 同口径，设置界面
// 即时反馈与启动校验共用，避免两处判定漂移）。
std::optional<std::string> ValidateSenseVoiceModelsDir(const std::string& models_dir);

// 解析 [local_asr] models_dir 配置值为实际目录：空 = exe_dir/models；相对
// 路径以 exe 目录为基准锚定；绝对路径原样返回。启动接线与设置界面状态检查
// 共用同一解析口径。
std::string ResolveLocalMicModelsDir(const std::string& configured,
                                     const std::string& exe_dir);

// 解析 [local_asr] refine_model 为实际 GGUF 路径（本地文本精修）：env
// VOICESTICK_REFINE_MODEL 最高优先（真机 smoke 注入用）；refine_model 非空时
// 绝对路径直用/相对路径锚 models_dir；空时探测默认档位
// Qwen3-1.7B-Q4_K_M/Qwen3-1.7B-Q4_K_M.gguf（spike 定案）。任何一步不存在
// 返回空串（调用方注入 nullptr 引擎退化为纯规则精修）。外壳装配与设置界面
// 状态检查共用同一口径。
std::string ResolveLocalRefineModelPath(const std::string& models_dir,
                                        const std::string& refine_model);

// SenseVoice 推理引擎抽象：生产实现包 sherpa-onnx C API；测试注入假引擎，
// 在无模型环境下驱动流式调度逻辑（TDD）。
class SenseVoiceEngine {
 public:
  virtual ~SenseVoiceEngine() = default;
  // 对整段 16kHz 单声道 PCM16 做一次离线推理，返回 UTF-8 文本。
  // 仅在 LocalAsrClient 的 worker 线程调用，实现无需自身加锁。
  virtual std::string Decode(std::span<const std::int16_t> samples) = 0;
};

class LocalAsrClient : public AsrClient {
 public:
  // models_dir: SenseVoice 模型目录（含 model.int8.onnx 与 tokens.txt）。
  // Start 时校验文件存在，缺失则失败并在 LastStartError 如实说明（不静默降级）。
  // num_threads: 推理线程数（SenseVoice int8 短句 2 线程足够）。
  // engine_override: 测试注入假引擎；非空时跳过模型目录校验与生产引擎创建。
  explicit LocalAsrClient(std::string models_dir, int num_threads = 2,
                          std::unique_ptr<SenseVoiceEngine> engine_override = nullptr);
  ~LocalAsrClient() override;

  LocalAsrClient(const LocalAsrClient&) = delete;
  LocalAsrClient& operator=(const LocalAsrClient&) = delete;

  bool Start(AsrSessionOptions options = {}) override;
  void SendOggOpusChunk(std::span<const std::uint8_t> data, bool is_last) override;
  void Cancel() override;
  std::string LastStartError() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voicestick

#endif  // VOICESTICK_LOCAL_ASR_CLIENT_WIN_H_
