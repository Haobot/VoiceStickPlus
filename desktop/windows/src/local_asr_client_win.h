// Copyright (c) 2026 Voice Stick contributors. All rights reserved.
//
// 本地离线 ASR 客户端（本机麦克风模式迭代一）：sherpa-onnx SenseVoice-Small int8。
// 实现标准 AsrClient 接口——协调器经 asr_factory 切换本地/云端，音频管线零改动。
//
// 数据流：SendOggOpusChunk 攒 Ogg 字节流（与 BLE 路径同格式），is_last 时 worker
// 线程做 ParseOggOpus → Opus 解码 PCM → SenseVoice 离线推理 → on_final 全文。
// 回调在 worker 线程触发（与 AsrClientWin 的网络线程语义一致，协调器侧
// on_final 已按异步回调设计）。

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

class LocalAsrClient : public AsrClient {
 public:
  // models_dir: SenseVoice 模型目录（含 model.int8.onnx 与 tokens.txt）。
  // Start 时校验文件存在，缺失则失败并在 LastStartError 如实说明（不静默降级）。
  // num_threads: 推理线程数（SenseVoice int8 短句 2 线程足够）。
  explicit LocalAsrClient(std::string models_dir, int num_threads = 2);
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
